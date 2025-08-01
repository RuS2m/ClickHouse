#include <Processors/Transforms/TTLTransform.h>
#include <Interpreters/inplaceBlockConversions.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Columns/ColumnConst.h>
#include <Interpreters/addTypeConversionToAST.h>
#include <Interpreters/Context.h>

#include <Processors/Port.h>
#include <Processors/TTL/TTLAggregationAlgorithm.h>
#include <Processors/TTL/TTLColumnAlgorithm.h>
#include <Processors/TTL/TTLDeleteAlgorithm.h>
#include <Processors/TTL/TTLUpdateInfoAlgorithm.h>

namespace DB
{

static TTLExpressions getExpressions(const TTLDescription & ttl_descr, PreparedSets::Subqueries & subqueries_for_sets, const ContextPtr & context)
{
    auto expr = ttl_descr.buildExpression(context);
    auto expr_queries = expr.sets->getSubqueries();
    subqueries_for_sets.insert(subqueries_for_sets.end(), expr_queries.begin(), expr_queries.end());

    auto where_expr = ttl_descr.buildWhereExpression(context);
    if (where_expr.sets)
    {
        auto where_expr_queries = where_expr.sets->getSubqueries();
        subqueries_for_sets.insert(subqueries_for_sets.end(), where_expr_queries.begin(), where_expr_queries.end());
    }

    return {expr.expression, where_expr.expression};
}

TTLTransform::TTLTransform(
    const ContextPtr & context,
    SharedHeader header_,
    const MergeTreeData & storage_,
    const StorageMetadataPtr & metadata_snapshot_,
    const MergeTreeData::MutableDataPartPtr & data_part_,
    time_t current_time_,
    bool force_)
    : IAccumulatingTransform(header_, header_)
    , data_part(data_part_)
    , log(getLogger(storage_.getLogName() + " (TTLTransform)"))
{
    auto old_ttl_infos = data_part->ttl_infos;

    if (metadata_snapshot_->hasRowsTTL())
    {
        const auto & rows_ttl = metadata_snapshot_->getRowsTTL();
        auto algorithm = std::make_unique<TTLDeleteAlgorithm>(
            getExpressions(rows_ttl, subqueries_for_sets, context), rows_ttl,
            old_ttl_infos.table_ttl, current_time_, force_);

        /// Skip all data if table ttl is expired for part
        if (algorithm->isMaxTTLExpired() && !rows_ttl.where_expression_ast)
            all_data_dropped = true;

        delete_algorithm = algorithm.get();
        algorithms.emplace_back(std::move(algorithm));
    }

    for (const auto & where_ttl : metadata_snapshot_->getRowsWhereTTLs())
        algorithms.emplace_back(std::make_unique<TTLDeleteAlgorithm>(
            getExpressions(where_ttl, subqueries_for_sets, context), where_ttl,
            old_ttl_infos.rows_where_ttl[where_ttl.result_column], current_time_, force_));

    for (const auto & group_by_ttl : metadata_snapshot_->getGroupByTTLs())
        algorithms.emplace_back(std::make_unique<TTLAggregationAlgorithm>(
                getExpressions(group_by_ttl, subqueries_for_sets, context), group_by_ttl,
                old_ttl_infos.group_by_ttl[group_by_ttl.result_column], current_time_, force_,
                getInputPort().getHeader(), storage_));

    if (metadata_snapshot_->hasAnyColumnTTL())
    {
        const auto & storage_columns = metadata_snapshot_->getColumns();
        const auto & column_defaults = storage_columns.getDefaults();

        for (const auto & [name, description] : metadata_snapshot_->getColumnTTLs())
        {
            ExpressionActionsPtr default_expression;
            String default_column_name;
            auto it = column_defaults.find(name);
            if (it != column_defaults.end())
            {
                const auto & column = storage_columns.get(name);
                auto default_ast = it->second.expression->clone();
                default_ast = addTypeConversionToAST(std::move(default_ast), column.type->getName());

                auto syntax_result
                    = TreeRewriter(storage_.getContext()).analyze(default_ast, metadata_snapshot_->getColumns().getAllPhysical());
                default_expression = ExpressionAnalyzer{default_ast, syntax_result, storage_.getContext()}.getActions(true);
                default_column_name = default_ast->getColumnName();
            }

            algorithms.emplace_back(std::make_unique<TTLColumnAlgorithm>(
                getExpressions(description, subqueries_for_sets, context), description,
                old_ttl_infos.columns_ttl[name], current_time_,
                force_, name, default_expression, default_column_name, isCompactPart(data_part)));
        }
    }

    for (const auto & move_ttl : metadata_snapshot_->getMoveTTLs())
        algorithms.emplace_back(std::make_unique<TTLUpdateInfoAlgorithm>(
            getExpressions(move_ttl, subqueries_for_sets, context), move_ttl,
            TTLUpdateField::MOVES_TTL, move_ttl.result_column, old_ttl_infos.moves_ttl[move_ttl.result_column], current_time_, force_));

    for (const auto & recompression_ttl : metadata_snapshot_->getRecompressionTTLs())
        algorithms.emplace_back(std::make_unique<TTLUpdateInfoAlgorithm>(
            getExpressions(recompression_ttl, subqueries_for_sets, context), recompression_ttl,
            TTLUpdateField::RECOMPRESSION_TTL, recompression_ttl.result_column, old_ttl_infos.recompression_ttl[recompression_ttl.result_column], current_time_, force_));
}

Block reorderColumns(Block block, const Block & header)
{
    Block res;
    for (const auto & col : header)
        res.insert(block.getByName(col.name));

    return res;
}

void TTLTransform::consume(Chunk chunk)
{
    if (all_data_dropped)
    {
        finishConsume();
        return;
    }

    convertToFullIfSparse(chunk);
    auto block = getInputPort().getHeader().cloneWithColumns(chunk.detachColumns());

    for (const auto & algorithm : algorithms)
        algorithm->execute(block);

    if (block.empty())
        return;

    size_t num_rows = block.rows();
    setReadyChunk(Chunk(reorderColumns(std::move(block), getOutputPort().getHeader()).getColumns(), num_rows));
}

Chunk TTLTransform::generate()
{
    Block block;
    for (const auto & algorithm : algorithms)
        algorithm->execute(block);

    if (block.empty())
        return {};

    size_t num_rows = block.rows();
    return Chunk(reorderColumns(std::move(block), getOutputPort().getHeader()).getColumns(), num_rows);
}

void TTLTransform::finalize()
{
    data_part->ttl_infos = {};
    for (const auto & algorithm : algorithms)
        algorithm->finalize(data_part);

    if (delete_algorithm)
    {
        if (all_data_dropped)
            LOG_DEBUG(log, "Removed all rows from part {} due to expired TTL", data_part->name);
        else
            LOG_DEBUG(log, "Removed {} rows with expired TTL from part {}", delete_algorithm->getNumberOfRemovedRows(), data_part->name);
    }
    else
        LOG_DEBUG(log, "No delete algorithm was applied for part {}", data_part->name);
}

IProcessor::Status TTLTransform::prepare()
{
    auto status = IAccumulatingTransform::prepare();
    if (status == Status::Finished)
        finalize();

    return status;
}

}
