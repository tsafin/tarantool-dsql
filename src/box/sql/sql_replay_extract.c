#include "sql_replay_extract.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "box/space.h"
#include "sql_stats_snapshot.h"
#include "sqlInt.h"
#include "sql_expr_canonical.h"
#include "sql_logical_plan.h"
#include "sql_replay_expr_list.h"
#include "sql_replay_schema.h"

static enum sql_replay_input_status
canonical_limit(const struct Expr *expr, const uint32_t *cursor_to_relation,
		size_t cursor_count, bool *present, uint64_t *value)
{
	*present = expr != NULL;
	*value = 0;
	if (expr == NULL)
		return SQL_REPLAY_INPUT_OK;
	enum sql_expr_canonical_reject reason;
	char *canonical = sql_expr_canonicalize(expr, cursor_to_relation,
						cursor_count, &reason);
	if (canonical == NULL)
		return reason == SQL_EXPR_CANONICAL_NOMEM ? SQL_REPLAY_INPUT_NOMEM :
			SQL_REPLAY_INPUT_INVALID;
	/* LIMIT/OFFSET are captured only when the AST is a nonnegative integer
	 * literal. General constant-folding would need to duplicate SQL semantics.
	 */
	if (strncmp(canonical, "int(", 4) != 0) {
		free(canonical);
		return SQL_REPLAY_INPUT_INVALID;
	}
	char *end;
	errno = 0;
	unsigned long long parsed = strtoull(canonical + 4, &end, 10);
	if (end == canonical + 4 || *end != ')' || end[1] != '\0' ||
	    errno == ERANGE || canonical[4] == '-') {
		free(canonical);
		return SQL_REPLAY_INPUT_INVALID;
	}
	*value = (uint64_t)parsed;
	free(canonical);
	return SQL_REPLAY_INPUT_OK;
}

static bool
valid_cursor_map(const struct Select *select,
		 const uint32_t *cursor_to_relation, size_t cursor_count)
{
	const struct SrcList_item *source = &select->pSrc->a[0];
	if (source->iCursor < 0 || (size_t)source->iCursor >= cursor_count ||
	    cursor_to_relation == NULL ||
	    cursor_to_relation[source->iCursor] != 0)
		return false;
	for (size_t i = 0; i < cursor_count; i++) {
		if (i != (size_t)source->iCursor &&
		    cursor_to_relation[i] != UINT32_MAX)
			return false;
	}
	return true;
}

enum sql_replay_input_status
sql_replay_input_extract_select(const struct Select *select,
				const struct sql_replay_input_spec *metadata,
				const uint32_t *cursor_to_relation,
				size_t cursor_count,
				struct sql_replay_input **result)
{
	if (result == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*result = NULL;
	if (select == NULL || metadata == NULL || select->pSrc == NULL ||
	    select->pNext != NULL || select->pValuesTail != NULL ||
	    (select->selFlags & (SF_Values | SF_NestedFrom)) != 0 ||
	    select->pSrc->nSrc != 1 || select->pSrc->a[0].space == NULL ||
	    select->pSrc->a[0].space->def == NULL ||
	    select->pSrc->a[0].space->def->opts.is_view ||
	    select->pSrc->a[0].pSelect != NULL ||
	    select->pSrc->a[0].fg.isTabFunc || select->pSrc->a[0].pOn != NULL ||
	    select->pSrc->a[0].pUsing != NULL ||
	    select->pSrc->a[0].fg.notIndexed ||
	    select->pSrc->a[0].fg.isIndexedBy ||
	    metadata->relation.column_count !=
	    select->pSrc->a[0].space->def->field_count ||
	    !valid_cursor_map(select, cursor_to_relation, cursor_count))
		return SQL_REPLAY_INPUT_INVALID;
	enum sql_logical_reject_reason reject_reason;
	struct sql_logical_plan *logical = sql_logical_plan_from_select(select,
								       &reject_reason);
	if (logical == NULL)
		return reject_reason == SQL_LOGICAL_REJECT_NONE ?
			SQL_REPLAY_INPUT_NOMEM : SQL_REPLAY_INPUT_INVALID;
	struct sql_replay_expr_list projections = {};
	struct sql_replay_expr_list order_expressions = {};
	enum sql_replay_expr_list_status expr_status =
		sql_replay_expr_list_create(select->pEList, cursor_to_relation,
					    cursor_count, &projections);
	if (expr_status != SQL_REPLAY_EXPR_LIST_OK) {
		sql_logical_plan_delete(logical);
		return expr_status == SQL_REPLAY_EXPR_LIST_NOMEM ?
			SQL_REPLAY_INPUT_NOMEM : SQL_REPLAY_INPUT_INVALID;
	}
	if (select->pOrderBy != NULL) {
		expr_status = sql_replay_expr_list_create(select->pOrderBy,
			cursor_to_relation, cursor_count, &order_expressions);
		if (expr_status != SQL_REPLAY_EXPR_LIST_OK) {
			sql_replay_expr_list_destroy(&projections);
			sql_logical_plan_delete(logical);
			return expr_status == SQL_REPLAY_EXPR_LIST_NOMEM ?
				SQL_REPLAY_INPUT_NOMEM : SQL_REPLAY_INPUT_INVALID;
		}
	}
	char *predicate = NULL;
	if (select->pWhere != NULL) {
		enum sql_expr_canonical_reject reason;
		predicate = sql_expr_canonicalize(select->pWhere,
			cursor_to_relation, cursor_count, &reason);
		if (predicate == NULL) {
			sql_replay_expr_list_destroy(&order_expressions);
			sql_replay_expr_list_destroy(&projections);
			sql_logical_plan_delete(logical);
			return reason == SQL_EXPR_CANONICAL_NOMEM ?
				SQL_REPLAY_INPUT_NOMEM : SQL_REPLAY_INPUT_INVALID;
		}
	} else {
		predicate = strdup("int(1)");
		if (predicate == NULL) {
			sql_replay_expr_list_destroy(&order_expressions);
			sql_replay_expr_list_destroy(&projections);
			sql_logical_plan_delete(logical);
			return SQL_REPLAY_INPUT_NOMEM;
		}
	}
	struct sql_replay_order_spec *order = NULL;
	if (order_expressions.count != 0) {
		if (order_expressions.count > SIZE_MAX / sizeof(*order)) {
			free(predicate);
			sql_replay_expr_list_destroy(&order_expressions);
			sql_replay_expr_list_destroy(&projections);
			sql_logical_plan_delete(logical);
			return SQL_REPLAY_INPUT_INVALID;
		}
		order = calloc(order_expressions.count, sizeof(*order));
		if (order == NULL) {
			free(predicate);
			sql_replay_expr_list_destroy(&order_expressions);
			sql_replay_expr_list_destroy(&projections);
			sql_logical_plan_delete(logical);
			return SQL_REPLAY_INPUT_NOMEM;
		}
		for (size_t i = 0; i < order_expressions.count; i++) {
			enum sort_order sort = select->pOrderBy->a[i].sort_order;
			if (sort != SORT_ORDER_ASC && sort != SORT_ORDER_DESC) {
				free(order);
				free(predicate);
				sql_replay_expr_list_destroy(&order_expressions);
				sql_replay_expr_list_destroy(&projections);
				sql_logical_plan_delete(logical);
				return SQL_REPLAY_INPUT_INVALID;
			}
			order[i] = (struct sql_replay_order_spec) {
				.canonical_expression = order_expressions.items[i],
				.descending = sort == SORT_ORDER_DESC,
				/* Tuple comparison orders NIL before values in ascending
				 * order and reverses that order for descending keys.
				 */
				.nulls_first = sort == SORT_ORDER_ASC,
			};
		}
	}
	bool limit_present, offset_present;
	uint64_t limit, offset;
	enum sql_replay_input_status status = canonical_limit(select->pLimit,
		cursor_to_relation, cursor_count, &limit_present, &limit);
	if (status == SQL_REPLAY_INPUT_OK)
		status = canonical_limit(select->pOffset, cursor_to_relation,
					 cursor_count, &offset_present, &offset);
	if (status == SQL_REPLAY_INPUT_OK) {
		struct sql_replay_input_spec extracted = *metadata;
		extracted.predicate = predicate;
		extracted.projections =
			(const char *const *)projections.items;
		extracted.projection_count = projections.count;
		extracted.order_by = order;
		extracted.order_by_count = order_expressions.count;
		extracted.limit_present = limit_present;
		extracted.limit = limit;
		extracted.offset_present = offset_present;
		extracted.offset = offset;
		status = sql_replay_input_create(&extracted, result);
	}
	free(order);
	free(predicate);
	sql_replay_expr_list_destroy(&order_expressions);
	sql_replay_expr_list_destroy(&projections);
	sql_logical_plan_delete(logical);
	return status;
}

enum sql_replay_input_status
sql_replay_input_extract_select_from_catalog(
	const struct Select *select, const uint32_t *cursor_to_relation,
	size_t cursor_count, uint32_t planner_algorithm_version,
	uint32_t planner_config_version, uint32_t beam_width,
	struct sql_replay_input **result)
{
	if (result == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*result = NULL;
	if (select == NULL || select->pSrc == NULL || select->pSrc->nSrc != 1)
		return SQL_REPLAY_INPUT_INVALID;
	struct sql_replay_space_schema schema;
	enum sql_replay_input_status status = sql_replay_space_schema_create(
		select->pSrc->a[0].space, &schema);
	if (status != SQL_REPLAY_INPUT_OK)
		return status;
	struct sql_replay_input_spec metadata = {
		.relation = schema.relation,
		.planner_algorithm_version = planner_algorithm_version,
		.planner_config_version = planner_config_version,
		.beam_width = beam_width,
	};
	status = sql_replay_input_extract_select(select, &metadata,
						 cursor_to_relation,
						 cursor_count, result);
	sql_replay_space_schema_destroy(&schema);
	return status;
}

static bool
replay_exact_uint64(double value, uint64_t *result)
{
	if (!isfinite(value) || value < 0 || value >= 0x1p64 ||
	    floor(value) != value)
		return false;
	*result = (uint64_t)value;
	return true;
}

enum sql_replay_input_status
sql_replay_input_extract_select_from_snapshot(
	const struct Select *select, const uint32_t *cursor_to_relation,
	size_t cursor_count, uint32_t planner_algorithm_version,
	uint32_t planner_config_version, uint32_t beam_width,
	const struct sql_stats_snapshot *snapshot, uint64_t current_schema_version,
	struct sql_replay_input **result)
{
	if (result == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*result = NULL;
	if (select == NULL || select->pSrc == NULL || select->pSrc->nSrc != 1)
		return SQL_REPLAY_INPUT_INVALID;
	struct sql_replay_space_schema schema;
	enum sql_replay_input_status status = sql_replay_space_schema_create(
		select->pSrc->a[0].space, &schema);
	if (status != SQL_REPLAY_INPUT_OK)
		return status;
	struct sql_replay_relation_spec relation = schema.relation;
	size_t index_count = relation.index_count;
	struct sql_replay_index_spec *indexes = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*indexes));
	uint64_t **prefix_storage = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*prefix_storage));
	if (index_count != 0 && (indexes == NULL || prefix_storage == NULL)) {
		free(indexes);
		free(prefix_storage);
		sql_replay_space_schema_destroy(&schema);
		return SQL_REPLAY_INPUT_NOMEM;
	}
	if (index_count != 0)
		memcpy(indexes, schema.relation.indexes,
		       index_count * sizeof(*indexes));
	const struct sql_stats_relation *stats_relation = NULL;
	enum sql_stats_lookup_status lookup = snapshot == NULL ?
		SQL_STATS_LOOKUP_MISSING : sql_stats_snapshot_get_relation(
			snapshot, current_schema_version,
			select->pSrc->a[0].space->def->id, &stats_relation);
	uint64_t row_count, average_width;
	if (lookup == SQL_STATS_LOOKUP_AVAILABLE) {
		enum sql_stats_cardinality_semantics semantics =
			sql_stats_relation_cardinality_semantics(stats_relation);
		if (semantics < SQL_STATS_CARDINALITY_VISIBLE_ROWS ||
		    semantics > SQL_STATS_CARDINALITY_ESTIMATE) {
			status = SQL_REPLAY_INPUT_INVALID;
			goto cleanup;
		}
		if (!replay_exact_uint64(sql_stats_relation_row_count(stats_relation),
					 &row_count) ||
		    !replay_exact_uint64(
				sql_stats_relation_average_row_width(stats_relation),
				&average_width) ||
		    sql_stats_relation_width_denominator_count(stats_relation) == 0 ||
		    sql_stats_relation_population_basis(stats_relation) == NULL ||
		    sql_stats_relation_width_basis(stats_relation) == NULL ||
		    sql_stats_relation_confidence_source(stats_relation) == NULL) {
			status = SQL_REPLAY_INPUT_INVALID;
			goto cleanup;
		}
		double confidence = sql_stats_relation_confidence(stats_relation);
		if (!isfinite(confidence) || confidence < 0 || confidence > 1) {
			status = SQL_REPLAY_INPUT_INVALID;
			goto cleanup;
		}
		relation.statistics_present = true;
		relation.row_count = row_count;
		relation.cardinality_semantics =
			(enum sql_replay_cardinality_semantics)semantics;
		relation.population_basis =
			sql_stats_relation_population_basis(stats_relation);
		relation.average_row_width = average_width;
		relation.width_basis = sql_stats_relation_width_basis(stats_relation);
		relation.width_denominator_count =
			sql_stats_relation_width_denominator_count(stats_relation);
		relation.confidence_ppm = (uint32_t)floor(confidence * 1000000 + 0.5);
		relation.confidence_source =
			sql_stats_relation_confidence_source(stats_relation);
		relation.collected_at =
			sql_stats_relation_collected_at(stats_relation);
		relation.modification_epoch =
			sql_stats_relation_modification_epoch(stats_relation);
		for (size_t i = 0; i < index_count; i++) {
			const struct sql_stats_index *stats_index = NULL;
			lookup = sql_stats_relation_get_index(stats_relation,
				schema.storage_index_ids[i], &stats_index);
			if (lookup != SQL_STATS_LOOKUP_AVAILABLE)
				continue;
			size_t count = sql_stats_index_prefix_count(stats_index);
			if (count != indexes[i].part_count ||
			    count > SIZE_MAX / sizeof(**prefix_storage)) {
				status = SQL_REPLAY_INPUT_INVALID;
				goto cleanup;
			}
			prefix_storage[i] = count == 0 ? NULL :
				calloc(count, sizeof(**prefix_storage));
			if (count != 0 && prefix_storage[i] == NULL) {
				status = SQL_REPLAY_INPUT_NOMEM;
				goto cleanup;
			}
			for (size_t j = 0; j < count; j++)
				prefix_storage[i][j] =
					sql_stats_index_distinct_prefix(stats_index, j);
			indexes[i].statistics_present = true;
			indexes[i].tuple_count =
				sql_stats_index_tuple_count(stats_index);
			enum sql_stats_cardinality_semantics index_semantics =
				sql_stats_index_tuple_count_semantics(stats_index);
			if (index_semantics < SQL_STATS_CARDINALITY_VISIBLE_ROWS ||
			    index_semantics > SQL_STATS_CARDINALITY_ESTIMATE) {
				status = SQL_REPLAY_INPUT_INVALID;
				goto cleanup;
			}
			indexes[i].tuple_count_semantics =
				(enum sql_replay_cardinality_semantics)index_semantics;
			indexes[i].definition_version =
				sql_stats_index_definition_version(stats_index);
			indexes[i].population_basis =
				sql_stats_index_population_basis(stats_index);
			indexes[i].ndv_basis = sql_stats_index_ndv_basis(stats_index);
			indexes[i].distinct_prefixes = prefix_storage[i];
			indexes[i].prefix_count = count;
		}
		relation.indexes = indexes;
	}
	struct sql_replay_input_spec metadata = {
		.relation = relation,
		.planner_algorithm_version = planner_algorithm_version,
		.planner_config_version = planner_config_version,
		.beam_width = beam_width,
	};
	status = sql_replay_input_extract_select(select, &metadata,
						 cursor_to_relation,
						 cursor_count, result);
cleanup:
	for (size_t i = 0; i < index_count; i++)
		free(prefix_storage[i]);
	free(prefix_storage);
	free(indexes);
	sql_replay_space_schema_destroy(&schema);
	return status;
}
