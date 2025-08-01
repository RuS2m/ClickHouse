#include <Client/Suggest.h>

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/Combinators/AggregateFunctionCombinatorFactory.h>
#include <Columns/ColumnString.h>
#include <Common/Exception.h>
#include <Common/setThreadName.h>
#include <Common/typeid_cast.h>
#include <Common/Macros.h>
#include <Core/Protocol.h>
#include <IO/WriteBufferFromFileDescriptor.h>
#include <IO/Operators.h>
#include <Functions/FunctionFactory.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <Interpreters/Context.h>
#include <Client/Connection.h>
#include <Client/LocalConnection.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int OK;
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_PACKET_FROM_SERVER;
    extern const int DEADLOCK_AVOIDED;
    extern const int USER_SESSION_LIMIT_EXCEEDED;
}

static String getLoadSuggestionQueryUsingSystemTables(Int32 suggestion_limit, bool basic_suggestion, UInt64 server_revision)
{
    /// NOTE: Once you will update the completion list,
    /// do not forget to update 01676_clickhouse_client_autocomplete.sh
    String query;

    auto add_subquery = [&](std::string_view select, std::string_view result_column_name)
    {
        if (!query.empty())
            query += " UNION ALL ";
        query += fmt::format("SELECT * FROM viewIfPermitted({} ELSE null('{} String'))", select, result_column_name);
    };

    auto add_column = [&](std::string_view column_name, std::string_view table_name, bool distinct, std::optional<Int64> limit)
    {
        add_subquery(
            fmt::format(
                "SELECT {}{} FROM system.{}{}",
                (distinct ? "DISTINCT " : ""),
                column_name,
                table_name,
                (limit ? (" LIMIT " + std::to_string(*limit)) : "")),
            column_name);
    };

    add_column("name", "functions", false, {});
    add_column("name", "table_engines", false, {});
    add_column("name", "formats", false, {});
    add_column("name", "table_functions", false, {});
    add_column("name", "data_type_families", false, {});
    add_column("name", "merge_tree_settings", false, {});
    add_column("name", "settings", false, {});

    if (server_revision >= DBMS_MIN_REVISION_WITH_SYSTEM_KEYWORDS_TABLE)
        add_column("keyword", "keywords", false, {});

    if (!basic_suggestion)
    {
        add_column("cluster", "clusters", false, {});
        add_column("macro", "macros", false, {});
        add_column("policy_name", "storage_policies", false, {});
    }

    add_subquery("SELECT concat(func.name, comb.name) AS x FROM system.functions AS func CROSS JOIN system.aggregate_function_combinators AS comb WHERE is_aggregate", "x");

    /// The user may disable loading of databases, tables, columns by setting suggestion_limit to zero.
    if (suggestion_limit > 0)
    {
        add_column("name", "databases", false, suggestion_limit);
        add_column("name", "tables", true, suggestion_limit);
        if (!basic_suggestion)
        {
            add_column("name", "dictionaries", true, suggestion_limit);
        }
        add_column("name", "columns", true, suggestion_limit);
    }

    query = "SELECT DISTINCT arrayJoin(extractAll(name, '[\\\\w_]{2,}')) AS res FROM (" + query + ") WHERE notEmpty(res)";
    return query;
}

static String getLoadSuggestionQueryUsingSystemCompletionsTable(Int32 suggestion_limit, bool basic_suggestion, UInt64 server_revision)
{
    /// NOTE: Once you will update the completion list,
    /// do not forget to update 01676_clickhouse_client_autocomplete.sh
    /// TODO: Use belongs column for better contextual suggestions
    String unlimited_contexts = fmt::format(
        "('function', 'table engine', 'format', 'table function', 'data type', 'merge tree setting', 'setting', 'aggregate function combinator pair'{}{})",
        (server_revision >= DBMS_MIN_REVISION_WITH_SYSTEM_KEYWORDS_TABLE ? ", 'keyword'" : ""),
        (basic_suggestion ? "" : ", 'cluster', 'macro', 'policy'")
    );
    String query = fmt::format(
        "SELECT word FROM system.completions WHERE context IN {}",
        unlimited_contexts
    );

    /// The user may disable loading of databases, tables, columns by setting suggestion_limit to zero.
    if (suggestion_limit > 0)
    {
        String limited_contexts = fmt::format(
            "('database', 'table', 'column'{})",
            (basic_suggestion ? "" : ", 'dictionary'")
        );
        query += fmt::format(
            " UNION ALL SELECT word FROM ("
            " SELECT word, context, ROW_NUMBER() OVER (PARTITION BY context ORDER BY word) AS rn FROM "
            " (SELECT DISTINCT word, context FROM system.completions WHERE context IN {})"
            ") WHERE rn <= {}",
            limited_contexts,
            suggestion_limit
        );
    }

    query = "SELECT DISTINCT arrayJoin(extractAll(word, '[\\\\w_]{2,}')) AS res FROM (" + query + ") WHERE notEmpty(res)";
    return query;
}

static std::optional<UInt8> tryReadUInt8FromBlock(const Block & block)
{
    if (block.empty())
        return std::nullopt;

    if (block.columns() != 1)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Wrong number of columns received for query to check existence of completions table");

    const auto & column = typeid_cast<const ColumnUInt8 &>(*block.getByPosition(0).column);
    return column.getData()[0];
}

bool Suggest::hasSystemCompletions(IServerConnection & connection, const ConnectionTimeouts & timeouts, const ClientInfo & client_info)
{
    UInt8 exists = 0;
    String query = "EXISTS TABLE system.completions";
    fetch(connection, timeouts, query, client_info,
        [&](const Block & block)
        {
            if (auto v = tryReadUInt8FromBlock(block))
                exists = *v;
        });
    return exists != 0;
}

template <typename ConnectionType>
void Suggest::load(ContextPtr context, const ConnectionParameters & connection_parameters, Int32 suggestion_limit, bool wait_for_load)
{
    loading_thread = std::thread([my_context=Context::createCopy(context), connection_parameters, suggestion_limit, this]
    {
        /// Creates new QueryScope/ThreadStatus to avoid sharing global context, which settings can be modified by the client in another thread.
        ThreadStatus thread_status;
        std::optional<CurrentThread::QueryScope> query_scope;
        /// LocalConnection creates QueryScope for each query
        if constexpr (!std::is_same_v<ConnectionType, LocalConnection>)
            query_scope.emplace(my_context);

        setThreadName("Suggest");

        for (size_t retry = 0; retry < 10; ++retry)
        {
            try
            {
                auto connection = ConnectionType::createConnection(connection_parameters, my_context);
                const auto server_revision = connection->getServerRevision(connection_parameters.timeouts);
                const auto basic_suggestion = std::is_same_v<ConnectionType, LocalConnection>;

                auto suggestion_query = hasSystemCompletions(*connection, connection_parameters.timeouts, my_context->getClientInfo()) ? getLoadSuggestionQueryUsingSystemCompletionsTable(suggestion_limit, basic_suggestion, server_revision) : getLoadSuggestionQueryUsingSystemTables(suggestion_limit, basic_suggestion, server_revision);
                fetch(*connection,
                    connection_parameters.timeouts,
                    suggestion_query,
                    my_context->getClientInfo(),
                    [this](const Block & b) { fillWordsFromBlock(b); });
            }
            catch (const Exception & e)
            {
                last_error = e.code();
                if (e.code() == ErrorCodes::DEADLOCK_AVOIDED)
                    continue;
                if (e.code() != ErrorCodes::USER_SESSION_LIMIT_EXCEEDED)
                {
                    /// We should not use std::cerr here, because this method works concurrently with the main thread.
                    /// WriteBufferFromFileDescriptor will write directly to the file descriptor, avoiding data race on std::cerr.
                    ///
                    /// USER_SESSION_LIMIT_EXCEEDED is ignored here. The client will try to receive
                    /// suggestions using the main connection later.
                    WriteBufferFromFileDescriptor out(STDERR_FILENO, 4096);
                    out << "Cannot load data for command line suggestions: " << getCurrentExceptionMessage(false, true) << "\n";
                    out.finalize();
                }
            }
            catch (...)
            {
                last_error = getCurrentExceptionCode();
                WriteBufferFromFileDescriptor out(STDERR_FILENO, 4096);
                out << "Cannot load data for command line suggestions: " << getCurrentExceptionMessage(false, true) << "\n";
                out.finalize();
            }

            break;
        }

        /// Note that keyword suggestions are available even if we cannot load data from server.
    });

    if (wait_for_load)
        loading_thread.join();
}

void Suggest::load(IServerConnection & connection,
                   const ConnectionTimeouts & timeouts,
                   Int32 suggestion_limit,
                   const ClientInfo & client_info)
{
    try
    {
        const auto server_revision = connection.getServerRevision(timeouts);
        auto suggestion_query = hasSystemCompletions(connection, timeouts, client_info) ? getLoadSuggestionQueryUsingSystemCompletionsTable(suggestion_limit, true, server_revision) : getLoadSuggestionQueryUsingSystemTables(suggestion_limit, true, server_revision);
        fetch(connection, timeouts, suggestion_query, client_info, [this](const Block & block) { fillWordsFromBlock(block); });
    }
    catch (...)
    {
        std::cerr << "Suggestions loading exception: " << getCurrentExceptionMessage(false, true) << std::endl;
        last_error = getCurrentExceptionCode();
    }
}

void Suggest::fetch(IServerConnection & connection, const ConnectionTimeouts & timeouts, const std::string & query, const ClientInfo & client_info, std::function<void(const Block &)> on_data)
{
    connection.sendQuery(
        timeouts, query, {} /* query_parameters */, "" /* query_id */, QueryProcessingStage::Complete, nullptr, &client_info, false, {} /* external_roles*/, {});

    while (true)
    {
        Packet packet = connection.receivePacket();
        switch (packet.type)
        {
            case Protocol::Server::Data:
                on_data(packet.block);
                continue;

            case Protocol::Server::TimezoneUpdate:
            case Protocol::Server::Progress:
            case Protocol::Server::ProfileInfo:
            case Protocol::Server::Totals:
            case Protocol::Server::Extremes:
            case Protocol::Server::Log:
            case Protocol::Server::ProfileEvents:
                continue;

            case Protocol::Server::Exception:
                packet.exception->rethrow();
                return;

            case Protocol::Server::EndOfStream:
                last_error = ErrorCodes::OK;
                return;

            default:
                throw Exception(ErrorCodes::UNKNOWN_PACKET_FROM_SERVER, "Unknown packet {} from server {}",
                    packet.type, connection.getDescription());
        }
    }
}

void Suggest::fillWordsFromBlock(const Block & block)
{
    if (block.empty())
        return;

    if (block.columns() != 1)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Wrong number of columns received for query to read words for suggestion");

    const ColumnString & column = typeid_cast<const ColumnString &>(*block.getByPosition(0).column);

    size_t rows = block.rows();

    Words new_words;
    new_words.reserve(rows);
    for (size_t i = 0; i < rows; ++i)
        new_words.emplace_back(column[i].safeGet<String>());

    addWords(std::move(new_words));
}

template
void Suggest::load<Connection>(ContextPtr context, const ConnectionParameters & connection_parameters, Int32 suggestion_limit, bool wait_for_load);

template
void Suggest::load<LocalConnection>(ContextPtr context, const ConnectionParameters & connection_parameters, Int32 suggestion_limit, bool wait_for_load);
}
