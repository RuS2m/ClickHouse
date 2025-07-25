#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/NumberTraits.h>
#include <Columns/ColumnString.h>

#include <Functions/DateTimeTransforms.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Functions/castTypeToEither.h>
#include <Functions/extractTimeZoneFromFunctionArguments.h>
#include <Functions/numLiteralChars.h>

#include <Interpreters/Context.h>
#include <Interpreters/castColumn.h>

#include <IO/WriteHelpers.h>

#include <Common/Concepts.h>
#include <Common/DateLUTImpl.h>
#include <base/find_symbols.h>
#include <Core/DecimalFunctions.h>
#include <Core/Settings.h>

#include <type_traits>
#include <concepts>


namespace DB
{
namespace Setting
{
    extern const SettingsBool formatdatetime_f_prints_scale_number_of_digits;
    extern const SettingsBool formatdatetime_f_prints_single_zero;
    extern const SettingsBool formatdatetime_format_without_leading_zeros;
    extern const SettingsBool formatdatetime_parsedatetime_m_is_month_name;
    extern const SettingsBool formatdatetime_e_with_space_padding;
}

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NOT_IMPLEMENTED;
    extern const int ILLEGAL_COLUMN;
    extern const int BAD_ARGUMENTS;
}

namespace
{
using Pos = const char *;

enum class SupportInteger : uint8_t
{
    Yes,
    No
};

enum class FormatSyntax : uint8_t
{
    MySQL,
    Joda
};

template <typename DataType> struct InstructionValueTypeMap {};
template <> struct InstructionValueTypeMap<DataTypeInt8>       { using InstructionValueType = UInt32; };
template <> struct InstructionValueTypeMap<DataTypeUInt8>      { using InstructionValueType = UInt32; };
template <> struct InstructionValueTypeMap<DataTypeInt16>      { using InstructionValueType = UInt32; };
template <> struct InstructionValueTypeMap<DataTypeUInt16>     { using InstructionValueType = UInt32; };
template <> struct InstructionValueTypeMap<DataTypeInt32>      { using InstructionValueType = UInt32; };
template <> struct InstructionValueTypeMap<DataTypeUInt32>     { using InstructionValueType = UInt32; };
template <> struct InstructionValueTypeMap<DataTypeInt64>      { using InstructionValueType = Int64; };
template <> struct InstructionValueTypeMap<DataTypeUInt64>     { using InstructionValueType = UInt64; };
template <> struct InstructionValueTypeMap<DataTypeDate>       { using InstructionValueType = UInt16; };
template <> struct InstructionValueTypeMap<DataTypeDate32>     { using InstructionValueType = Int32; };
template <> struct InstructionValueTypeMap<DataTypeDateTime>   { using InstructionValueType = UInt32; };
template <> struct InstructionValueTypeMap<DataTypeDateTime64> { using InstructionValueType = Int64; };

/// Cast value from integer to string, making sure digits number in result string is no less than total_digits by padding leading '0'.
String padValue(UInt32 val, size_t min_digits)
{
    String str = std::to_string(val);
    auto length = str.size();
    if (length >= min_digits)
        return str;

    String paddings(min_digits - length, '0');
    return str.insert(0, paddings);
}

constexpr std::string_view weekdaysFull[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

constexpr std::string_view weekdaysShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

constexpr std::string_view monthsFull[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

constexpr std::string_view monthsShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/** formatDateTime(time, 'format')
  * Performs formatting of time, according to provided format.
  *
  * This function is optimized with an assumption, that the resulting strings are fixed width.
  * (This assumption is fulfilled for currently supported formatting options).
  *
  * It is implemented in two steps.
  * At first step, it creates a template of zeros, literal characters, whitespaces, etc.
  *  and quickly fills resulting character array (string column) with this format.
  * At second step, it walks across the resulting character array and modifies/replaces specific characters,
  *  by calling some functions by pointers and shifting cursor by specified amount.
  *
  * Advantages:
  * - memcpy is mostly unrolled;
  * - low number of arithmetic ops due to pre-filled template;
  * - for somewhat reason, function by pointer call is faster than switch/case.
  *
  * Possible further optimization options:
  * - slightly interleave first and second step for better cache locality
  *   (but it has no sense when character array fits in L1d cache);
  * - avoid indirect function calls and inline functions with JIT compilation.
  *
  * Performance on Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz:
  *
  * WITH formatDateTime(now() + number, '%H:%i:%S') AS x SELECT count() FROM system.numbers WHERE NOT ignore(x);
  * - 97 million rows per second per core;
  *
  * WITH formatDateTime(toDateTime('2018-01-01 00:00:00') + number, '%F %T') AS x SELECT count() FROM system.numbers WHERE NOT ignore(x)
  * - 71 million rows per second per core;
  *
  * select count() from (select formatDateTime(t, '%m/%d/%Y %H:%i:%S') from (select toDateTime('2018-01-01 00:00:00')+number as t from numbers(100000000)));
  * - 53 million rows per second per core;
  *
  * select count() from (select formatDateTime(t, 'Hello %Y World') from (select toDateTime('2018-01-01 00:00:00')+number as t from numbers(100000000)));
  * - 138 million rows per second per core;
  *
  * PS. We can make this function to return FixedString. Currently it returns String.
  */
template <typename Name, SupportInteger support_integer, FormatSyntax format_syntax>
class FunctionFormatDateTimeImpl : public IFunction
{
private:
    /// Time is either UInt32 for DateTime or UInt16 for Date.
    template <typename F>
    static bool castType(const IDataType * type, F && f)
    {
        return castTypeToEither<
            DataTypeInt8,
            DataTypeUInt8,
            DataTypeInt16,
            DataTypeUInt16,
            DataTypeInt32,
            DataTypeUInt32,
            DataTypeInt64,
            DataTypeUInt64>(type, std::forward<F>(f));
    }

    template <typename Time>
    class Instruction
    {
    public:
        /// Joda format generally requires capturing extra variables (i.e. holding state) which is more convenient with
        /// std::function and std::bind. Unfortunately, std::function causes a performance degradation by 0.45x compared to raw function
        /// pointers. For MySQL format, we generally prefer raw function pointers. Because of the special case that not all formatters are
        /// fixed-width formatters (see mysqlLiteral instruction), we still need to be able to store state. For that reason, we use member
        /// function pointers instead of static function pointers.
        using FuncMysql = size_t (Instruction<Time>::*)(char *, Time, UInt64, UInt32, const DateLUTImpl &);
        FuncMysql func_mysql = nullptr;

        using FuncJoda = std::function<size_t(char *, Time, UInt64, UInt32, const DateLUTImpl &)>;
        FuncJoda func_joda = nullptr;

        /// extra_shift is only used in MySQL format syntax. It is always 0 in Joda format syntax.
        size_t extra_shift = 0;

        // Holds literal characters that will be copied into the output. Used by the mysqlLiteral instruction.
        String literal;

        Instruction() = default;

        void setMysqlFunc(FuncMysql && func) { func_mysql = std::move(func); }
        void setJodaFunc(FuncJoda && func) { func_joda = std::move(func); }
        void setLiteral(std::string_view literal_) { literal = literal_; }

        void perform(char *& dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            size_t shift = func_mysql
                           ? std::invoke(func_mysql, this, dest, source, fractional_second, scale, timezone)
                           : std::invoke(func_joda, dest, source, fractional_second, scale, timezone);
            dest += shift + extra_shift;
        }

    private:
        template <typename T>
        static size_t writeNumber2(char * p, T v)
        {
            memcpy(p, &digits100[v * 2], 2);
            return 2;
        }

        template <typename T>
        static size_t writeNumber3(char * p, T v)
        {
            writeNumber2(p, v / 10);
            p[2] = '0' + v % 10;
            return 3;
        }

        template <typename T>
        static size_t writeNumber4(char * p, T v)
        {
            writeNumber2(p, v / 100);
            writeNumber2(p + 2, v % 100);
            return 4;
        }

        /// Casts val from integer to string, then appends result string to buffer.
        /// Makes sure digits number in result string is no less than min_digits by padding leading '0'.
        /// Notice: '-' is not counted as digit.
        /// For example:
        /// val = -123, min_digits = 2 => dest = "-123"
        /// val = -123, min_digits = 3 => dest = "-123"
        /// val = -123, min_digits = 4 => dest = "-0123"
        static size_t writeNumberWithPadding(char * dest, std::integral auto val, size_t min_digits)
        {
            using T = decltype(val);
            using WeightType = typename NumberTraits::Construct<is_signed_v<T>, /*is_floating*/ false, sizeof(T)>::Type;
            WeightType w = 1;
            WeightType n = val;
            size_t digits = 0;
            while (n)
            {
                w *= 10;
                n /= 10;
                ++digits;
            }

            size_t pos = 0;
            n = val;

            /// Possible sign
            if constexpr (is_signed_v<T>)
                if (val < 0)
                {
                    n = (~n) + 1;
                    dest[pos] = '-';
                    ++pos;
                }

            /// Possible leading paddings
            if (min_digits > digits)
            {
                memset(dest, '0', min_digits - digits);
                pos += min_digits - digits;
            }

            /// Digits
            size_t digits_written = 0;
            while (w >= 100)
            {
                w /= 100;

                writeNumber2(dest + pos, n / w);
                pos += 2;
                digits_written += 2;
                n = n % w;
            }
            if (digits_written < digits)
            {
                dest[pos] = '0' + n;
                ++pos;
            }

            return pos;
        }
    public:
        size_t mysqlNoop(char *, Time, UInt64, UInt32, const DateLUTImpl &)
        {
            return 0;
        }

        size_t mysqlLiteral(char * dest, Time, UInt64, UInt32, const DateLUTImpl &)
        {
            memcpy(dest, literal.data(), literal.size());
            return literal.size();
        }

        size_t mysqlCentury(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto year = ToYearImpl::execute(source, timezone);
            auto century = year / 100;
            return writeNumber2(dest, century);
        }

        size_t mysqlDayOfMonth(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToDayOfMonthImpl::execute(source, timezone));
        }

        size_t mysqlAmericanDate(char * dest, Time source, const DateLUTImpl & timezone)
        {
            writeNumber2(dest, ToMonthImpl::execute(source, timezone));
            writeNumber2(dest + 3, ToDayOfMonthImpl::execute(source, timezone));
            writeNumber2(dest + 6, ToYearImpl::execute(source, timezone) % 100);
            return 8;
        }

        size_t mysqlAmericanDateWithoutSeparators(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return mysqlAmericanDate(dest, source, timezone);
        }

        size_t mysqlAmericanDateWithSeparators(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            size_t total_bytes = mysqlAmericanDate(dest, source, timezone);
            dest[2] = '/';
            dest[5] = '/';
            return total_bytes;
        }

        size_t mysqlDayOfMonthSpacePadded(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto day = ToDayOfMonthImpl::execute(source, timezone);
            if (day < 10)
                dest[1] = '0' + day;
            else
                writeNumber2(dest, day);
            return 2;
        }

        size_t mysqlISO8601Date(char * dest, Time source, const DateLUTImpl & timezone)
        {
            writeNumber4(dest, ToYearImpl::execute(source, timezone));
            writeNumber2(dest + 5, ToMonthImpl::execute(source, timezone));
            writeNumber2(dest + 8, ToDayOfMonthImpl::execute(source, timezone));
            return 10;
        }

        size_t mysqlISO8601DateWithoutSeparators(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return mysqlISO8601Date(dest, source, timezone);
        }

        size_t mysqlDayOfMonthWithSpacePadding(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto day = ToDayOfMonthImpl::execute(source, timezone);
            if (day < 10)
                dest[1] = '0' + day;
            else
                writeNumber2(dest, day);
            return 2;
        }

        size_t mysqlDayOfMonthWithoutSpacePadding(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto day = ToDayOfMonthImpl::execute(source, timezone);
            if (day < 10)
            {
                dest[0] = '0' + day;
                return 1;
            }
            writeNumber2(dest, day);
            return 2;
        }

        size_t mysqlISO8601DateWithSeparators(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            size_t total_bytes = mysqlISO8601Date(dest, source, timezone);
            dest[4] = '-';
            dest[7] = '-';
            return total_bytes;
        }

        size_t mysqlDayOfYear(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber3(dest, ToDayOfYearImpl::execute(source, timezone));
        }

        size_t mysqlMonth(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToMonthImpl::execute(source, timezone));
        }

        size_t mysqlMonthWithoutLeadingZero(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto month = ToMonthImpl::execute(source, timezone);
            if (month < 10)
            {
                dest[0] = '0' + month;
                return 1;
            }
            return writeNumber2(dest, month);
        }

        static size_t monthOfYearText(char * dest, Time source, bool abbreviate, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto month = ToMonthImpl::execute(source, timezone);
            std::string_view str_view = abbreviate ? monthsShort[month - 1] : monthsFull[month - 1];
            memcpy(dest, str_view.data(), str_view.size());
            return str_view.size();
        }

        size_t mysqlMonthOfYearTextShort(char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            return monthOfYearText(dest, source, true, fractional_second, scale, timezone);
        }

        size_t mysqlMonthOfYearTextLong(char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            return monthOfYearText(dest, source, false, fractional_second, scale, timezone);
        }

        size_t mysqlDayOfWeek(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            *dest = '0' + ToDayOfWeekImpl::execute(source, 0, timezone);
            return 1;
        }

        static size_t dayOfWeekText(char * dest, Time source, bool abbreviate, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto week_day = ToDayOfWeekImpl::execute(source, 0, timezone);
            if (week_day == 7)
                week_day = 0;

            std::string_view str_view = abbreviate ? weekdaysShort[week_day] : weekdaysFull[week_day];
            memcpy(dest, str_view.data(), str_view.size());
            return str_view.size();
        }

        size_t mysqlDayOfWeekTextShort(char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            return dayOfWeekText(dest, source, true, fractional_second, scale, timezone);
        }

        size_t mysqlDayOfWeekTextLong(char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            return dayOfWeekText(dest, source, false, fractional_second, scale, timezone);
        }

        size_t mysqlDayOfWeek0To6(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto day = ToDayOfWeekImpl::execute(source, 0, timezone);
            *dest = '0' + (day == 7 ? 0 : day);
            return 1;
        }

        size_t mysqlISO8601Week(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToISOWeekImpl::execute(source, timezone));
        }

        size_t mysqlISO8601Year2(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToISOYearImpl::execute(source, timezone) % 100);
        }

        size_t mysqlISO8601Year4(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber4(dest, ToISOYearImpl::execute(source, timezone));
        }

        size_t mysqlYear2(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToYearImpl::execute(source, timezone) % 100);
        }

        size_t mysqlYear4(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber4(dest, ToYearImpl::execute(source, timezone));
        }

        size_t mysqlHour24(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToHourImpl::execute(source, timezone));
        }

        size_t mysqlHour24WithoutLeadingZero(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone);
            if (hour < 10)
            {
                dest[0] = '0' + hour;
                return 1;
            }
            return writeNumber2(dest, hour);
        }

        size_t mysqlHour12(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone);
            hour = (hour == 0) ? 12 : (hour > 12 ? hour - 12 : hour);
            return writeNumber2(dest, hour);
        }

        size_t mysqlHour12WithoutLeadingZero(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone);
            hour = hour == 0 ? 12 : (hour > 12 ? hour - 12 : hour);
            if (hour < 10)
            {
                dest[0] = '0' + hour;
                return 1;
            }
            return writeNumber2(dest, hour);
        }

        size_t mysqlMinute(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToMinuteImpl::execute(source, timezone));
        }

        static size_t AMPM(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone) // NOLINT
        {
            auto hour = ToHourImpl::execute(source, timezone);
            dest[0] = hour >= 12 ? 'P' : 'A';
            dest[1] = 'M';
            return 2;
        }

        size_t mysqlAMPM(char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            return AMPM(dest, source, fractional_second, scale, timezone);
        }

        size_t mysqlHHMM24(char * dest, Time source, const DateLUTImpl & timezone)
        {
            writeNumber2(dest, ToHourImpl::execute(source, timezone));
            writeNumber2(dest + 3, ToMinuteImpl::execute(source, timezone));
            return 5;
        }

        size_t mysqlHHMM24WithoutSeparator(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return mysqlHHMM24(dest, source, timezone);
        }

        size_t mysqlHHMM24WithSeparator(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            size_t total_bytes = mysqlHHMM24(dest, source, timezone);
            dest[2] = ':';
            return total_bytes;
        }

        size_t mysqlHHMM12(char * dest, Time source, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone);
            writeNumber2(dest, hour == 0 ? 12 : (hour > 12 ? hour - 12 : hour));
            writeNumber2(dest + 3, ToMinuteImpl::execute(source, timezone));

            dest[6] = hour >= 12 ? 'P' : 'A';
            return 8;
        }

        size_t mysqlHHMM12WithoutSeparator(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return mysqlHHMM12(dest, source, timezone);
        }

        size_t mysqlHHMM12WithSeparator(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            size_t total_bytes = mysqlHHMM12(dest, source, timezone);
            dest[2] = ':';
            dest[5] = ' ';
            dest[7] = 'M';
            return total_bytes;
        }

        size_t mysqlSecond(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            return writeNumber2(dest, ToSecondImpl::execute(source, timezone));
        }

        /// Always prints six digits (as specified by MySQL documentation)
        size_t mysqlFractionalSecond(char * dest, Time /*source*/, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & /*timezone*/)
        {
            /// Truncate to buffer size == 6 if more than 6 digits are passed in.
            while (scale > 6)
            {
                --scale;
                fractional_second /= 10;
            }

            size_t writes = 0;
            for (UInt32 i = scale; i > 0; --i)
            {
                dest[i - 1] = '0' + (fractional_second % 10);
                fractional_second /= 10;
                ++writes;
            }

            for (UInt32 i = writes; i < 6; ++i)
                dest[i] = '0';

            return 6;
        }

        /// Prints the number of digits that were specified by the scale of the DateTime64. Legacy.
        size_t mysqlFractionalSecondScaleNumDigits(char * dest, Time /*source*/, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & /*timezone*/)
        {
            if (scale == 0)
                scale = 6;

            for (UInt32 i = scale; i > 0; --i)
            {
                dest[i - 1] += fractional_second % 10;
                fractional_second /= 10;
            }
            return scale;
        }

        /// Same as mysqlFractionalSecondScaleNumDigits but prints a single zero if the value has no fractional seconds. Legacy.
        size_t mysqlFractionalSecondSingleZero(char * dest, Time /*source*/, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & /*timezone*/)
        {
            if (scale == 0)
                scale = 1;

            for (UInt32 i = scale; i > 0; --i)
            {
                dest[i - 1] += fractional_second % 10;
                fractional_second /= 10;
            }
            return scale;
        }

        size_t mysqlISO8601Time(char * dest, Time source, const DateLUTImpl & timezone) // NOLINT
        {
            writeNumber2(dest, ToHourImpl::execute(source, timezone));
            writeNumber2(dest + 3, ToMinuteImpl::execute(source, timezone));
            writeNumber2(dest + 6, ToSecondImpl::execute(source, timezone));
            return 8;
        }

        size_t mysqlISO8601TimeWithoutSeparators(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone) // NOLINT
        {
            return mysqlISO8601Time(dest, source, timezone);
        }

        size_t mysqlISO8601TimeWithSeparators(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone) // NOLINT
        {
            size_t total_bytes = mysqlISO8601Time(dest, source, timezone);
            dest[2] = ':';
            dest[5] = ':';
            return total_bytes;
        }

        size_t mysqlTimezoneOffset(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto offset = TimezoneOffsetImpl::execute(source, timezone);
            if (offset < 0)
            {
                *dest = '-';
                offset = -offset;
            }
            else
            {
                *dest = '+';
            }

            writeNumber2(dest + 1, offset / 3600);
            writeNumber2(dest + 3, offset % 3600 / 60);
            return 5;
        }

        size_t mysqlQuarter(char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            *dest = '0' + ToQuarterImpl::execute(source, timezone);
            return 1;
        }

        template <typename Literal>
        static size_t jodaLiteral(const Literal & literal, char * dest, Time, UInt64, UInt32, const DateLUTImpl &)
        {
            memcpy(dest, literal.data(), literal.size());
            return literal.size();
        }

        static size_t jodaEra(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            Int32 year = static_cast<Int32>(ToYearImpl::execute(source, timezone));
            String res;
            if (min_represent_digits <= 3)
                res = year > 0 ? "AD" : "BC";
            else
                res = year > 0 ? "Anno Domini" : "Before Christ";

            memcpy(dest, res.data(), res.size());
            return res.size();
        }

        static size_t jodaCenturyOfEra(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto year = static_cast<Int32>(ToYearImpl::execute(source, timezone));
            year = (year < 0 ? -year : year);
            return writeNumberWithPadding(dest, year / 100, min_represent_digits);
        }

        static size_t jodaYearOfEra(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto year = static_cast<Int32>(ToYearImpl::execute(source, timezone));
            if (min_represent_digits == 2)
                return writeNumberWithPadding(dest, std::abs(year) % 100, 2);

            year = year <= 0 ? std::abs(year - 1) : year;
            return writeNumberWithPadding(dest, year, min_represent_digits);
        }

        static size_t jodaDayOfWeek1Based(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto week_day = ToDayOfWeekImpl::execute(source, 0, timezone);
            return writeNumberWithPadding(dest, week_day, min_represent_digits);
        }

        static size_t jodaDayOfWeekText(size_t min_represent_digits, char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            bool abbreviate = min_represent_digits <= 3;
            return dayOfWeekText(dest, source, abbreviate, fractional_second, scale, timezone);
        }

        static size_t jodaYear(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto year = static_cast<Int32>(ToYearImpl::execute(source, timezone));
            if (min_represent_digits == 2)
            {
                year = std::abs(year);
                auto two_digit_year = year % 100;
                return writeNumberWithPadding(dest, two_digit_year, 2);
            }
            return writeNumberWithPadding(dest, year, min_represent_digits);
        }

        static size_t jodaWeekYear(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto week_year = ToWeekYearImpl::execute(source, timezone);
            return writeNumberWithPadding(dest, week_year, min_represent_digits);
        }

        static size_t jodaWeekOfWeekYear(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto week_of_weekyear = ToWeekOfWeekYearImpl::execute(source, timezone);
            return writeNumberWithPadding(dest, week_of_weekyear, min_represent_digits);
        }

        static size_t jodaDayOfYear(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto day_of_year = ToDayOfYearImpl::execute(source, timezone);
            return writeNumberWithPadding(dest, day_of_year, min_represent_digits);
        }

        static size_t jodaMonthOfYear(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto month_of_year = ToMonthImpl::execute(source, timezone);
            return writeNumberWithPadding(dest, month_of_year, min_represent_digits);
        }

        static size_t jodaMonthOfYearText(size_t min_represent_digits, char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            bool abbreviate = min_represent_digits <= 3;
            return monthOfYearText(dest, source, abbreviate, fractional_second, scale, timezone);
        }

        static size_t jodaDayOfMonth(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto day_of_month = ToDayOfMonthImpl::execute(source, timezone);
            return writeNumberWithPadding(dest, day_of_month, min_represent_digits);
        }

        static size_t jodaHalfDayOfDay(
            size_t /*min_represent_digits*/, char * dest, Time source, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & timezone)
        {
            return AMPM(dest, source, fractional_second, scale, timezone);
        }

        static size_t jodaHourOfHalfDay(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone) % 12;
            return writeNumberWithPadding(dest, hour, min_represent_digits);
        }

        static size_t jodaClockHourOfHalfDay(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone) ;
            hour = (hour + 11) % 12 + 1;
            return writeNumberWithPadding(dest, hour, min_represent_digits);
        }

        static size_t jodaHourOfDay(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone) ;
            return writeNumberWithPadding(dest, hour, min_represent_digits);
        }

        static size_t jodaClockHourOfDay(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto hour = ToHourImpl::execute(source, timezone);
            hour = (hour + 23) % 24 + 1;
            return writeNumberWithPadding(dest, hour, min_represent_digits);
        }

        static size_t jodaMinuteOfHour(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto minute_of_hour = ToMinuteImpl::execute(source, timezone);
            return writeNumberWithPadding(dest, minute_of_hour, min_represent_digits);
        }

        static size_t jodaSecondOfMinute(size_t min_represent_digits, char * dest, Time source, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            auto second_of_minute = ToSecondImpl::execute(source, timezone);
            return writeNumberWithPadding(dest, second_of_minute, min_represent_digits);
        }

        static size_t jodaFractionOfSecond(size_t min_represent_digits, char * dest, Time /*source*/, UInt64 fractional_second, UInt32 scale, const DateLUTImpl & /*timezone*/)
        {
            min_represent_digits = std::min<size_t>(min_represent_digits, 9);
            if (fractional_second == 0)
            {
                for (UInt64 i = 0; i < min_represent_digits; ++i)
                    dest[i] = '0';
                return min_represent_digits;
            }
            auto str = toString(fractional_second);
            if (min_represent_digits > scale)
            {
                for (UInt64 i = 0; i < min_represent_digits - scale; ++i)
                    str += '0';
            }
            else if (min_represent_digits < scale)
            {
                str = str.substr(0, min_represent_digits);
            }
            memcpy(dest, str.data(), str.size());
            return min_represent_digits;
        }

        static size_t jodaTimezone(size_t min_represent_digits, char * dest, Time /*source*/, UInt64, UInt32, const DateLUTImpl & timezone)
        {
            if (min_represent_digits <= 3)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Short name time zone is not yet supported");

            auto str = timezone.getTimeZone();
            memcpy(dest, str.data(), str.size());
            return str.size();
        }
    };

    [[noreturn]] static void throwLastCharacterIsPercentException()
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "'%' must not be the last character in the format string, use '%%' instead");
    }

    static bool containsOnlyFixedWidthMySQLFormatters(std::string_view format, bool mysql_M_is_month_name, bool mysql_format_ckl_without_leading_zeros, bool mysql_e_with_space_padding)
    {
        static constexpr std::array variable_width_formatter = {'W'};
        static constexpr std::array variable_width_formatter_M_is_month_name = {'W', 'M'};
        static constexpr std::array variable_width_formatter_leading_zeros = {'c', 'l', 'k'};
        static constexpr std::array variable_width_formatter_e_with_space_padding = {'e'};

        for (size_t i = 0; i < format.size(); ++i)
        {
            switch (format[i])
            {
                case '%':
                    if (i + 1 >= format.size())
                        throwLastCharacterIsPercentException();
                    if (mysql_M_is_month_name)
                    {
                        if (std::any_of(
                                variable_width_formatter_M_is_month_name.begin(), variable_width_formatter_M_is_month_name.end(),
                                [&](char c){ return c == format[i + 1]; }))
                            return false;
                    }
                    if (mysql_format_ckl_without_leading_zeros)
                    {
                        if (std::any_of(
                                variable_width_formatter_leading_zeros.begin(), variable_width_formatter_leading_zeros.end(),
                                [&](char c){ return c == format[i + 1]; }))
                            return false;
                    }
                    if (!mysql_e_with_space_padding)
                    {
                        if (std::any_of(
                                variable_width_formatter_e_with_space_padding.begin(), variable_width_formatter_e_with_space_padding.end(),
                                [&](char c){ return c == format[i + 1]; }))
                            return false;
                    }
                    else
                    {
                        if (std::any_of(
                                variable_width_formatter.begin(), variable_width_formatter.end(),
                                [&](char c){ return c == format[i + 1]; }))
                            return false;
                    }
                    i += 1;
                    continue;
                default:
                    break;
            }
        }

        return true;
    }

    const bool mysql_M_is_month_name;
    const bool mysql_f_prints_single_zero;
    const bool mysql_f_prints_scale_number_of_digits;
    const bool mysql_format_ckl_without_leading_zeros;
    const bool mysql_e_with_space_padding;

public:
    static constexpr auto name = Name::name;

    static FunctionPtr create(ContextPtr context) { return std::make_shared<FunctionFormatDateTimeImpl>(context); }

    explicit FunctionFormatDateTimeImpl(ContextPtr context)
        : mysql_M_is_month_name(context->getSettingsRef()[Setting::formatdatetime_parsedatetime_m_is_month_name])
        , mysql_f_prints_single_zero(context->getSettingsRef()[Setting::formatdatetime_f_prints_single_zero])
        , mysql_f_prints_scale_number_of_digits(context->getSettingsRef()[Setting::formatdatetime_f_prints_scale_number_of_digits])
        , mysql_format_ckl_without_leading_zeros(context->getSettingsRef()[Setting::formatdatetime_format_without_leading_zeros])
        , mysql_e_with_space_padding(context->getSettingsRef()[Setting::formatdatetime_e_with_space_padding])
    {
    }

    String getName() const override
    {
        return name;
    }

    bool useDefaultImplementationForConstants() const override { return true; }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }

    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {1}; }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if constexpr (support_integer == SupportInteger::Yes)
        {
            if (arguments.size() != 1 && arguments.size() != 2 && arguments.size() != 3)
                throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                    "Number of arguments for function {} doesn't match: passed {}, should be 1, 2 or 3",
                    getName(), arguments.size());
            if (arguments.size() == 1 && !isInteger(arguments[0].type))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Illegal type {} of first argument of function {} when arguments size is 1. Should be integer",
                    arguments[0].type->getName(), getName());
            if (arguments.size() > 1 && !(isInteger(arguments[0].type) || isDate(arguments[0].type) || isDateTime(arguments[0].type) || isDate32(arguments[0].type) || isDateTime64(arguments[0].type)))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                                "Illegal type {} of first argument of function {} when arguments size is 2 or 3. "
                                "Should be a integer or a date with time",
                                arguments[0].type->getName(), getName());
        }
        else
        {
            if (arguments.size() != 2 && arguments.size() != 3)
                throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                    "Number of arguments for function {} doesn't match: passed {}, should be 2 or 3",
                    getName(), arguments.size());
            if (!isDate(arguments[0].type) && !isDateTime(arguments[0].type) && !isDate32(arguments[0].type) && !isDateTime64(arguments[0].type))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Illegal type {} of first argument of function {}. Should be a date or a date with time",
                    arguments[0].type->getName(), getName());
        }

        if (arguments.size() == 2 && !WhichDataType(arguments[1].type).isString())
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Illegal type {} of second argument of function {}. Must be String.",
                arguments[1].type->getName(), getName());

        if (arguments.size() == 3 && !WhichDataType(arguments[2].type).isString())
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Illegal type {} of third argument of function {}. Must be String.",
                arguments[2].type->getName(), getName());

        if (arguments.size() == 1)
            return std::make_shared<DataTypeDateTime>();
        return std::make_shared<DataTypeString>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        ColumnPtr res;
        if constexpr (support_integer == SupportInteger::Yes)
        {
            if (arguments.size() == 1)
            {
                return castColumn(arguments[0], result_type);
            }

            if (!castType(
                    arguments[0].type.get(),
                    [&](const auto & type)
                    {
                        using FromDataType = std::decay_t<decltype(type)>;
                        if (!(res = executeType<FromDataType>(arguments, result_type, input_rows_count)))
                            throw Exception(
                                ErrorCodes::ILLEGAL_COLUMN,
                                "Illegal column {} of function {}, must be Integer, Date, Date32, DateTime or DateTime64.",
                                arguments[0].column->getName(),
                                getName());
                        return true;
                    }))
            {
                if (!((res = executeType<DataTypeDate>(arguments, result_type, input_rows_count))
                      || (res = executeType<DataTypeDate32>(arguments, result_type, input_rows_count))
                      || (res = executeType<DataTypeDateTime>(arguments, result_type, input_rows_count))
                      || (res = executeType<DataTypeDateTime64>(arguments, result_type, input_rows_count))))
                    throw Exception(
                        ErrorCodes::ILLEGAL_COLUMN,
                        "Illegal column {} of function {}, must be Integer or DateTime.",
                        arguments[0].column->getName(),
                        getName());
            }
        }
        else
        {
            if (!((res = executeType<DataTypeDate>(arguments, result_type, input_rows_count))
                || (res = executeType<DataTypeDate32>(arguments, result_type, input_rows_count))
                || (res = executeType<DataTypeDateTime>(arguments, result_type, input_rows_count))
                || (res = executeType<DataTypeDateTime64>(arguments, result_type, input_rows_count))))
                throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                    "Illegal column {} of function {}, must be Date or DateTime.",
                    arguments[0].column->getName(), getName());
        }

        return res;
    }

    template <typename DataType>
    ColumnPtr executeType(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const
    {
        auto non_const_datetime = arguments[0].column->convertToFullColumnIfConst();
        auto * times = checkAndGetColumn<typename DataType::ColumnType>(non_const_datetime.get());
        if (!times)
            return nullptr;

        String format;
        if (const auto * format_column = checkAndGetColumnConst<ColumnString>(arguments[1].column.get()))
            format = format_column->getValue<String>();
        else
            throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                "Illegal column {} of second ('format') argument of function {}. Must be constant string.",
                arguments[1].column->getName(), getName());

        const ColumnConst * const_time_zone_column = nullptr;
        const DateLUTImpl * time_zone = nullptr;
        if (arguments.size() == 2)
            time_zone = &extractTimeZoneFromFunctionArguments(arguments, 2, 0);
        else if (arguments.size() > 2)
            const_time_zone_column = checkAndGetColumnConst<ColumnString>(arguments[2].column.get());

        UInt32 scale [[maybe_unused]] = 0;
        if constexpr (std::is_same_v<DataType, DataTypeDateTime64>)
            scale = times->getScale();

        /// For MySQL, we support two modes of execution:
        ///
        /// - All formatters in the format string are fixed-width. As a result, all output rows will have the same width and structure. We
        ///   take advantage of this and
        ///     1. create a "template" with placeholders from the format string,
        ///     2. allocate a result column large enough to store the template on each row,
        ///     3. copy the template into each result row,
        ///     4. run instructions which replace the formatter placeholders. All other parts of the template (e.g. whitespaces) are already
        ///        as desired and instructions skip over them (see 'extra_shift' in the formatters).
        ///
        /// - The format string contains at least one variable-width formatter. Output rows will potentially be of different size.
        ///   Steps 1. and 2. are performed as above (the result column is allocated based on a worst-case size estimation). The result
        ///   column rows are NOT populated with the template and left uninitialized. We run the normal instructions for formatters AND
        ///   instructions that copy literal characters before/between/after formatters. As a result, each byte of each result row is
        ///   written which is obviously slow.
        bool mysql_with_only_fixed_length_formatters = (format_syntax == FormatSyntax::MySQL)
            ? containsOnlyFixedWidthMySQLFormatters(format, mysql_M_is_month_name, mysql_format_ckl_without_leading_zeros, mysql_e_with_space_padding)
            : false;

        using T = typename InstructionValueTypeMap<DataType>::InstructionValueType;
        std::vector<Instruction<T>> instructions;
        String out_template;
        size_t out_template_size = parseFormat(format, instructions, scale, mysql_with_only_fixed_length_formatters, out_template);

        if (castType(arguments[0].type.get(), [&]([[maybe_unused]] const auto & type) { return true; }))
        {
            if (const_time_zone_column)
                time_zone = &extractTimeZoneFromFunctionArguments(arguments, 2, 0);
        }
        else if (std::is_same_v<DataType, DataTypeDateTime64> || std::is_same_v<DataType, DataTypeDateTime>)
        {
            if (const_time_zone_column)
                time_zone = &extractTimeZoneFromFunctionArguments(arguments, 2, 0);
        }
        else
            time_zone = &DateLUT::instance();

        auto col_res = ColumnString::create();
        auto & res_data = col_res->getChars();
        auto & res_offsets = col_res->getOffsets();
        res_data.resize(input_rows_count * (out_template_size + 1));
        res_offsets.resize(input_rows_count);

        if constexpr (format_syntax == FormatSyntax::MySQL)
        {
            if (mysql_with_only_fixed_length_formatters)
            {
                /// Fill result with template.
                {
                    const UInt8 * const begin = res_data.data();
                    const UInt8 * const end = res_data.data() + res_data.size();
                    UInt8 * pos = res_data.data();

                    if (pos < end)
                    {
                        memcpy(pos, out_template.data(), out_template_size + 1); /// With zero terminator. mystring[mystring.size()] = '\0' is guaranteed since C++11.
                        pos += out_template_size + 1;
                    }

                    /// Copy exponentially growing ranges.
                    while (pos < end)
                    {
                        size_t bytes_to_copy = std::min(pos - begin, end - pos);
                        memcpy(pos, begin, bytes_to_copy);
                        pos += bytes_to_copy;
                    }
                }
            }
        }

        const auto & vec = times->getData();

        auto * begin = reinterpret_cast<char *>(res_data.data());
        auto * pos = begin;
        for (size_t i = 0; i < input_rows_count; ++i)
        {
            if (!const_time_zone_column && arguments.size() > 2)
            {
                if (!arguments[2].column.get()->getDataAt(i).toString().empty())
                    time_zone = &DateLUT::instance(arguments[2].column.get()->getDataAt(i).toString());
                else
                    throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Provided time zone must be non-empty");
            }
            if constexpr (std::is_same_v<DataType, DataTypeDateTime64>)
            {
                auto c = DecimalUtils::split(vec[i], scale);

                // -1.123 splits to -1 /  0.123
                if (vec[i].value < 0 && c.fractional)
                {
                    using F = typename DataType::FieldType;
                    c.fractional = DecimalUtils::scaleMultiplier<F>(scale) + (c.whole ? F(-1) : F(1)) * c.fractional;
                    --c.whole;
                }

                for (auto & instruction : instructions)
                    instruction.perform(pos, static_cast<Int64>(c.whole), c.fractional, scale, *time_zone);
            }
            else
            {
                for (auto & instruction : instructions)
                    instruction.perform(pos, static_cast<T>(vec[i]), 0, 0, *time_zone);
            }
            *pos++ = '\0';

            res_offsets[i] = pos - begin;
        }

        res_data.resize(pos - begin);
        return col_res;
    }

    template <typename T>
    size_t parseFormat(const String & format, std::vector<Instruction<T>> & instructions, UInt32 scale, bool mysql_with_only_fixed_length_formatters, String & out_template) const
    {
        static_assert(format_syntax == FormatSyntax::MySQL || format_syntax == FormatSyntax::Joda);

        if constexpr (format_syntax == FormatSyntax::MySQL)
            return parseMySQLFormat(format, instructions, scale, mysql_with_only_fixed_length_formatters, out_template);
        else
            return parseJodaFormat(format, instructions, scale, mysql_with_only_fixed_length_formatters, out_template);
    }

    template <typename T>
    size_t parseMySQLFormat(const String & format, std::vector<Instruction<T>> & instructions, UInt32 scale, bool mysql_with_only_fixed_length_formatters, String & out_template) const
    {
        auto add_extra_shift = [&](size_t amount)
        {
            if (instructions.empty())
            {
                Instruction<T> instruction;
                instruction.setMysqlFunc(&Instruction<T>::mysqlNoop);
                instructions.push_back(std::move(instruction));
            }
            instructions.back().extra_shift += amount;
        };

        auto add_literal_instruction = [&](std::string_view literal)
        {
            Instruction<T> instruction;
            instruction.setMysqlFunc(&Instruction<T>::mysqlLiteral);
            instruction.setLiteral(literal);
            instructions.push_back(std::move(instruction));
        };

        auto add_extra_shift_or_literal_instruction = [&](std::string_view literal)
        {
            if (mysql_with_only_fixed_length_formatters)
                add_extra_shift(literal.size());
            else
                add_literal_instruction(literal);
        };

        auto add_time_instruction = [&]([[maybe_unused]] typename Instruction<T>::FuncMysql && func, [[maybe_unused]] std::string_view literal)
        {
            /// DateTime/DateTime64 --> insert instruction
            /// Other types cannot provide the requested data --> write out template
            if constexpr (is_any_of<T, UInt32, Int64, UInt64>)
            {
                Instruction<T> instruction;
                instruction.setMysqlFunc(std::move(func));
                instructions.push_back(std::move(instruction));
            }
            else
                add_extra_shift_or_literal_instruction(literal);
        };

        Pos pos = format.data();
        const Pos end = format.data() + format.size();

        while (true)
        {
            const Pos percent_pos = find_first_symbols<'%'>(pos, end);

            if (percent_pos < end)
            {
                if (pos < percent_pos)
                {
                    /// Handle characters before next %
                    add_extra_shift_or_literal_instruction(std::string_view(pos, percent_pos - pos));
                    out_template += String(pos, percent_pos - pos);
                }

                pos = percent_pos + 1;
                if (pos >= end)
                    throwLastCharacterIsPercentException();

                switch (*pos)
                {
                    // Abbreviated weekday [Mon-Sun]
                    case 'a':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfWeekTextShort);
                        instructions.push_back(std::move(instruction));
                        out_template += "Mon";
                        break;
                    }

                    // Abbreviated month [Jan-Dec]
                    case 'b':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlMonthOfYearTextShort);
                        instructions.push_back(std::move(instruction));
                        out_template += "Jan";
                        break;
                    }

                    // Month as a integer number:
                    // - if formatdatetime_format_without_leading_zeros = true: prints without leading zero, i.e. 1-12
                    // - otherwise: print with leading zeros: i.e. 01-12
                    case 'c':
                    {
                        Instruction<T> instruction;
                        if (mysql_format_ckl_without_leading_zeros)
                        {
                            instruction.setMysqlFunc(&Instruction<T>::mysqlMonthWithoutLeadingZero);
                            instructions.push_back(std::move(instruction));
                        }
                        else
                        {
                            instruction.setMysqlFunc(&Instruction<T>::mysqlMonth);
                            instructions.push_back(std::move(instruction));
                        }
                        out_template += "00";
                        break;
                    }

                    // Year, divided by 100, zero-padded
                    case 'C':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlCentury);
                        instructions.push_back(std::move(instruction));
                        out_template += "00";
                        break;
                    }

                    // Day of month, zero-padded (01-31)
                    case 'd':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfMonth);
                        instructions.push_back(std::move(instruction));
                        out_template += "00";
                        break;
                    }

                    // Short MM/DD/YY date, equivalent to %m/%d/%y
                    case 'D':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(mysql_with_only_fixed_length_formatters
                                                    ? &Instruction<T>::mysqlAmericanDateWithoutSeparators /// assumes that `/` is already there
                                                    : &Instruction<T>::mysqlAmericanDateWithSeparators);
                        instructions.push_back(std::move(instruction));
                        out_template += "00/00/00";
                        break;
                    }

                    // Day of month
                    case 'e':
                    {
                        if (mysql_e_with_space_padding)
                        {
                            /// Space-padded ( 1-31)
                            Instruction<T> instruction;
                            instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfMonthWithSpacePadding);
                            instructions.push_back(std::move(std::move(instruction)));
                            out_template += " 0";
                        }
                        else
                        {
                            /// Not space-padded (1-31)
                            Instruction<T> instruction;
                            instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfMonthWithoutSpacePadding);
                            instructions.push_back(std::move(std::move(instruction)));
                            out_template += "00";
                        }
                        break;
                    }

                    // Depending on a setting
                    // - Full month [January-December] OR
                    // - Minute of hour range [0, 59]
                    case 'M':
                    {
                        Instruction<T> instruction;
                        if (mysql_M_is_month_name)
                        {
                            instruction.setMysqlFunc(&Instruction<T>::mysqlMonthOfYearTextLong);
                            instructions.push_back(std::move(instruction));
                            out_template += "September"; /// longest possible month name
                        }
                        else
                        {
                            static constexpr std::string_view val = "00";
                            add_time_instruction(&Instruction<T>::mysqlMinute, val);
                            out_template += val;
                        }
                        break;
                    }

                    // Fractional seconds
                    case 'f':
                    {
                        if (mysql_f_prints_single_zero)
                        {
                            /// If the time data type has no fractional part, print '0' as fractional part.
                            /// Legacy behavior.
                            Instruction<T> instruction;
                            instruction.setMysqlFunc(&Instruction<T>::mysqlFractionalSecondSingleZero);
                            instructions.push_back(std::move(instruction));
                            out_template += String(scale == 0 ? 1 : scale, '0');
                        }
                        else
                        {
                            /// If the time data type has no fractional part, print '000000' as fractional part.
                            if (mysql_f_prints_scale_number_of_digits)
                            {
                                /// Print as many digits as specified by scale. Legacy behavior.
                                Instruction<T> instruction;
                                instruction.setMysqlFunc(&Instruction<T>::mysqlFractionalSecondScaleNumDigits);
                                instructions.push_back(std::move(instruction));
                                out_template += String(scale == 0 ? 6 : scale, '0');
                            }
                            else
                            {
                                /// Unconditionally print six digits (independent of scale).
                                /// This is what MySQL does, may it live long and prosper.
                                Instruction<T> instruction;
                                instruction.setMysqlFunc(&Instruction<T>::mysqlFractionalSecond);
                                instructions.push_back(std::move(instruction));
                                out_template += String(6, '0');
                            }
                        }
                        break;
                    }

                    // Short YYYY-MM-DD date, equivalent to %Y-%m-%d   2001-08-23
                    case 'F':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(mysql_with_only_fixed_length_formatters
                                                    ? &Instruction<T>::mysqlISO8601DateWithoutSeparators /// assumes that `-` is already there
                                                    : &Instruction<T>::mysqlISO8601DateWithSeparators);
                        instructions.push_back(std::move(instruction));
                        out_template += "0000-00-00";
                        break;
                    }

                    // Last two digits of year of ISO 8601 week number (see %G)
                    case 'g':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlISO8601Year2);
                        instructions.push_back(std::move(instruction));
                        out_template += "00";
                        break;
                    }

                    // Year of ISO 8601 week number (see %V)
                    case 'G':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlISO8601Year4);
                        instructions.push_back(std::move(instruction));
                        out_template += "0000";
                        break;
                    }

                    // Day of the year (001-366)   235
                    case 'j':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfYear);
                        instructions.push_back(std::move(instruction));
                        out_template += "000";
                        break;
                    }

                    // Month as a integer number (01-12)
                    case 'm':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlMonth);
                        instructions.push_back(std::move(instruction));
                        out_template += "00";
                        break;
                    }

                    // ISO 8601 weekday as number with Monday as 1 (1-7)
                    case 'u':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfWeek);
                        instructions.push_back(std::move(instruction));
                        out_template += "0";
                        break;
                    }

                    // ISO 8601 week number (01-53)
                    case 'V':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlISO8601Week);
                        instructions.push_back(std::move(instruction));
                        out_template += "00";
                        break;
                    }

                    // Weekday as a decimal number with Sunday as 0 (0-6)  4
                    case 'w':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfWeek0To6);
                        instructions.push_back(std::move(instruction));
                        out_template += "0";
                        break;
                    }

                    // Full weekday [Monday-Sunday]
                    case 'W':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlDayOfWeekTextLong);
                        instructions.push_back(std::move(instruction));
                        out_template += "Wednesday"; /// longest possible weekday name
                        break;
                    }

                    // Two digits year
                    case 'y':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlYear2);
                        instructions.push_back(std::move(instruction));
                        out_template += "00";
                        break;
                    }

                    // Four digits year
                    case 'Y':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlYear4);
                        instructions.push_back(std::move(instruction));
                        out_template += "0000";
                        break;
                    }

                    // Quarter (1-4)
                    case 'Q':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlQuarter);
                        instructions.push_back(std::move(instruction));
                        out_template += "0";
                        break;
                    }

                    // Offset from UTC timezone as +hhmm or -hhmm
                    case 'z':
                    {
                        Instruction<T> instruction;
                        instruction.setMysqlFunc(&Instruction<T>::mysqlTimezoneOffset);
                        instructions.push_back(std::move(instruction));
                        out_template += "+0000";
                        break;
                    }

                    /// Time components. If the argument is Date, not a DateTime, then this components will have default value.

                    // AM or PM
                    case 'p':
                    {
                        static constexpr std::string_view val = "AM";
                        add_time_instruction(&Instruction<T>::mysqlAMPM, val);
                        out_template += val;
                        break;
                    }

                    // 12-hour HH:MM time, equivalent to %h:%i %p 2:55 PM
                    case 'r':
                    {
                        static constexpr std::string_view val = "12:00 AM";
                        add_time_instruction(mysql_with_only_fixed_length_formatters
                                                    ? &Instruction<T>::mysqlHHMM12WithoutSeparator /// assumes that `:`, ` ` and `M` are already there
                                                    : &Instruction<T>::mysqlHHMM12WithSeparator,
                                             val);
                        out_template += val;
                        break;
                    }

                    // 24-hour HH:MM time, equivalent to %H:%i 14:55
                    case 'R':
                    {
                        static constexpr std::string_view val = "00:00";
                        add_time_instruction(mysql_with_only_fixed_length_formatters
                                                    ? &Instruction<T>::mysqlHHMM24WithoutSeparator /// assumes that ':' is already there
                                                    : &Instruction<T>::mysqlHHMM24WithSeparator,
                                                    val);
                        out_template += val;
                        break;
                    }

                    // Seconds
                    case 's':
                    {
                        static constexpr std::string_view val = "00";
                        add_time_instruction(&Instruction<T>::mysqlSecond, val);
                        out_template += val;
                        break;
                    }

                    // Seconds
                    case 'S':
                    {
                        static constexpr std::string_view val = "00";
                        add_time_instruction(&Instruction<T>::mysqlSecond, val);
                        out_template += val;
                        break;
                    }

                    // ISO 8601 time format (HH:MM:SS), equivalent to %H:%i:%S 14:55:02
                    case 'T':
                    {
                        static constexpr std::string_view val = "00:00:00";
                        add_time_instruction(mysql_with_only_fixed_length_formatters
                                                    ? &Instruction<T>::mysqlISO8601TimeWithoutSeparators /// assumes that `:` is already there
                                                    : &Instruction<T>::mysqlISO8601TimeWithSeparators,
                                                    val);
                        out_template += val;
                        break;
                    }

                    // Hour in 12h format (01-12)
                    case 'h':
                    {
                        static constexpr std::string_view val = "12";
                        add_time_instruction(&Instruction<T>::mysqlHour12, val);
                        out_template += val;
                        break;
                    }

                    // Hour in 24h format (00-23)
                    case 'H':
                    {
                        static constexpr std::string_view val = "00";
                        add_time_instruction(&Instruction<T>::mysqlHour24, val);
                        out_template += val;
                        break;
                    }

                    // Minute of hour range [0, 59]
                    case 'i':
                    {
                        static constexpr std::string_view val = "00";
                        add_time_instruction(&Instruction<T>::mysqlMinute, val);
                        out_template += val;
                        break;
                    }

                    // Hour in 12h format (01-12)
                    case 'I':
                    {
                        static constexpr std::string_view val = "12";
                        add_time_instruction(&Instruction<T>::mysqlHour12, val);
                        out_template += val;
                        break;
                    }

                    // Hour in 24h format:
                    // - if formatdatetime_format_without_leading_zeros = true: prints without leading zero, i.e. 0-23
                    // - otherwise: print with leading zeros: i.e. 00-23
                    case 'k':
                    {
                        static constexpr std::string_view val = "00";
                        if (mysql_format_ckl_without_leading_zeros)
                            add_time_instruction(&Instruction<T>::mysqlHour24WithoutLeadingZero, val);
                        else
                            add_time_instruction(&Instruction<T>::mysqlHour24, val);
                        out_template += val;
                        break;
                    }

                    // Hour in 12h format:
                    // - if formatdatetime_format_without_leading_zeros = true: prints without leading zero, i.e. 0-12
                    // - otherwise: print with leading zeros: i.e. 00-12
                    case 'l':
                    {
                        static constexpr std::string_view val = "12";
                        if (mysql_format_ckl_without_leading_zeros)
                            add_time_instruction(&Instruction<T>::mysqlHour12WithoutLeadingZero, val);
                        else
                            add_time_instruction(&Instruction<T>::mysqlHour12, val);
                        out_template += val;
                        break;
                    }

                    case 't':
                    {
                        static constexpr std::string_view val = "\t";
                        add_extra_shift_or_literal_instruction(val);
                        out_template += val;
                        break;
                    }

                    case 'n':
                    {
                        static constexpr std::string_view val = "\n";
                        add_extra_shift_or_literal_instruction(val);
                        out_template += val;
                        break;
                    }

                    // Escaped literal characters.
                    case '%':
                    {
                        static constexpr std::string_view val = "%";
                        add_extra_shift_or_literal_instruction(val);
                        out_template += val;
                        break;
                    }

                    // Unimplemented
                    case 'U':
                        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "format is not supported for WEEK (Sun-Sat)");
                    case 'v':
                        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "format is not supported for WEEK (Mon-Sun)");
                    case 'x':
                        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "format is not supported for YEAR for week (Mon-Sun)");
                    case 'X':
                        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "format is not supported for YEAR for week (Sun-Sat)");
                    default:
                        throw Exception(
                            ErrorCodes::BAD_ARGUMENTS,
                            "Incorrect syntax '{}', symbol is not supported '{}' for function {}",
                            format,
                            *pos,
                            getName());
                }
                ++pos;
            }
            else
            {
                /// Handle characters after last %
                add_extra_shift_or_literal_instruction(std::string_view(pos, end - pos));
                out_template += String(pos, end - pos);
                break;
            }
        }

        return out_template.size();
    }

    template <typename T>
    size_t parseJodaFormat(const String & format, std::vector<Instruction<T>> & instructions, UInt32, bool, String &) const
    {
        /// If the argument was DateTime, add instruction for printing. If it was date, just append default literal
        auto add_instruction = [&]([[maybe_unused]] typename Instruction<T>::FuncJoda && func, [[maybe_unused]] const String & default_literal)
        {
            if constexpr (is_any_of<T, UInt32, Int64, UInt64>)
            {
                Instruction<T> instruction;
                instruction.setJodaFunc(std::move(func));
                instructions.push_back(std::move(instruction));
            }
            else
            {
                Instruction<T> instruction;
                instruction.setJodaFunc(std::bind_front(&Instruction<T>::template jodaLiteral<String>, default_literal));
                instructions.push_back(std::move(instruction));
            }
        };

        size_t reserve_size = 0;
        Pos pos = format.data();
        Pos end = format.data() + format.size();
        while (pos < end)
        {
            Pos cur_token = pos;
            // Literal case
            if (*cur_token == '\'')
            {
                // Case 1: 2 consecutive single quote
                if (pos + 1 < end && *(pos + 1) == '\'')
                {
                    Instruction<T> instruction;
                    std::string_view literal(cur_token, 1);
                    instruction.setJodaFunc(std::bind_front(&Instruction<T>::template jodaLiteral<decltype(literal)>, literal));
                    instructions.push_back(std::move(instruction));
                    ++reserve_size;
                    pos += 2;
                }
                else
                {
                    // Case 2: find closing single quote
                    Int64 count = numLiteralChars(cur_token + 1, end);
                    if (count == -1)
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "No closing single quote for literal");

                    for (Int64 i = 1; i <= count; i++)
                    {
                        Instruction<T> instruction;
                        std::string_view literal(cur_token + i, 1);
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::template jodaLiteral<decltype(literal)>, literal));
                        instructions.push_back(std::move(instruction));
                        ++reserve_size;
                        if (*(cur_token + i) == '\'')
                            i += 1;
                    }
                    pos += count + 2;
                }
            }
            else
            {
                int repetitions = 1;
                ++pos;
                while (pos < end && *cur_token == *pos)
                {
                    ++repetitions;
                    ++pos;
                }
                switch (*cur_token)
                {
                    case 'G':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaEra, repetitions));
                        instructions.push_back(std::move(instruction));
                        reserve_size += repetitions <= 3 ? 2 : 13;
                        break;
                    }
                    case 'C':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaCenturyOfEra, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Year range [1900, 2299]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    }
                    case 'Y':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaYearOfEra, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Year range [1900, 2299]
                        reserve_size += repetitions == 2 ? 2 : std::max(repetitions, 4);
                        break;
                    }
                    case 'x':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaWeekYear, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// weekyear range [1900, 2299]
                        reserve_size += std::max(repetitions, 4);
                        break;
                    }
                    case 'w':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaWeekOfWeekYear, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Week of weekyear range [1, 52]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    }
                    case 'e':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaDayOfWeek1Based, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Day of week range [1, 7]
                        reserve_size += std::max(repetitions, 1);
                        break;
                    }
                    case 'E':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaDayOfWeekText, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Maximum length of short name is 3, maximum length of full name is 9.
                        reserve_size += repetitions <= 3 ? 3 : 9;
                        break;
                    }
                    case 'y':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaYear, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Year range [1900, 2299]
                        reserve_size += repetitions == 2 ? 2 : std::max(repetitions, 4);
                        break;
                    }
                    case 'D':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaDayOfYear, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Day of year range [1, 366]
                        reserve_size += std::max(repetitions, 3);
                        break;
                    }
                    case 'M':
                    {
                        if (repetitions <= 2)
                        {
                            Instruction<T> instruction;
                            instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaMonthOfYear, repetitions));
                            instructions.push_back(std::move(instruction));
                            /// Month of year range [1, 12]
                            reserve_size += 2;
                        }
                        else
                        {
                            Instruction<T> instruction;
                            instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaMonthOfYearText, repetitions));
                            instructions.push_back(std::move(instruction));
                            /// Maximum length of short name is 3, maximum length of full name is 9.
                            reserve_size += repetitions <= 3 ? 3 : 9;
                        }
                        break;
                    }
                    case 'd':
                    {
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaDayOfMonth, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Day of month range [1, 3]
                        reserve_size += std::max(repetitions, 3);
                        break;
                    }
                    case 'a':
                        /// Default half day of day is "AM"
                        add_instruction(std::bind_front(&Instruction<T>::jodaHalfDayOfDay, repetitions), "AM");
                        reserve_size += 2;
                        break;
                    case 'K':
                        /// Default hour of half day is 0
                        add_instruction(
                            std::bind_front(&Instruction<T>::jodaHourOfHalfDay, repetitions), padValue(0, repetitions));
                        /// Hour of half day range [0, 11]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    case 'h':
                        /// Default clock hour of half day is 12
                        add_instruction(
                            std::bind_front(&Instruction<T>::jodaClockHourOfHalfDay, repetitions),
                            padValue(12, repetitions));
                        /// Clock hour of half day range [1, 12]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    case 'H':
                        /// Default hour of day is 0
                        add_instruction(std::bind_front(&Instruction<T>::jodaHourOfDay, repetitions), padValue(0, repetitions));
                        /// Hour of day range [0, 23]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    case 'k':
                        /// Default clock hour of day is 24
                        add_instruction(std::bind_front(&Instruction<T>::jodaClockHourOfDay, repetitions), padValue(24, repetitions));
                        /// Clock hour of day range [1, 24]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    case 'm':
                        /// Default minute of hour is 0
                        add_instruction(std::bind_front(&Instruction<T>::jodaMinuteOfHour, repetitions), padValue(0, repetitions));
                        /// Minute of hour range [0, 59]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    case 's':
                        /// Default second of minute is 0
                        add_instruction(std::bind_front(&Instruction<T>::jodaSecondOfMinute, repetitions), padValue(0, repetitions));
                        /// Second of minute range [0, 59]
                        reserve_size += std::max(repetitions, 2);
                        break;
                    case 'S':
                    {
                        /// Default fraction of second is 0
                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaFractionOfSecond, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// 'S' repetitions range [0, 9]
                        reserve_size += repetitions <= 9 ? repetitions : 9;
                        break;
                    }
                    case 'z':
                    {
                        if (repetitions <= 3)
                            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Short name time zone is not yet supported");

                        Instruction<T> instruction;
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::jodaTimezone, repetitions));
                        instructions.push_back(std::move(instruction));
                        /// Longest length of full name of time zone is 32.
                        reserve_size += 32;
                        break;
                    }
                    case 'Z':
                        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "format is not supported for TIMEZONE_OFFSET_ID");
                    default:
                    {
                        if (isalpha(*cur_token))
                            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "format is not supported for {}", String(cur_token, repetitions));

                        Instruction<T> instruction;
                        std::string_view literal(cur_token, pos - cur_token);
                        instruction.setJodaFunc(std::bind_front(&Instruction<T>::template jodaLiteral<decltype(literal)>, literal));
                        instructions.push_back(std::move(instruction));
                        reserve_size += pos - cur_token;
                        break;
                    }
                }
            }
        }
        return reserve_size;
    }
};

struct NameFormatDateTime
{
    static constexpr auto name = "formatDateTime";
};

struct NameFromUnixTime
{
    static constexpr auto name = "fromUnixTimestamp";
};

struct NameFormatDateTimeInJodaSyntax
{
    static constexpr auto name = "formatDateTimeInJodaSyntax";
};

struct NameFromUnixTimeInJodaSyntax
{
    static constexpr auto name = "fromUnixTimestampInJodaSyntax";
};


using FunctionFormatDateTime = FunctionFormatDateTimeImpl<NameFormatDateTime, SupportInteger::No, FormatSyntax::MySQL>;
using FunctionFromUnixTimestamp = FunctionFormatDateTimeImpl<NameFromUnixTime, SupportInteger::Yes, FormatSyntax::MySQL>;
using FunctionFormatDateTimeInJodaSyntax = FunctionFormatDateTimeImpl<NameFormatDateTimeInJodaSyntax, SupportInteger::No, FormatSyntax::Joda>;
using FunctionFromUnixTimestampInJodaSyntax = FunctionFormatDateTimeImpl<NameFromUnixTimeInJodaSyntax, SupportInteger::Yes, FormatSyntax::Joda>;

}

REGISTER_FUNCTION(FormatDateTime)
{
    FunctionDocumentation::Description description_formatDateTime = R"(
Formats a date or date with time according to the given format string. `format` is a constant expression, so you cannot have multiple formats for a single result column.

`formatDateTime` uses MySQL datetime format style, refer to the [mysql docs](https://dev.mysql.com/doc/refman/8.0/en/date-and-time-functions.html#function_date-format).

The opposite operation of this function is [`parseDateTime`](/sql-reference/functions/type-conversion-functions#parsedatetime).

Using replacement fields, you can define a pattern for the resulting string.
The example column in the table below shows formatting result for `2018-01-02 22:33:44`.

**Replacement fields:**

| Placeholder | Description | Example |
|-------------|-------------|---------|
| %a | abbreviated weekday name (Mon-Sun) | Mon |
| %b | abbreviated month name (Jan-Dec) | Jan |
| %c | month as an integer number (01-12) | 01 |
| %C | year divided by 100 and truncated to integer (00-99) | 20 |
| %d | day of the month, zero-padded (01-31) | 02 |
| %D | Short MM/DD/YY date, equivalent to %m/%d/%y | 01/02/18 |
| %e | day of the month, space-padded (1-31) | 2 |
| %f | fractional second | 123456 |
| %F | short YYYY-MM-DD date, equivalent to %Y-%m-%d | 2018-01-02 |
| %g | two-digit year format, aligned to ISO 8601 | 18 |
| %G | four-digit year format for ISO week number | 2018 |
| %h | hour in 12h format (01-12) | 09 |
| %H | hour in 24h format (00-23) | 22 |
| %i | minute (00-59) | 33 |
| %I | hour in 12h format (01-12) | 10 |
| %j | day of the year (001-366) | 002 |
| %k | hour in 24h format (00-23) | 14 |
| %l | hour in 12h format (01-12) | 09 |
| %m | month as an integer number (01-12) | 01 |
| %M | full month name (January-December) | January |
| %n | new-line character | |
| %p | AM or PM designation | PM |
| %Q | Quarter (1-4) | 1 |
| %r | 12-hour HH:MM AM/PM time, equivalent to %h:%i %p | 10:30 PM |
| %R | 24-hour HH:MM time, equivalent to %H:%i | 22:33 |
| %s | second (00-59) | 44 |
| %S | second (00-59) | 44 |
| %t | horizontal-tab character | |
| %T | ISO 8601 time format (HH:MM:SS), equivalent to %H:%i:%S | 22:33:44 |
| %u | ISO 8601 weekday as number with Monday as 1 (1-7) | 2 |
| %V | ISO 8601 week number (01-53) | 01 |
| %w | weekday as a integer number with Sunday as 0 (0-6) | 2 |
| %W | full weekday name (Monday-Sunday) | Monday |
| %y | Year, last two digits (00-99) | 18 |
| %Y | Year | 2018 |
| %z | Time offset from UTC as +HHMM or -HHMM | -0500 |
| %% | a % sign | % |

- In ClickHouse versions earlier than v23.4, `%f` prints a single zero (0) if the formatted value is a Date, Date32 or DateTime (which have no fractional seconds) or a DateTime64 with a precision of 0.
- In ClickHouse versions earlier than v25.1, `%f` prints as many digits as specified by the scale of the DateTime64 instead of fixed 6 digits.
- In ClickHouse versions earlier than v23.4, `%M` prints the minute (00-59) instead of the full month name (January-December).
    )";
    FunctionDocumentation::Syntax syntax_formatDateTime = R"(
formatDateTime(datetime, format[, timezone])
    )";
    FunctionDocumentation::Arguments arguments_formatDateTime =
    {
        {"datetime", "A date or date time to format.", {"Date", "Date32", "DateTime", "DateTime64"}},
        {"format", "Format string with replacement fields.", {"String"}},
        {"timezone", "Optional. Timezone name for the formatted time.", {"String"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_formatDateTime = {"Returns time and date values according to the determined format.", {"String"}};
    FunctionDocumentation::Examples examples_formatDateTime =
    {
        {"Format date with year placeholder", R"(
SELECT formatDateTime(toDate('2010-01-04'), '%g')
        )",
        R"(
┌─formatDateTime(toDate('2010-01-04'), '%g')─┐
│ 10                                         │
└────────────────────────────────────────────┘
        )"},
        {"Format DateTime64 with fractional seconds", R"(
SELECT formatDateTime(toDateTime64('2010-01-04 12:34:56.123456', 7), '%f')
        )",
        R"(
┌─formatDateTime(toDateTime64('2010-01-04 12:34:56.123456', 7), '%f')─┐
│ 1234560                                                             │
└─────────────────────────────────────────────────────────────────────┘
        )"},
        {"Format with timezone", R"(
SELECT
    now() AS ts,
    time_zone,
    formatDateTime(ts, '%T', time_zone) AS str_tz_time
FROM system.time_zones
WHERE time_zone LIKE 'Europe%'
LIMIT 10
        )",
        R"(
┌──────────────────ts─┬─time_zone─────────┬─str_tz_time─┐
│ 2023-09-08 19:13:40 │ Europe/Amsterdam  │ 21:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Andorra    │ 21:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Astrakhan  │ 23:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Athens     │ 22:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Belfast    │ 20:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Belgrade   │ 21:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Berlin     │ 21:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Bratislava │ 21:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Brussels   │ 21:13:40    │
│ 2023-09-08 19:13:40 │ Europe/Bucharest  │ 22:13:40    │
└─────────────────────┴───────────────────┴─────────────┘
        )"}
    };
    FunctionDocumentation::IntroducedIn introduced_in_formatDateTime = {1, 1};
    FunctionDocumentation::Category category_formatDateTime = FunctionDocumentation::Category::DateAndTime;
    FunctionDocumentation documentation_formatDateTime = {description_formatDateTime, syntax_formatDateTime, arguments_formatDateTime, returned_value_formatDateTime, examples_formatDateTime, introduced_in_formatDateTime, category_formatDateTime};

    factory.registerFunction<FunctionFormatDateTime>(documentation_formatDateTime);
    factory.registerAlias("DATE_FORMAT", FunctionFormatDateTime::name, FunctionFactory::Case::Insensitive);

    FunctionDocumentation::Description description_fromUnixTimestamp = R"(
This function converts a Unix timestamp to a calendar date and a time of a day.

It can be called in two ways:

- When given a single argument of type [`Integer`](../data-types/int-uint.md), it returns a value of type [`DateTime`](../data-types/datetime.md), i.e. behaves like [`toDateTime`](../../sql-reference/functions/type-conversion-functions.md#todatetime).
- When given two or three arguments where the first argument is a value of type [`Integer`](../data-types/int-uint.md), [`Date`](../data-types/date.md), [`Date32`](../data-types/date32.md), [`DateTime`](../data-types/datetime.md) or [`DateTime64`](../data-types/datetime64.md), the second argument is a constant format string and the third argument is an optional constant time zone string, the function returns a value of type [`String`](../data-types/string.md), i.e. it behaves like [`formatDateTime`](#formatDateTime).
  In this case, [MySQL's datetime format style](https://dev.mysql.com/doc/refman/8.0/en/date-and-time-functions.html#function_date-format) is used.
    )";
    FunctionDocumentation::Syntax syntax_fromUnixTimestamp = R"(
fromUnixTimestamp(timestamp)
fromUnixTimestamp(timestamp[, format[, timezone]])
    )";
    FunctionDocumentation::Arguments arguments_fromUnixTimestamp =
    {
        {"timestamp", "Unix timestamp or date/date with time value.", {"(U)Int*", "Date", "Date32", "DateTime", "DateTime64"}},
        {"format", "Optional. Constant format string for output formatting.", {"String"}},
        {"timezone", "Optional. Constant time zone string.", {"String"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_fromUnixTimestamp = {"Returns `DateTime` of the timestamp when called with one argument, or a String  when called with two or three arguments.", {"DateTime", "String"}};
    FunctionDocumentation::Examples examples_fromUnixTimestamp =
    {
        {"Convert Unix timestamp to DateTime", R"(
SELECT fromUnixTimestamp(423543535)
        )",
        R"(
┌─fromUnixTimestamp(423543535)─┐
│          1983-06-04 10:58:55 │
└──────────────────────────────┘
        )"},
        {"Convert Unix timestamp with format", R"(
SELECT fromUnixTimestamp(1234334543, '%Y-%m-%d %R:%S') AS DateTime
        )",
        R"(
┌─DateTime────────────┐
│ 2009-02-11 14:42:23 │
└─────────────────────┘
        )"}
    };
    FunctionDocumentation::IntroducedIn introduced_in_fromUnixTimestamp = {20, 8};
    FunctionDocumentation::Category category_fromUnixTimestamp = FunctionDocumentation::Category::DateAndTime;
    FunctionDocumentation documentation_fromUnixTimestamp =
    {description_fromUnixTimestamp, syntax_fromUnixTimestamp, arguments_fromUnixTimestamp, returned_value_fromUnixTimestamp, examples_fromUnixTimestamp, introduced_in_fromUnixTimestamp, category_fromUnixTimestamp};

    factory.registerFunction<FunctionFromUnixTimestamp>(documentation_fromUnixTimestamp);
    factory.registerAlias("FROM_UNIXTIME", FunctionFromUnixTimestamp::name, FunctionFactory::Case::Insensitive);

    FunctionDocumentation::Description description_formatDateTimeInJodaSyntax = R"(
Similar to `formatDateTime`, except that it formats datetime in Joda style instead of MySQL style. Refer to [Joda Time documentation](https://joda-time.sourceforge.net/apidocs/org/joda/time/format/DateTimeFormat.html).

The opposite operation of this function is [`parseDateTimeInJodaSyntax`](/sql-reference/functions/type-conversion-functions#parsedatetimeinjodasyntax).

Using replacement fields, you can define a pattern for the resulting string.

**Replacement fields:**

| Placeholder | Description | Presentation | Examples |
|-------------|-------------|-------------|----------|
| G | era | text | AD |
| C | century of era (>=0) | number | 20 |
| Y | year of era (>=0) | year | 1996 |
| x | weekyear (not supported yet) | year | 1996 |
| w | week of weekyear (not supported yet) | number | 27 |
| e | day of week | number | 2 |
| E | day of week | text | Tuesday; Tue |
| y | year | year | 1996 |
| D | day of year | number | 189 |
| M | month of year | month | July; Jul; 07 |
| d | day of month | number | 10 |
| a | halfday of day | text | PM |
| K | hour of halfday (0~11) | number | 0 |
| h | clockhour of halfday (1~12) | number | 12 |
| H | hour of day (0~23) | number | 0 |
| k | clockhour of day (1~24) | number | 24 |
| m | minute of hour | number | 30 |
| s | second of minute | number | 55 |
| S | fraction of second | number | 978 |
| z | time zone | text | Eastern Standard Time; EST |
| Z | time zone offset | zone | -0800; -0812 |
| ' | escape for text | delimiter | |
| '' | single quote | literal | ' |
    )";
    FunctionDocumentation::Syntax syntax_formatDateTimeInJodaSyntax = R"(
formatDateTimeInJodaSyntax(datetime, format[, timezone])
    )";
    FunctionDocumentation::Arguments arguments_formatDateTimeInJodaSyntax =
    {
        {"datetime", "A date or date time to format.", {"DateTime", "Date", "Date32", "DateTime64"}},
        {"format", "Format string with Joda-style replacement fields.", {"String"}},
        {"timezone", "Optional. Timezone name for the formatted time.", {"String"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_formatDateTimeInJodaSyntax = {"Returns time and date values according to the determined format.", {"String"}};
    FunctionDocumentation::Examples examples_formatDateTimeInJodaSyntax =
    {
        {"Format datetime using Joda syntax", R"(
SELECT formatDateTimeInJodaSyntax(toDateTime('2010-01-04 12:34:56'), 'yyyy-MM-dd HH:mm:ss')
        )",
        R"(
┌─formatDateTimeInJodaSyntax(toDateTime('2010-01-04 12:34:56'), 'yyyy-MM-dd HH:mm:ss')─┐
│ 2010-01-04 12:34:56                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────────┘
        )"}
    };
    FunctionDocumentation::IntroducedIn introduced_in_formatDateTimeInJodaSyntax = {20, 1};
    FunctionDocumentation::Category category_formatDateTimeInJodaSyntax = FunctionDocumentation::Category::DateAndTime;
    FunctionDocumentation documentation_formatDateTimeInJodaSyntax = {description_formatDateTimeInJodaSyntax, syntax_formatDateTimeInJodaSyntax, arguments_formatDateTimeInJodaSyntax, returned_value_formatDateTimeInJodaSyntax, examples_formatDateTimeInJodaSyntax, introduced_in_formatDateTimeInJodaSyntax, category_formatDateTimeInJodaSyntax};

    factory.registerFunction<FunctionFormatDateTimeInJodaSyntax>(documentation_formatDateTimeInJodaSyntax);

    FunctionDocumentation::Description description_fromUnixTimestampInJodaSyntax = R"(
This function converts a Unix timestamp to a calendar date and a time of a day.

It can be called in two ways:

When given a single argument of type [`Integer`](../data-types/int-uint.md), it returns a value of type [`DateTime`](../data-types/datetime.md), i.e. behaves like [`toDateTime`](../../sql-reference/functions/type-conversion-functions.md#todatetime).

When given two or three arguments where the first argument is a value of type [`Integer`](../data-types/int-uint.md), [`Date`](../data-types/date.md), [`Date32`](../data-types/date32.md), [`DateTime`](../data-types/datetime.md) or [`DateTime64`](../data-types/datetime64.md), the second argument is a constant format string and the third argument is an optional constant time zone string, the function returns a value of type [`String`](../data-types/string.md), i.e. it behaves like [`formatDateTimeInJodaSyntax`](#formatdatetimeinjodasyntax). In this case, [Joda datetime format style](https://joda-time.sourceforge.net/apidocs/org/joda/time/format/DateTimeFormat.html) is used.
    )";
    FunctionDocumentation::Syntax syntax_fromUnixTimestampInJodaSyntax = R"(
fromUnixTimestampInJodaSyntax(timestamp)
fromUnixTimestampInJodaSyntax(timestamp, format[, timezone])
    )";
    FunctionDocumentation::Arguments arguments_fromUnixTimestampInJodaSyntax =
    {
        {"timestamp", "Unix timestamp or date/time value.", {"(U)Int*", "Date", "Date32", "DateTime", "DateTime64"}},
        {"format", "Optional. Constant format string using Joda syntax for output formatting.", {"String"}},
        {"timezone", "Optional. Constant time zone string.", {"String"}}
    };
    FunctionDocumentation::ReturnedValue returned_value_fromUnixTimestampInJodaSyntax = {"Returns a date with time when called with one argument, or a String when called with two or three arguments.}", {"DateTime", "String"}};
    FunctionDocumentation::Examples examples_fromUnixTimestampInJodaSyntax =
    {
        {"Convert Unix timestamp with Joda format", R"(
SELECT fromUnixTimestampInJodaSyntax(1234334543, 'yyyy-MM-dd HH:mm:ss', 'UTC') AS DateTime
        )",
        R"(
┌─DateTime────────────┐
│ 2009-02-11 06:42:23 │
└─────────────────────┘
        )"}
    };
    FunctionDocumentation::IntroducedIn introduced_in_fromUnixTimestampInJodaSyntax = {23, 1};
    FunctionDocumentation::Category category_fromUnixTimestampInJodaSyntax = FunctionDocumentation::Category::DateAndTime;
    FunctionDocumentation documentation_fromUnixTimestampInJodaSyntax = {description_fromUnixTimestampInJodaSyntax, syntax_fromUnixTimestampInJodaSyntax, arguments_fromUnixTimestampInJodaSyntax, returned_value_fromUnixTimestampInJodaSyntax, examples_fromUnixTimestampInJodaSyntax, introduced_in_fromUnixTimestampInJodaSyntax, category_fromUnixTimestampInJodaSyntax};

    factory.registerFunction<FunctionFromUnixTimestampInJodaSyntax>(documentation_fromUnixTimestampInJodaSyntax);
}
}
