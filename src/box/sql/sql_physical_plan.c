#include "sql_physical_plan.h"

#include <errno.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "sqlInt.h"
#include "sql_logical_plan.h"
#include "box/index.h"
#include "box/index_def.h"
#include "box/key_def.h"
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
	*value = (uint64_t)signed_value;
	return true;
}

struct parsed_pk_bound {
	enum sql_plan_bound_op op;
	bool is_unsigned;
	int64_t signed_key;
	uint64_t unsigned_key;
};

static bool
parse_pk_bound(const struct Expr *expr, int cursor, uint32_t fieldno,
	       bool is_unsigned, struct parsed_pk_bound *out)
{
	if (expr == NULL || out == NULL)
		return false;
	const struct Expr *column = expr->pLeft;
	const struct Expr *value = expr->pRight;
	if (column != NULL && value != NULL && column->op != TK_COLUMN_REF &&
	    value->op == TK_COLUMN_REF) {
		const struct Expr *tmp = column;
		column = value;
		value = tmp;
	}
	enum sql_plan_bound_op op;
	switch (expr->op) {
	case TK_EQ: op = SQL_PLAN_EQ; break;
	case TK_GT: op = SQL_PLAN_GT; break;
	case TK_GE: op = SQL_PLAN_GE; break;
	case TK_LT: op = SQL_PLAN_LT; break;
	case TK_LE: op = SQL_PLAN_LE; break;
	default: return false;
	}
	if (column == NULL || value == NULL ||
	    ExprHasProperty(column, EP_TokenOnly | EP_Reduced) ||
	    column->op != TK_COLUMN_REF || column->pLeft != NULL ||
	    column->pRight != NULL || column->iTable != cursor ||
	    column->iColumn < 0 || (uint32_t)column->iColumn != fieldno ||
	    ExprHasProperty(value, EP_TokenOnly | EP_Reduced))
		return false;
	bool negated = value->op == TK_UMINUS;
	const struct Expr *literal = negated ? value->pLeft : value;
	if ((negated && value->pRight != NULL) || literal == NULL ||
	    ExprHasProperty(literal, EP_TokenOnly | EP_Reduced) ||
	    literal->op != TK_INTEGER || (literal->flags & EP_Resolved) == 0 ||
	    literal->pLeft != NULL || literal->pRight != NULL)
		return false;
	struct parsed_pk_bound result = {.op = op, .is_unsigned = is_unsigned};
	if (is_unsigned) {
		if (negated)
			return false;
		if ((literal->flags & EP_IntValue) != 0) {
			if (literal->u.iValue < 0)
				return false;
			result.unsigned_key = (uint64_t)literal->u.iValue;
		} else {
			const char *token = literal->u.zToken;
			if (token == NULL || token[0] == '-')
				return false;
			errno = 0;
			char *end;
			unsigned long long parsed = strtoull(token, &end, 10);
			if (errno == ERANGE || end == token || *end != '\0')
				return false;
			result.unsigned_key = (uint64_t)parsed;
		}
	} else {
		bool negative = false;
		bool parsed = false;
		if ((literal->flags & EP_IntValue) != 0) {
			result.signed_key = literal->u.iValue;
			parsed = true;
		} else if (literal->u.zToken != NULL &&
			   sql_atoi64(literal->u.zToken, &result.signed_key,
				      &negative, strlen(literal->u.zToken)) == 0) {
			parsed = true;
		} else if (negated && literal->u.zToken != NULL &&
			   strcmp(literal->u.zToken, "9223372036854775808") == 0) {
			result.signed_key = INT64_MIN;
			parsed = true;
		}
		if (!parsed || (negated && negative))
			return false;
		if (negated && result.signed_key != INT64_MIN) {
			if (result.signed_key < 0)
				return false;
			result.signed_key = -result.signed_key;
		}
	}
	if (column != expr->pLeft) {
		switch (op) {
		case SQL_PLAN_GT: result.op = SQL_PLAN_LT; break;
		case SQL_PLAN_GE: result.op = SQL_PLAN_LE; break;
		case SQL_PLAN_LT: result.op = SQL_PLAN_GT; break;
		case SQL_PLAN_LE: result.op = SQL_PLAN_GE; break;
		default: break;
		}
	}
	*out = result;
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
	    select->pSrc->nSrc != 1 ||
	    select->pEList == NULL ||
	    select->pEList->nExpr <= 0) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		return NULL;
	}
	struct sql_plan_finalize finalize = {0};
	bool force_empty = false;
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
	    source->space->def->opts.is_view || source->iCursor < 0 ||
	    source->space->index_map == NULL ||
	    source->space->index_map[0] == NULL ||
	    source->space->index_map[0]->def == NULL ||
	    source->space->index_map[0]->def->key_def == NULL) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		return NULL;
	}
	bool has_point_key = false;
	bool has_range_key = false;
	bool has_range_end_key = false;
	bool primary_key_not_null = false;
	bool primary_key_is_null = false;
	int64_t point_key = 0;
	int64_t range_key = 0;
	int64_t range_end_key = 0;
	enum sql_plan_bound_op range_op = SQL_PLAN_EQ;
	enum sql_plan_bound_op range_end_op = SQL_PLAN_EQ;
	uint64_t unsigned_point_key = 0;
	uint64_t unsigned_range_key = 0;
	uint64_t unsigned_range_end_key = 0;
	bool unsigned_point = false;
	uint32_t primary_field = source->space->index_map[0]->def->key_def->
		parts[0].fieldno;
	const struct key_def *pk = source->space->index_map[0]->def->key_def;
	unsigned_point = pk->part_count == 1 &&
		pk->parts[0].type == FIELD_TYPE_UNSIGNED;
	if (select->pWhere != NULL) {
		const struct Expr *where = select->pWhere;
		bool is_primary_key_part = false;
		if ((where->op == TK_NOTNULL || where->op == TK_ISNULL) &&
		    where->pLeft != NULL && where->pRight == NULL &&
		    where->pLeft->op == TK_COLUMN_REF &&
		    where->pLeft->pLeft == NULL && where->pLeft->pRight == NULL &&
		    where->pLeft->iTable == source->iCursor &&
		    where->pLeft->iColumn >= 0) {
			for (uint32_t i = 0; i < pk->part_count; ++i) {
				if ((uint32_t)where->pLeft->iColumn == pk->parts[i].fieldno) {
					is_primary_key_part = true;
					break;
				}
			}
		}
		if ((where->op == TK_NOTNULL || where->op == TK_ISNULL) &&
		    where->pLeft != NULL &&
		    where->pRight == NULL && where->pLeft->op == TK_COLUMN_REF &&
		    where->pLeft->pLeft == NULL && where->pLeft->pRight == NULL &&
		    where->pLeft->iTable == source->iCursor &&
		    is_primary_key_part) {
			if (where->op == TK_NOTNULL) {
				/* Tarantool primary-key fields are non-null. This predicate is
				 * an identity and the full-scan path preserves it. */
				primary_key_not_null = true;
			} else {
				/* A primary-key field can never be SQL NULL. */
				primary_key_is_null = true;
			}
		}
		if (primary_key_not_null)
			goto predicate_parsed;
		if (primary_key_is_null) {
			force_empty = true;
			goto predicate_parsed;
		}
		const struct Expr *exprs[2] = {select->pWhere, NULL};
		size_t expr_count = 1;
		if (select->pWhere->op == TK_AND && select->pWhere->pLeft != NULL &&
		    select->pWhere->pRight != NULL) {
			exprs[0] = select->pWhere->pLeft;
			exprs[1] = select->pWhere->pRight;
			expr_count = 2;
		}
		if (pk->part_count != 1 ||
		    (pk->parts[0].type != FIELD_TYPE_INTEGER &&
		     pk->parts[0].type != FIELD_TYPE_UNSIGNED))
			goto invalid_predicate;
		struct parsed_pk_bound parsed[2];
		for (size_t i = 0; i < expr_count; ++i)
			if (!parse_pk_bound(exprs[i], source->iCursor, primary_field,
					    unsigned_point, &parsed[i]))
				goto invalid_predicate;
		if (expr_count == 1 && parsed[0].op == SQL_PLAN_EQ) {
			has_point_key = true;
			if (unsigned_point)
				unsigned_point_key = parsed[0].unsigned_key;
			else
				point_key = parsed[0].signed_key;
		} else if (expr_count == 1) {
			has_range_key = true;
			range_op = parsed[0].op;
			if (unsigned_point)
				unsigned_range_key = parsed[0].unsigned_key;
			else
				range_key = parsed[0].signed_key;
		} else {
			if (parsed[0].op == SQL_PLAN_EQ || parsed[1].op == SQL_PLAN_EQ)
				goto invalid_predicate;
			int lower = (parsed[0].op == SQL_PLAN_GT ||
				     parsed[0].op == SQL_PLAN_GE) ? 0 : 1;
			int upper = 1 - lower;
			if ((parsed[lower].op != SQL_PLAN_GT &&
			     parsed[lower].op != SQL_PLAN_GE) ||
			    (parsed[upper].op != SQL_PLAN_LT &&
			     parsed[upper].op != SQL_PLAN_LE))
				goto invalid_predicate;
			has_range_key = has_range_end_key = true;
			range_op = parsed[lower].op;
			range_end_op = parsed[upper].op;
			if (unsigned_point) {
				unsigned_range_key = parsed[lower].unsigned_key;
				unsigned_range_end_key = parsed[upper].unsigned_key;
			} else {
				range_key = parsed[lower].signed_key;
				range_end_key = parsed[upper].signed_key;
			}
		}
	}
	goto predicate_parsed;
invalid_predicate:
	if (reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
	return NULL;
predicate_parsed:
	;
	if (force_empty) {
		finalize = (struct sql_plan_finalize) {
			.kind = SQL_PLAN_LIMIT,
			.limit = 0,
			.offset = 0,
		};
	}
	enum sql_plan_direction direction = SQL_PLAN_ASC;
	if (has_range_key)
		direction = range_op == SQL_PLAN_LT || range_op == SQL_PLAN_LE ?
			SQL_PLAN_DESC : SQL_PLAN_ASC;
	struct sql_plan_order_term order_term;
	if (select->pOrderBy != NULL) {
		const struct ExprList *order_by = select->pOrderBy;
		const struct Expr *order_expr = order_by->nExpr == 1 ?
			order_by->a[0].pExpr : NULL;
		if (source->space->index_map[0]->def->key_def->part_count != 1) {
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			return NULL;
		}
		uint32_t primary_field = source->space->index_map[0]->def->key_def->
			parts[0].fieldno;
		if (order_expr == NULL ||
		    ExprHasProperty(order_expr, EP_TokenOnly | EP_Reduced) ||
		    order_expr->op != TK_COLUMN_REF || order_expr->pLeft != NULL ||
		    order_expr->pRight != NULL || order_expr->iTable != source->iCursor ||
		    order_expr->iColumn < 0 ||
		    (uint32_t)order_expr->iColumn != primary_field ||
		    (order_by->a[0].sort_order != SORT_ORDER_UNDEF &&
		     order_by->a[0].sort_order != SORT_ORDER_ASC &&
		     order_by->a[0].sort_order != SORT_ORDER_DESC)) {
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			return NULL;
		}
		direction = order_by->a[0].sort_order == SORT_ORDER_DESC ?
			SQL_PLAN_DESC : SQL_PLAN_ASC;
		if (has_range_key && !has_range_end_key &&
		    direction != (range_op == SQL_PLAN_LT ||
				  range_op == SQL_PLAN_LE ? SQL_PLAN_DESC :
				  SQL_PLAN_ASC)) {
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			return NULL;
		}
		order_term = (struct sql_plan_order_term) {
			.column = primary_field,
			.direction = direction,
		};
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
	struct sql_plan_expression point_expressions[2] = {
		{
			.id = 1,
			.canonical = has_range_key ? (unsigned_point ?
				"unsigned-range-key" : "integer-range-key") :
				"integer-point-key",
		},
		{
			.id = 2,
			.canonical = unsigned_point ? "unsigned-range-end-key" :
				"integer-range-end-key",
		},
	};
	struct sql_plan_bound point_bounds[2] = {
		{
			.side = has_range_key && (range_op == SQL_PLAN_LT ||
				range_op == SQL_PLAN_LE) ? SQL_PLAN_UPPER : SQL_PLAN_LOWER,
			.op = has_range_key ? range_op : SQL_PLAN_EQ,
			.expr_ref = 1,
		},
		{
			.side = SQL_PLAN_UPPER,
			.op = range_end_op,
			.expr_ref = 2,
		},
	};
	/*
	 * Keep the original single-bound encoding for point and one-sided routes;
	 * bounded ranges add one independently-owned expression/bound.
	 */
	struct sql_plan_expression point_expression = {
		.id = 1,
		.canonical = has_range_key ? (unsigned_point ? "unsigned-range-key" :
			"integer-range-key") :
			"integer-point-key",
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.finalize = select->pLimit == NULL && select->pOffset == NULL &&
			!force_empty ?
			NULL : &finalize,
		.finalize_count = select->pLimit == NULL && select->pOffset == NULL ?
			(force_empty ? 1 : 0) : 1,
		.space_id = source->space->def->id,
		.space_name = source->space->def->name,
		.access = {
			.kind = has_point_key ? SQL_PLAN_PK_POINT_LOOKUP :
				has_range_key ? SQL_PLAN_INDEX_RANGE_SCAN :
				SQL_PLAN_TABLE_FULL_SCAN,
			.bounds = has_point_key || has_range_key ? point_bounds : NULL,
			.bound_count = has_range_key ? (has_range_end_key ? 2 : 1) :
				has_point_key ? 1 : 0,
			.has_integer_point_key = has_point_key && !unsigned_point,
			.integer_point_key = point_key,
			.has_unsigned_point_key = has_point_key && unsigned_point,
			.unsigned_point_key = unsigned_point_key,
			.has_integer_range_key = has_range_key && !unsigned_point,
			.integer_range_key = range_key,
			.has_unsigned_range_key = has_range_key && unsigned_point,
			.unsigned_range_key = unsigned_range_key,
			.integer_range_op = range_op,
			.has_integer_range_end_key = has_range_end_key &&
				!unsigned_point,
			.integer_range_end_key = range_end_key,
			.has_unsigned_range_end_key = has_range_end_key &&
				unsigned_point,
			.unsigned_range_end_key = unsigned_range_end_key,
			.integer_range_end_op = range_end_op,
			.range_key_column = primary_field,
			.direction = direction,
			.produced_order = select->pOrderBy == NULL ? NULL : &order_term,
			.produced_order_count = select->pOrderBy == NULL ? 0 : 1,
			.projected_columns = columns,
			.projected_column_count = select->pEList->nExpr,
			.est_rows = has_point_key ? 1 : has_range_key ?
				estimate->rows / 2 : estimate->rows,
			.est_rows_confidence = estimate->confidence,
		},
		.projection_columns = columns,
		.projection_column_count = select->pEList->nExpr,
		.expressions = has_point_key || has_range_key ?
			(has_range_end_key ? point_expressions : &point_expression) : NULL,
		.expression_count = has_range_end_key ? 2 :
			(has_point_key || has_range_key ? 1 : 0),
		.cost_startup = estimate->startup_cost,
		.cost_total = has_point_key ? 1 : has_range_key ?
			estimate->total_cost / 2 : estimate->total_cost,
		.cost_rows = has_point_key ? 1 : has_range_key ?
			estimate->rows / 2 : estimate->rows,
		.cost_row_width = estimate->row_width,
		.cost_confidence = estimate->confidence,
	};
	struct sql_plan_descriptor *plan = sql_plan_descriptor_new(&input);
	free(columns);
	if (plan == NULL && reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
	return plan;
}
