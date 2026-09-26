#include "sql_replay_extract.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "box/space.h"
#include "sqlInt.h"
#include "sql_expr_canonical.h"
#include "sql_logical_plan.h"
#include "sql_replay_expr_list.h"

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
