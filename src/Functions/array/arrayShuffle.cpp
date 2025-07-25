#include <Columns/ColumnArray.h>
#include <Columns/ColumnString.h>
#include <DataTypes/DataTypeArray.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Common/iota.h>
#include <Common/randomSeed.h>
#include <Common/shuffle.h>
#include <Common/typeid_cast.h>

#include <pcg_random.hpp>

#include <numeric>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

/** Shuffle array elements
 * arrayShuffle(arr)
 * arrayShuffle(arr, seed)
 */
struct FunctionArrayShuffleTraits
{
    static constexpr auto name = "arrayShuffle";
    static constexpr auto has_limit = false; // Permute the whole array
    static ColumnNumbers getArgumentsThatAreAlwaysConstant() { return {1}; }
    static constexpr auto max_num_params = 2; // array[, seed]
    static constexpr auto seed_param_idx = 1; // --------^^^^
};

/** Partial shuffle array elements
 * arrayPartialShuffle(arr)
 * arrayPartialShuffle(arr, limit)
 * arrayPartialShuffle(arr, limit, seed)
 */
struct FunctionArrayPartialShuffleTraits
{
    static constexpr auto name = "arrayPartialShuffle";
    static constexpr auto has_limit = true;
    static ColumnNumbers getArgumentsThatAreAlwaysConstant() { return {1, 2}; }
    static constexpr auto max_num_params = 3; // array[, limit[, seed]]
    static constexpr auto seed_param_idx = 2; // ----------------^^^^
};

template <typename Traits>
class FunctionArrayShuffleImpl : public IFunction
{
public:
    static constexpr auto name = Traits::name;

    String getName() const override { return name; }
    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return Traits::getArgumentsThatAreAlwaysConstant(); }
    bool useDefaultImplementationForConstants() const override { return true; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionArrayShuffleImpl<Traits>>(); }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() > Traits::max_num_params || arguments.empty())
        {
            throw Exception(
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Function '{}' needs from 1 to {} arguments, passed {}.",
                getName(),
                Traits::max_num_params,
                arguments.size());
        }

        const DataTypeArray * array_type = checkAndGetDataType<DataTypeArray>(arguments[0].get());
        if (!array_type)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "First argument of function '{}' must be array", getName());

        auto check_is_integral = [&](auto param_idx)
        {
            WhichDataType which(arguments[param_idx]);
            if (!which.isUInt() && !which.isInt())
                throw Exception(
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Illegal type {} of arguments of function {} (must be UInt or Int)",
                    arguments[param_idx]->getName(),
                    getName());
        };

        for (size_t idx = 1; idx < arguments.size(); ++idx)
            check_is_integral(idx);

        return arguments[0];
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t) const override;

private:
    static ColumnPtr executeGeneric(const ColumnArray & array, pcg64_fast & rng, size_t limit);
};

template <typename Traits>
ColumnPtr FunctionArrayShuffleImpl<Traits>::executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t) const
{
    const ColumnArray * array = checkAndGetColumn<ColumnArray>(arguments[0].column.get());
    if (!array)
        throw Exception(
            ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of first argument of function {}", arguments[0].column->getName(), getName());

    const auto seed = [&]() -> uint64_t
    {
        // If present, seed comes as the last argument
        if (arguments.size() != Traits::max_num_params)
            return randomSeed();
        const auto * val = arguments[Traits::seed_param_idx].column.get();
        return val->getUInt(0);
    }();
    pcg64_fast rng(seed);

    size_t limit = [&]() -> size_t
    {
        if constexpr (Traits::has_limit)
        {
            if (arguments.size() > 1)
            {
                const auto * val = arguments[1].column.get();
                return val->getUInt(0);
            }
        }
        return 0;
    }();

    return executeGeneric(*array, rng, limit);
}

template <typename Traits>
ColumnPtr FunctionArrayShuffleImpl<Traits>::executeGeneric(const ColumnArray & array, pcg64_fast & rng, size_t limit [[maybe_unused]])
{
    const ColumnArray::Offsets & offsets = array.getOffsets();

    size_t size = offsets.size();
    size_t nested_size = array.getData().size();
    IColumn::Permutation permutation(nested_size);
    iota(permutation.data(), permutation.size(), IColumn::Permutation::value_type(0));

    ColumnArray::Offset current_offset = 0;
    for (size_t i = 0; i < size; ++i)
    {
        auto next_offset = offsets[i];
        if constexpr (Traits::has_limit)
        {
            if (limit)
            {
                const auto effective_limit = std::min<size_t>(limit, next_offset - current_offset);
                partial_shuffle(&permutation[current_offset], &permutation[next_offset], effective_limit, rng);
            }
            else
                shuffle(&permutation[current_offset], &permutation[next_offset], rng);
        }
        else
            shuffle(&permutation[current_offset], &permutation[next_offset], rng);
        current_offset = next_offset;
    }
    return ColumnArray::create(array.getData().permute(permutation, 0), array.getOffsetsPtr());
}

REGISTER_FUNCTION(ArrayShuffle)
{
    FunctionDocumentation::Description description = R"(
Returns an array of the same size as the original array containing the elements in shuffled order.
Elements are reordered in such a way that each possible permutation of those elements has equal probability of appearance.

:::note
This function will not materialize constants.
:::
    )";
    FunctionDocumentation::Syntax syntax = "arrayShuffle(arr [, seed])";
    FunctionDocumentation::Arguments arguments = {
        {"arr", "The array to shuffle.", {"Array(T)"}},
        {"seed (optional)", "Optional. The seed to be used with random number generation. If not provided a random one is used.", {"(U)Int*"}},
    };
    FunctionDocumentation::ReturnedValue returned_value = {"Array with elements shuffled", {"Array(T)"}};
    FunctionDocumentation::Examples examples = {
        {"Example without seed (unstable results)", "SELECT arrayShuffle([1, 2, 3, 4]);", "[1,4,2,3]"},
        {"Example without seed (stable results)", "SELECT arrayShuffle([1, 2, 3, 4], 41);", "[3,2,1,4]"}
    };
    FunctionDocumentation::IntroducedIn introduced_in = {23, 2};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Array;
    FunctionDocumentation documentation = {description, syntax, arguments, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionArrayShuffleImpl<FunctionArrayShuffleTraits>>(documentation, FunctionFactory::Case::Insensitive);

    description = R"(
Returns an array of the same size as the original array where elements in range `[1..limit]` are a random
subset of the original array. Remaining `(limit..n]` shall contain the elements not in `[1..limit]` range in undefined order.
Value of limit shall be in range `[1..n]`. Values outside of that range are equivalent to performing full `arrayShuffle`:

:::note
This function will not materialize constants.

The value of `limit` should be in the range `[1..N]`. Values outside of that range are equivalent to performing full [`arrayShuffle`](#arrayShuffle).
:::
    )";
    syntax = "arrayPartialShuffle(arr [, limit[, seed]])";
    arguments = {
        {"arr", "The array to shuffle.", {"Array(T)"}},
        {"seed", "Optional. The seed to be used with random number generation. If not provided, a random one is used.", {"(U)Int*"}},
        {"limit", "Optional. The number to limit element swaps to, in the range `[1..N]`.", {"(U)Int*"}},
    };
    returned_value = {"Array with elements partially shuffled.", {"Array(T)"}};
    examples = {
        {"no_limit1", "SELECT arrayPartialShuffle([1, 2, 3, 4], 0)", "[2, 4, 3, 1]"},
        {"no_limit2", "SELECT arrayPartialShuffle([1, 2, 3, 4])", "[4, 1, 3, 2]"},
        {"random_seed", "SELECT arrayPartialShuffle([1, 2, 3, 4], 2)", "[3, 4, 1, 2]"},
        {"explicit_seed", "SELECT arrayPartialShuffle([1, 2, 3, 4], 2, 41)", "[3, 2, 1, 4]"},
        {"materialize", "SELECT arrayPartialShuffle(materialize([1, 2, 3, 4]), 2, 42), arrayPartialShuffle([1, 2, 3], 2, 42) FROM numbers(10)", R"(
┌─arrayPartial⋯4]), 2, 42)─┬─arrayPartial⋯ 3], 2, 42)─┐
│ [3,2,1,4]                │ [3,2,1]                  │
│ [3,2,1,4]                │ [3,2,1]                  │
│ [4,3,2,1]                │ [3,2,1]                  │
│ [1,4,3,2]                │ [3,2,1]                  │
│ [3,4,1,2]                │ [3,2,1]                  │
│ [1,2,3,4]                │ [3,2,1]                  │
│ [1,4,3,2]                │ [3,2,1]                  │
│ [1,4,3,2]                │ [3,2,1]                  │
│ [3,1,2,4]                │ [3,2,1]                  │
│ [1,3,2,4]                │ [3,2,1]                  │
└──────────────────────────┴──────────────────────────┘
    )"}
    };
    documentation = {description, syntax, arguments, returned_value, examples, introduced_in, category};
    factory.registerFunction<FunctionArrayShuffleImpl<FunctionArrayPartialShuffleTraits>>(documentation, FunctionFactory::Case::Insensitive);
}

}
