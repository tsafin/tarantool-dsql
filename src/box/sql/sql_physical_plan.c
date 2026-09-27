#include "sql_physical_plan.h"

#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include <string.h>

#include "sqlInt.h"
#include "sql_logical_plan.h"
#include "box/space.h"

static const struct sql_logical_node *
find_scan(const struct sql_logical_plan *logical)
{
	const struct sql_logical_node *node = sql_logical_plan_root(logical);
	while (node != NULL && node->op != SQL_LOGICAL_SCAN)
		node = node->input;
	return node;
}

static bool
extract_literal_limit(const struct Expr *expr, uint64_t *value)
{
	if (expr == NULL || value == NULL || expr->op != TK_INTEGER ||
	    (expr->flags & EP_Resolved) == 0 ||
	    (expr->flags & (EP_Reduced | EP_TokenOnly)) != 0)
		return false;
	int64_t signed_value;
	bool is_negative = false;
	if ((expr->flags & EP_IntValue) != 0) {
		signed_value = expr->u.iValue;
		if (signed_value < 0)
			return false;
	} else if (expr->u.zToken == NULL ||
		   sql_atoi64(expr->u.zToken, &signed_value, &is_negative,
			      strlen(expr->u.zToken)) != 0 || is_negative) {
		return false;
	}
	if ((uint64_t)signed_value > INT_MAX)
		return false;
	*value = (uint64_t)signed_value;
	return true;
}

static bool
candidate_is_better(const struct sql_physical_candidate *a,
		    const struct sql_physical_candidate *b)
{
	if (b == NULL || a->total_cost < b->total_cost)
		return true;
	if (a->total_cost > b->total_cost)
		return false;
	if (a->access.kind != b->access.kind)
		return a->access.kind < b->access.kind;
	return a->access.index_id < b->access.index_id;
}

struct sql_plan_descriptor *
sql_physical_plan_from_logical(const struct sql_logical_plan *logical,
			       const struct sql_physical_candidate *candidates,
			       size_t candidate_count,
			       enum sql_physical_reject_reason *reason)
{
	if (reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_NONE;
	const struct sql_logical_node *scan = find_scan(logical);
	if (scan == NULL || scan->space_name == NULL) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		return NULL;
	}
	if (candidates == NULL || candidate_count == 0) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_NO_ACCESS_PATH;
		return NULL;
	}
	const struct sql_physical_candidate *best = NULL;
	struct sql_plan_descriptor *best_plan = NULL;
	bool invalid_candidate = false;
	for (size_t i = 0; i < candidate_count; ++i) {
		const struct sql_physical_candidate *candidate = &candidates[i];
		if (!isfinite(candidate->startup_cost) ||
		    !isfinite(candidate->total_cost) ||
		    !isfinite(candidate->rows) || !isfinite(candidate->row_width) ||
		    !isfinite(candidate->confidence) ||
		    candidate->startup_cost < 0 ||
		    candidate->total_cost < candidate->startup_cost ||
		    candidate->rows < 0 || candidate->row_width < 0 ||
		    candidate->confidence < 0 || candidate->confidence > 1) {
			invalid_candidate = true;
			continue;
		}
		if (!candidate_is_better(candidate, best))
			continue;
		struct sql_plan_descriptor_input input = {
			.descriptor_version = 1,
			.planner_version = 1,
			.path_class = SQL_PLAN_NEW_PLANNER,
			.space_id = scan->space_id,
			.space_name = scan->space_name,
			.access = candidate->access,
			.expressions = candidate->expressions,
			.expression_count = candidate->expression_count,
			.cost_startup = candidate->startup_cost,
			.cost_total = candidate->total_cost,
			.cost_rows = candidate->rows,
			.cost_row_width = candidate->row_width,
			.cost_confidence = candidate->confidence,
		};
		struct sql_plan_descriptor *plan = sql_plan_descriptor_new(&input);
		if (plan == NULL) {
			invalid_candidate = true;
			continue;
		}
		sql_plan_descriptor_delete(best_plan);
		best_plan = plan;
		best = candidate;
	}
	if (best_plan == NULL && reason != NULL)
		*reason = invalid_candidate ? SQL_PHYSICAL_REJECT_INVALID_CANDIDATE :
			SQL_PHYSICAL_REJECT_NO_ACCESS_PATH;
	return best_plan;
}

struct sql_plan_descriptor *
sql_physical_table_scan_from_select(
	const struct Select *select,
	const struct sql_physical_table_scan_estimate *estimate,
	enum sql_physical_reject_reason *reason)
{
	if (reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_NONE;
	if (select == NULL || estimate == NULL || select->pSrc == NULL ||
	    select->pSrc->nSrc != 1 || select->pWhere != NULL ||
	    select->pOrderBy != NULL ||
	    select->pEList == NULL ||
	    select->pEList->nExpr <= 0) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		return NULL;
	}
	struct sql_plan_finalize finalize;
	if (select->pLimit != NULL || select->pOffset != NULL) {
		uint64_t limit, offset = 0;
		if (!extract_literal_limit(select->pLimit, &limit) ||
		    (select->pOffset != NULL &&
		     !extract_literal_limit(select->pOffset, &offset))) {
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			return NULL;
		}
		finalize = (struct sql_plan_finalize) {
			.kind = SQL_PLAN_LIMIT,
			.limit = limit,
			.offset = offset,
		};
	}
	enum sql_logical_reject_reason logical_reason;
	struct sql_logical_plan *logical = sql_logical_plan_from_select(select,
									 &logical_reason);
	if (logical == NULL) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		return NULL;
	}
	sql_logical_plan_delete(logical);
	const struct SrcList_item *source = &select->pSrc->a[0];
	if (source->space == NULL || source->space->def == NULL ||
	    source->space->def->opts.is_view || source->iCursor < 0) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		return NULL;
	}
	uint32_t *columns = calloc(select->pEList->nExpr, sizeof(*columns));
	if (columns == NULL) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
		return NULL;
	}
	for (int i = 0; i < select->pEList->nExpr; ++i) {
		const struct Expr *expr = select->pEList->a[i].pExpr;
		if (expr == NULL || ExprHasProperty(expr, EP_TokenOnly | EP_Reduced) ||
		    expr->op != TK_COLUMN_REF || expr->pLeft != NULL ||
		    expr->pRight != NULL ||
		    expr->iTable != source->iCursor || expr->iColumn < 0 ||
		    (uint32_t)expr->iColumn >= source->space->def->field_count) {
			free(columns);
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			return NULL;
		}
		columns[i] = (uint32_t)expr->iColumn;
	}
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.finalize = select->pLimit == NULL && select->pOffset == NULL ?
			NULL : &finalize,
		.finalize_count = select->pLimit == NULL && select->pOffset == NULL ?
			0 : 1,
		.space_id = source->space->def->id,
		.space_name = source->space->def->name,
		.access = {
			.kind = SQL_PLAN_TABLE_FULL_SCAN,
			.direction = SQL_PLAN_ASC,
			.projected_columns = columns,
			.projected_column_count = select->pEList->nExpr,
			.est_rows = estimate->rows,
			.est_rows_confidence = estimate->confidence,
		},
		.projection_columns = columns,
		.projection_column_count = select->pEList->nExpr,
		.cost_startup = estimate->startup_cost,
		.cost_total = estimate->total_cost,
		.cost_rows = estimate->rows,
		.cost_row_width = estimate->row_width,
		.cost_confidence = estimate->confidence,
	};
	struct sql_plan_descriptor *plan = sql_plan_descriptor_new(&input);
	free(columns);
	if (plan == NULL && reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
	return plan;
}
