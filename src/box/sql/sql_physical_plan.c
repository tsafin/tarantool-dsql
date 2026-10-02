#include "sql_physical_plan.h"

#include <errno.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "sqlInt.h"
#include "sql_expr_canonical.h"
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

static bool
is_supported_filter_constant(const struct Expr *expr)
{
	if (expr == NULL || !sqlExprIsConstant((struct Expr *)expr))
		return false;
	enum sql_expr_canonical_reject reason;
	char *canonical = sql_expr_canonicalize(expr, NULL, 0, &reason);
	free(canonical);
	return reason == SQL_EXPR_CANONICAL_OK;
}

static bool
is_source_column(const struct Expr *expr, int cursor, uint32_t field_count)
{
	return expr != NULL && expr->op == TK_COLUMN_REF &&
		expr->pLeft == NULL && expr->pRight == NULL &&
		expr->iTable == cursor && expr->iColumn >= 0 &&
		(uint32_t)expr->iColumn < field_count;
}

static bool
has_only_source_columns(const struct Expr *expr, int cursor,
			uint32_t field_count, size_t depth)
{
	if (expr == NULL || depth >= SQL_PLAN_POINT_KEY_PART_MAX ||
	    ExprHasProperty(expr, EP_TokenOnly | EP_Reduced | EP_xIsSelect))
		return false;
	if (expr->op == TK_COLUMN_REF)
		return is_source_column(expr, cursor, field_count);
	if (expr->op == TK_IN || expr->op == TK_BETWEEN) {
		if (expr->pRight != NULL || expr->x.pList == NULL ||
		    expr->x.pList->nExpr <= 0 ||
		    !has_only_source_columns(expr->pLeft, cursor, field_count,
					    depth + 1))
			return false;
		for (int i = 0; i < expr->x.pList->nExpr; ++i) {
			if (!has_only_source_columns(expr->x.pList->a[i].pExpr,
						     cursor, field_count,
						     depth + 1))
				return false;
		}
		return true;
	}
	if (expr->op == TK_FUNCTION) {
		if (!ExprHasProperty(expr, EP_ConstFunc) || expr->pLeft != NULL ||
		    expr->pRight != NULL)
			return false;
		for (int i = 0; expr->x.pList != NULL &&
		     i < expr->x.pList->nExpr; ++i) {
			if (!has_only_source_columns(expr->x.pList->a[i].pExpr,
						     cursor, field_count,
						     depth + 1))
				return false;
		}
		return true;
	}
	if (expr->pLeft == NULL && expr->pRight == NULL)
		return sqlExprIsConstant((struct Expr *)expr);
	return (expr->pLeft == NULL ||
		has_only_source_columns(expr->pLeft, cursor, field_count,
					depth + 1)) &&
	       (expr->pRight == NULL ||
		has_only_source_columns(expr->pRight, cursor, field_count,
					depth + 1));
}

static bool
has_source_column(const struct Expr *expr, int cursor, uint32_t field_count,
		  size_t depth)
{
	if (expr == NULL || depth >= SQL_PLAN_POINT_KEY_PART_MAX)
		return false;
	if (is_source_column(expr, cursor, field_count))
		return true;
	if (expr->op == TK_IN || expr->op == TK_BETWEEN) {
		if (has_source_column(expr->pLeft, cursor, field_count, depth + 1))
			return true;
		if (expr->x.pList != NULL) {
			for (int i = 0; i < expr->x.pList->nExpr; ++i) {
				if (has_source_column(expr->x.pList->a[i].pExpr,
						     cursor, field_count,
						     depth + 1))
					return true;
			}
		}
		return false;
	}
	if (expr->op == TK_FUNCTION) {
		for (int i = 0; expr->x.pList != NULL &&
		     i < expr->x.pList->nExpr; ++i) {
			if (has_source_column(expr->x.pList->a[i].pExpr, cursor,
					      field_count, depth + 1))
				return true;
		}
		return false;
	}
	return has_source_column(expr->pLeft, cursor, field_count, depth + 1) ||
		has_source_column(expr->pRight, cursor, field_count, depth + 1);
}

static bool
is_like_filter(const struct Expr *expr, int cursor, uint32_t field_count)
{
	if (expr == NULL || expr->op != TK_FUNCTION ||
	    !ExprHasProperty(expr, EP_ConstFunc) || expr->u.zToken == NULL ||
	    expr->pLeft != NULL || expr->pRight != NULL || expr->x.pList == NULL ||
	    expr->x.pList->nExpr < 2 || expr->x.pList->nExpr > 3)
		return false;
	const char *name = expr->u.zToken;
	if (!((name[0] == 'l' || name[0] == 'L') &&
	      (name[1] == 'i' || name[1] == 'I') &&
	      (name[2] == 'k' || name[2] == 'K') &&
	      (name[3] == 'e' || name[3] == 'E') && name[4] == '\0'))
		return false;
	bool has_source = false;
	for (int i = 0; i < expr->x.pList->nExpr; ++i) {
		const struct Expr *arg = expr->x.pList->a[i].pExpr;
		if (!has_only_source_columns(arg, cursor, field_count, 0))
			return false;
		has_source |= has_source_column(arg, cursor, field_count, 0);
	}
	return has_source;
}

static bool
is_supported_boolean_filter(const struct Expr *expr, int cursor,
			    uint32_t field_count, size_t depth)
{
	if (expr == NULL || depth >= SQL_PLAN_POINT_KEY_PART_MAX)
		return false;
	if ((expr->op == TK_AND || expr->op == TK_OR) &&
	    expr->pLeft != NULL && expr->pRight != NULL)
		return is_supported_boolean_filter(expr->pLeft, cursor, field_count,
						   depth + 1) &&
			is_supported_boolean_filter(expr->pRight, cursor, field_count,
						     depth + 1);
	if (expr->op == TK_NOT && expr->pLeft != NULL && expr->pRight == NULL)
		return is_supported_boolean_filter(expr->pLeft, cursor, field_count,
						   depth + 1);
	if (is_like_filter(expr, cursor, field_count))
		return true;
	if ((expr->op == TK_ISNULL || expr->op == TK_NOTNULL) &&
	    expr->pRight == NULL)
		return has_only_source_columns(expr->pLeft, cursor, field_count, 0);
	if (expr->op == TK_BETWEEN && expr->pRight == NULL &&
	    expr->x.pList != NULL && expr->x.pList->nExpr == 2 &&
	    has_only_source_columns(expr->pLeft, cursor, field_count, 0) &&
	    has_source_column(expr->pLeft, cursor, field_count, 0))
		return (is_supported_filter_constant(expr->x.pList->a[0].pExpr) ||
			(has_only_source_columns(expr->x.pList->a[0].pExpr,
						 cursor, field_count, 0) &&
			 has_source_column(expr->x.pList->a[0].pExpr, cursor,
					    field_count, 0))) &&
		       (is_supported_filter_constant(expr->x.pList->a[1].pExpr) ||
			(has_only_source_columns(expr->x.pList->a[1].pExpr,
						 cursor, field_count, 0) &&
			 has_source_column(expr->x.pList->a[1].pExpr, cursor,
					    field_count, 0)));
	if (expr->op == TK_IN && expr->pRight == NULL &&
	    expr->x.pList != NULL && expr->x.pList->nExpr > 0 &&
	    !ExprHasProperty(expr, EP_xIsSelect) &&
	    has_only_source_columns(expr->pLeft, cursor, field_count, 0) &&
	    has_source_column(expr->pLeft, cursor, field_count, 0)) {
		for (int i = 0; i < expr->x.pList->nExpr; ++i) {
			const struct Expr *item = expr->x.pList->a[i].pExpr;
			if (!is_supported_filter_constant(item) &&
			    !(has_only_source_columns(item, cursor, field_count, 0) &&
			      has_source_column(item, cursor, field_count, 0)))
				return false;
		}
		return true;
	}
	if (expr->op != TK_EQ && expr->op != TK_NE && expr->op != TK_GT &&
	    expr->op != TK_GE && expr->op != TK_LT && expr->op != TK_LE)
		return false;
	if (expr->pLeft == NULL || expr->pRight == NULL)
		return false;
	if (has_only_source_columns(expr->pLeft, cursor, field_count, 0) &&
	    has_only_source_columns(expr->pRight, cursor, field_count, 0) &&
	    (has_source_column(expr->pLeft, cursor, field_count, 0) ||
	     has_source_column(expr->pRight, cursor, field_count, 0)))
		return true;
	return (is_source_column(expr->pLeft, cursor, field_count) &&
		is_supported_filter_constant(expr->pRight)) ||
	       (is_source_column(expr->pRight, cursor, field_count) &&
		is_supported_filter_constant(expr->pLeft));
}

struct parsed_pk_bound {
	enum sql_plan_bound_op op;
	bool is_unsigned;
	int64_t signed_key;
	uint64_t unsigned_key;
};

static bool
collect_and_terms(const struct Expr *expr, const struct Expr **terms,
		  size_t capacity, size_t *count, size_t depth)
{
	if (expr == NULL || terms == NULL || count == NULL || depth >= capacity)
		return false;
	if (expr->op == TK_AND && expr->pLeft != NULL && expr->pRight != NULL)
		return collect_and_terms(expr->pLeft, terms, capacity, count,
					 depth + 1) &&
			collect_and_terms(expr->pRight, terms, capacity, count,
					  depth + 1);
	if (*count == capacity)
		return false;
	terms[(*count)++] = expr;
	return true;
}

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
	bool unary = negated || value->op == TK_UPLUS;
	const struct Expr *literal = unary ? value->pLeft : value;
	if ((unary && value->pRight != NULL) || literal == NULL ||
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
		/* INTEGER fields and indexes accept unsigned MsgPack integers too.
		 * Equality access must therefore keep a positive literal above
		 * INT64_MAX in unsigned form instead of letting sql_atoi64() wrap it
		 * through its signed output parameter.
		 */
		if (op == SQL_PLAN_EQ && !negated &&
		    (literal->flags & EP_IntValue) == 0 &&
		    literal->u.zToken != NULL && literal->u.zToken[0] != '-') {
			errno = 0;
			char *end;
			unsigned long long wide =
				strtoull(literal->u.zToken, &end, 10);
			if (errno == 0 && end != literal->u.zToken && *end == '\0' &&
			    wide > INT64_MAX) {
				result.is_unsigned = true;
				result.unsigned_key = (uint64_t)wide;
				*out = result;
				return true;
			}
		}
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

static int
compare_pk_bound_value(const struct parsed_pk_bound *a,
		       const struct parsed_pk_bound *b)
{
	if (a->is_unsigned) {
		if (a->unsigned_key < b->unsigned_key)
			return -1;
		return a->unsigned_key > b->unsigned_key ? 1 : 0;
	}
	if (a->signed_key < b->signed_key)
		return -1;
	return a->signed_key > b->signed_key ? 1 : 0;
}

static bool
pk_bound_is_stricter(const struct parsed_pk_bound *candidate,
		     const struct parsed_pk_bound *current, bool lower)
{
	int comparison = compare_pk_bound_value(candidate, current);
	if (lower && comparison != 0)
		return comparison > 0;
	if (!lower && comparison != 0)
		return comparison < 0;
	return lower ? candidate->op == SQL_PLAN_GT && current->op == SQL_PLAN_GE :
		candidate->op == SQL_PLAN_LT && current->op == SQL_PLAN_LE;
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
	bool has_multi_point_key = false;
	bool has_range_key = false;
	bool has_range_end_key = false;
	bool primary_key_not_null = false;
	bool primary_key_is_null = false;
	struct sql_plan_filter filters[SQL_PLAN_FILTER_MAX] = {{0}};
	const struct Expr *filter_expressions[SQL_PLAN_FILTER_MAX] = {0};
	char *filter_canonical[SQL_PLAN_FILTER_MAX] = {0};
	size_t filter_count = 0;
	int64_t point_key = 0;
	int64_t range_key = 0;
	int64_t range_end_key = 0;
	enum sql_plan_bound_op range_op = SQL_PLAN_EQ;
	enum sql_plan_bound_op range_end_op = SQL_PLAN_EQ;
	uint64_t unsigned_point_key = 0;
	uint32_t point_key_variable = 0;
	uint64_t unsigned_range_key = 0;
	uint64_t unsigned_range_end_key = 0;
	bool has_secondary_equality_scan = false;
	const struct Expr *secondary_scan_term = NULL;
	uint32_t secondary_index_id = 0;
	uint32_t secondary_key_column = 0;
	bool secondary_key_unsigned = false;
	int64_t secondary_signed_key = 0;
	uint64_t secondary_unsigned_key = 0;
	bool has_secondary_range_scan = false;
	bool has_secondary_prefix_scan = false;
	uint32_t secondary_prefix_index_id = 0;
	struct sql_plan_point_key_part secondary_prefix_parts[
		SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
	const struct Expr *secondary_prefix_terms[
		SQL_PLAN_POINT_KEY_PART_MAX] = {0};
	size_t secondary_prefix_count = 0;
	bool has_secondary_full_scan = false;
	bool secondary_full_scan_natural_order = false;
	uint32_t secondary_full_index_id = 0;
	uint32_t secondary_range_index_id = 0;
	uint32_t secondary_range_key_column = 0;
	bool secondary_range_unsigned = false;
	bool secondary_range_descending = false;
	struct parsed_pk_bound secondary_range_lower = {0};
	struct parsed_pk_bound secondary_range_upper = {0};
	bool has_secondary_range_lower = false;
	bool has_secondary_range_upper = false;
	const struct Expr *secondary_range_terms[2] = {0};
	size_t secondary_range_term_count = 0;
	struct sql_plan_point_key_part secondary_range_prefix_parts[
		SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
	const struct Expr *secondary_range_prefix_terms[
		SQL_PLAN_POINT_KEY_PART_MAX] = {0};
	size_t secondary_range_prefix_count = 0;
	struct sql_plan_point_key_part secondary_key_parts[
		SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
	const struct Expr *secondary_scan_terms[
		SQL_PLAN_POINT_KEY_PART_MAX] = {0};
	size_t secondary_key_part_count = 0;
	struct sql_plan_point_key_part composite_point_parts[
		SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
	struct sql_plan_point_key_part multi_point_values[
		SQL_PLAN_PK_MULTI_VALUE_MAX] = {{0}};
	size_t multi_point_count = 0;
	size_t composite_point_count = 0;
	bool has_composite_point = false;
	bool has_prefix_scan = false;
	bool has_prefix_range_scan = false;
	bool prefix_range_has_lower = false;
	bool prefix_range_has_upper = false;
	bool unsigned_point = false;
	bool range_unsigned = false;
	uint32_t primary_field = source->space->index_map[0]->def->key_def->
		parts[0].fieldno;
	uint32_t range_key_column = primary_field;
	const struct key_def *pk = source->space->index_map[0]->def->key_def;
	unsigned_point = pk->parts[0].type == FIELD_TYPE_UNSIGNED;
	range_unsigned = unsigned_point;
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
		if ((where->op == TK_NOTNULL || where->op == TK_ISNULL) &&
		    where->pLeft != NULL && where->pRight == NULL &&
		    where->pLeft->op == TK_COLUMN_REF &&
		    where->pLeft->pLeft == NULL && where->pLeft->pRight == NULL &&
		    where->pLeft->iTable == source->iCursor &&
		    where->pLeft->iColumn >= 0 &&
		    (uint32_t)where->pLeft->iColumn <
			source->space->def->field_count) {
			filters[filter_count++] = (struct sql_plan_filter) {
				.column = (uint32_t)where->pLeft->iColumn,
				.op = where->op == TK_ISNULL ? SQL_PLAN_FILTER_IS_NULL :
					SQL_PLAN_FILTER_IS_NOT_NULL,
				.selectivity = 0.5,
			};
			goto predicate_parsed;
		}
		const struct Expr *terms[SQL_PLAN_POINT_KEY_PART_MAX];
		const struct Expr *exprs[SQL_PLAN_POINT_KEY_PART_MAX];
		const struct Expr *expr_sources[SQL_PLAN_POINT_KEY_PART_MAX];
		struct Expr between_bounds[SQL_PLAN_POINT_KEY_PART_MAX];
		size_t term_count = 0;
		if (!collect_and_terms(select->pWhere, terms,
				       SQL_PLAN_POINT_KEY_PART_MAX, &term_count, 0))
			goto invalid_predicate;
		size_t expr_count = 0;
		for (size_t i = 0; i < term_count; ++i) {
			const struct Expr *term = terms[i];
			if (is_like_filter(term, source->iCursor,
					   source->space->def->field_count)) {
				if (filter_count == SQL_PLAN_FILTER_MAX)
					goto invalid_predicate;
				filters[filter_count] = (struct sql_plan_filter) {
					.op = SQL_PLAN_FILTER_EXPRESSION,
					.selectivity = 0.5,
				};
				filter_expressions[filter_count++] = term;
				continue;
			}
			if (term->op == TK_IN &&
			    is_supported_boolean_filter(term, source->iCursor,
							 source->space->def->field_count, 0) &&
			    has_only_source_columns(term->pLeft, source->iCursor,
						   source->space->def->field_count, 0) &&
			    has_source_column(term->pLeft, source->iCursor,
					      source->space->def->field_count, 0)) {
				bool is_pk_column = false;
				if (is_source_column(term->pLeft, source->iCursor,
					     source->space->def->field_count)) {
					for (uint32_t part = 0; part < pk->part_count; ++part)
						is_pk_column |=
							(uint32_t)term->pLeft->iColumn ==
							pk->parts[part].fieldno;
				}
				bool singleton_in = term->x.pList != NULL &&
					term->x.pList->nExpr == 1 &&
					!ExprHasProperty(term, EP_TokenOnly | EP_Reduced |
							 EP_xIsSelect) &&
					term->x.pList->a[0].pExpr != NULL;
				bool normalized_singleton_in = false;
				if (is_pk_column && !has_multi_point_key &&
				    pk->part_count == 1 &&
				    term->x.pList != NULL &&
				    term->x.pList->nExpr >= 2 &&
				    term->x.pList->nExpr <= SQL_PLAN_PK_MULTI_VALUE_MAX &&
				    !ExprHasProperty(term, EP_TokenOnly | EP_Reduced |
						     EP_xIsSelect)) {
					bool is_unsigned = pk->parts[0].type ==
						FIELD_TYPE_UNSIGNED;
					if (!is_unsigned && pk->parts[0].type !=
					    FIELD_TYPE_INTEGER)
						goto invalid_predicate;
					for (int value_no = 0;
					     value_no < term->x.pList->nExpr; ++value_no) {
						struct Expr equality = {
							.op = TK_EQ,
							.pLeft = term->pLeft,
							.pRight = term->x.pList->a[value_no].pExpr,
						};
						struct parsed_pk_bound parsed;
						if (equality.pRight == NULL ||
						    !parse_pk_bound(&equality, source->iCursor,
							pk->parts[0].fieldno, is_unsigned,
							&parsed) || parsed.op != SQL_PLAN_EQ ||
						    parsed.is_unsigned != is_unsigned)
							goto invalid_predicate;
						bool duplicate = false;
						for (size_t i = 0; i < multi_point_count; ++i)
							duplicate |= is_unsigned ?
								multi_point_values[i].unsigned_value ==
								parsed.unsigned_key :
								multi_point_values[i].integer_value ==
								parsed.signed_key;
						if (duplicate)
							continue;
						multi_point_values[multi_point_count++] =
							(struct sql_plan_point_key_part) {
								.column = pk->parts[0].fieldno,
								.is_unsigned = is_unsigned,
								.integer_value = parsed.signed_key,
								.unsigned_value = parsed.unsigned_key,
							};
					}
					if (multi_point_count == 0)
						goto invalid_predicate;
					/* Keep seeks in key order, and make the descriptor's
					 * result order deterministic for unordered IN queries. */
					for (size_t i = 1; i < multi_point_count; ++i) {
						struct sql_plan_point_key_part value =
							multi_point_values[i];
						size_t j = i;
						while (j > 0 && (is_unsigned ?
						       multi_point_values[j - 1].unsigned_value >
						       value.unsigned_value :
						       multi_point_values[j - 1].integer_value >
						       value.integer_value)) {
							multi_point_values[j] =
								multi_point_values[j - 1];
							--j;
						}
						multi_point_values[j] = value;
					}
					has_multi_point_key = has_point_key = true;
					continue;
				}
				if (singleton_in && expr_count <
				    SQL_PLAN_POINT_KEY_PART_MAX) {
					struct Expr *equality = &between_bounds[expr_count];
					*equality = (struct Expr) {
						.op = TK_EQ,
						.pLeft = term->pLeft,
						.pRight = term->x.pList->a[0].pExpr,
					};
					if (is_pk_column) {
						for (uint32_t part = 0; part < pk->part_count;
						     ++part) {
							bool is_unsigned = pk->parts[part].type ==
								FIELD_TYPE_UNSIGNED;
							struct parsed_pk_bound parsed;
							if ((!is_unsigned && pk->parts[part].type !=
							     FIELD_TYPE_INTEGER) ||
							    !parse_pk_bound(equality, source->iCursor,
								    pk->parts[part].fieldno,
								    is_unsigned, &parsed) ||
							    parsed.op != SQL_PLAN_EQ)
								continue;
							exprs[expr_count] = equality;
							expr_sources[expr_count++] = term;
							normalized_singleton_in = true;
							break;
						}
					} else if (!has_secondary_equality_scan) {
						for (uint32_t index_no = 1;
						     index_no < source->space->index_count;
						     ++index_no) {
							const struct index *index =
								source->space->index_map[index_no];
							if (index == NULL || index->def == NULL ||
							    index->def->type != TREE ||
							    index->def->key_def == NULL ||
							    index->def->key_def->part_count != 1)
								continue;
							const struct key_part *key_part =
								&index->def->key_def->parts[0];
							bool is_unsigned = key_part->type ==
								FIELD_TYPE_UNSIGNED;
							struct parsed_pk_bound parsed;
							if (key_part->fieldno !=
							    (uint32_t)term->pLeft->iColumn ||
							    (!is_unsigned && key_part->type !=
							     FIELD_TYPE_INTEGER) ||
							    !parse_pk_bound(equality, source->iCursor,
								    key_part->fieldno,
								    is_unsigned, &parsed) ||
							    parsed.op != SQL_PLAN_EQ)
								continue;
							secondary_index_id = index->def->iid;
							secondary_key_column = key_part->fieldno;
							secondary_key_unsigned = parsed.is_unsigned;
							secondary_signed_key = parsed.signed_key;
							secondary_unsigned_key = parsed.unsigned_key;
							secondary_scan_term = term;
							has_secondary_equality_scan = true;
							normalized_singleton_in = true;
							break;
						}
						if (!normalized_singleton_in) {
							for (uint32_t index_no = 1;
							     index_no < source->space->index_count;
							     ++index_no) {
								const struct index *index =
									source->space->index_map[index_no];
								if (index == NULL || index->def == NULL ||
								    index->def->type != TREE ||
								    index->def->key_def == NULL ||
								    index->def->key_def->part_count < 2)
									continue;
								const struct key_def *key_def =
									index->def->key_def;
								for (uint32_t part = 0;
								     part < key_def->part_count; ++part) {
									const struct key_part *key_part =
										&key_def->parts[part];
									if (key_part->fieldno !=
									    (uint32_t)term->pLeft->iColumn)
										continue;
									bool is_unsigned = key_part->type ==
										FIELD_TYPE_UNSIGNED;
									if (!is_unsigned && key_part->type !=
									    FIELD_TYPE_INTEGER)
										continue;
									bool complete_prefix = true;
									for (uint32_t prefix = 0;
									     prefix < part && complete_prefix;
									     ++prefix) {
										const struct key_part *prefix_part =
											&key_def->parts[prefix];
										bool prefix_unsigned =
											prefix_part->type ==
											FIELD_TYPE_UNSIGNED;
										if (!prefix_unsigned &&
										    prefix_part->type !=
										    FIELD_TYPE_INTEGER) {
											complete_prefix = false;
											break;
										}
										bool found = false;
										for (size_t source_no = 0;
										     source_no < term_count;
										     ++source_no) {
											struct parsed_pk_bound prefix_bound;
											if (parse_pk_bound(
												terms[source_no],
												source->iCursor,
												prefix_part->fieldno,
												prefix_unsigned,
												&prefix_bound) &&
											    prefix_bound.op ==
												SQL_PLAN_EQ) {
												found = true;
												break;
											}
										}
										complete_prefix &= found;
									}
									struct parsed_pk_bound parsed;
									if (!complete_prefix ||
									    !parse_pk_bound(equality,
										    source->iCursor,
										    key_part->fieldno,
										    is_unsigned,
										    &parsed) ||
									    parsed.op != SQL_PLAN_EQ)
										continue;
									exprs[expr_count] = equality;
									expr_sources[expr_count++] = term;
									normalized_singleton_in = true;
									break;
								}
								if (normalized_singleton_in)
									break;
							}
						}
					}
				}
				if (normalized_singleton_in) {
					if (!is_pk_column) {
						if (filter_count == SQL_PLAN_FILTER_MAX)
							goto invalid_predicate;
						filters[filter_count] = (struct sql_plan_filter) {
							.op = SQL_PLAN_FILTER_EXPRESSION,
							.selectivity = 0.5,
						};
						filter_expressions[filter_count++] = term;
					}
					continue;
				}
				if (!is_pk_column) {
					if (filter_count == SQL_PLAN_FILTER_MAX)
						goto invalid_predicate;
					filters[filter_count] = (struct sql_plan_filter) {
						.op = SQL_PLAN_FILTER_EXPRESSION,
						.selectivity = 0.5,
					};
					filter_expressions[filter_count++] = term;
					continue;
				}
			}
			if (term->op == TK_BETWEEN &&
			    is_supported_boolean_filter(term, source->iCursor,
							source->space->def->field_count, 0) &&
			    has_only_source_columns(term->pLeft, source->iCursor,
						   source->space->def->field_count, 0) &&
			    has_source_column(term->pLeft, source->iCursor,
					      source->space->def->field_count, 0)) {
				bool is_pk_column = false;
				if (is_source_column(term->pLeft, source->iCursor,
					     source->space->def->field_count)) {
					for (uint32_t part = 0; part < pk->part_count; ++part)
						is_pk_column |=
							(uint32_t)term->pLeft->iColumn ==
							pk->parts[part].fieldno;
				}
				if (!is_pk_column) {
					if (filter_count == SQL_PLAN_FILTER_MAX)
						goto invalid_predicate;
					filters[filter_count] = (struct sql_plan_filter) {
						.op = SQL_PLAN_FILTER_EXPRESSION,
						.selectivity = 0.5,
					};
					filter_expressions[filter_count++] = term;
					bool has_secondary_range_key = false;
					for (uint32_t index_no = 1;
					     !has_secondary_range_key &&
					     index_no < source->space->index_count;
					     ++index_no) {
						const struct index *index =
							source->space->index_map[index_no];
						if (index == NULL || index->def == NULL ||
						    index->def->type != TREE ||
						    index->def->key_def == NULL)
							continue;
						for (uint32_t part = 0;
						     part < index->def->key_def->part_count;
						     ++part) {
							const struct key_part *key_part =
								&index->def->key_def->parts[part];
							if (key_part->fieldno ==
							    (uint32_t)term->pLeft->iColumn &&
							    (key_part->type == FIELD_TYPE_INTEGER ||
							     key_part->type == FIELD_TYPE_UNSIGNED)) {
								has_secondary_range_key = true;
								break;
							}
						}
					}
					if (!has_secondary_range_key)
						continue;
				}
			}
			if (term->op != TK_BETWEEN) {
				if (expr_count == SQL_PLAN_POINT_KEY_PART_MAX)
					goto invalid_predicate;
				exprs[expr_count] = term;
				expr_sources[expr_count++] = term;
				continue;
			}
			/* BETWEEN is represented as x >= low AND x <= high by the
			 * SQL expression code generator. Normalize the same shape here;
			 * only literal bounds on a primary-key part will pass the existing
			 * bound parser below. Negated and malformed forms stay fail-closed.
			 */
			if (ExprHasProperty(term, EP_TokenOnly | EP_Reduced | EP_xIsSelect) ||
			    term->pLeft == NULL || term->x.pList == NULL ||
			    term->x.pList->nExpr != 2 ||
			    expr_count > SQL_PLAN_POINT_KEY_PART_MAX - 2)
				goto invalid_predicate;
			struct Expr *lower = &between_bounds[expr_count];
			struct Expr *upper = &between_bounds[expr_count + 1];
			*lower = (struct Expr) {
				.op = TK_GE,
				.pLeft = term->pLeft,
				.pRight = term->x.pList->a[0].pExpr,
			};
			*upper = (struct Expr) {
				.op = TK_LE,
				.pLeft = term->pLeft,
				.pRight = term->x.pList->a[1].pExpr,
			};
			exprs[expr_count] = lower;
			expr_sources[expr_count++] = term;
			exprs[expr_count] = upper;
			expr_sources[expr_count++] = term;
			continue;
		}
		/* A composite secondary access is eligible only when every key part
		 * has a compatible literal equality. Keep the chosen source terms so
		 * they can be elided only if this access path wins. */
		if (source->space->index_map != NULL) {
			for (uint32_t index_no = 1;
			     index_no < source->space->index_count; ++index_no) {
				const struct index *index =
					source->space->index_map[index_no];
				if (index == NULL || index->def == NULL ||
				    index->def->type != TREE || index->def->key_def == NULL ||
				    index->def->key_def->part_count < 2 ||
				    index->def->key_def->part_count >
					SQL_PLAN_POINT_KEY_PART_MAX)
					continue;
				struct sql_plan_point_key_part candidate_parts[
					SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
				const struct Expr *candidate_terms[
					SQL_PLAN_POINT_KEY_PART_MAX] = {0};
				bool complete = true;
				for (uint32_t part = 0;
				     part < index->def->key_def->part_count; ++part) {
					const struct key_part *key_part =
						&index->def->key_def->parts[part];
					bool is_unsigned = key_part->type == FIELD_TYPE_UNSIGNED;
					if (!is_unsigned && key_part->type != FIELD_TYPE_INTEGER) {
						complete = false;
						break;
					}
					for (size_t term_no = 0; term_no < expr_count; ++term_no) {
						const struct Expr *term = exprs[term_no];
						if (term->op != TK_EQ)
							continue;
						struct parsed_pk_bound parsed;
						if (!parse_pk_bound(term, source->iCursor,
								    key_part->fieldno,
								    is_unsigned, &parsed) ||
						    parsed.op != SQL_PLAN_EQ)
							continue;
						candidate_terms[part] = expr_sources[term_no];
						candidate_parts[part] =
							(struct sql_plan_point_key_part) {
							.column = key_part->fieldno,
							.is_unsigned = is_unsigned,
							.integer_value = parsed.signed_key,
							.unsigned_value = parsed.unsigned_key,
						};
						break;
					}
					if (candidate_terms[part] == NULL) {
						complete = false;
						break;
					}
				}
				if (!complete)
					continue;
				secondary_index_id = index->def->iid;
				secondary_key_part_count =
					index->def->key_def->part_count;
				memcpy(secondary_key_parts, candidate_parts,
				       secondary_key_part_count * sizeof(candidate_parts[0]));
				memcpy(secondary_scan_terms, candidate_terms,
				       secondary_key_part_count * sizeof(candidate_terms[0]));
				has_secondary_equality_scan = true;
				secondary_scan_term = secondary_scan_terms[0];
				secondary_key_column = secondary_key_parts[0].column;
				secondary_key_unsigned = secondary_key_parts[0].is_unsigned;
				secondary_signed_key = secondary_key_parts[0].integer_value;
				secondary_unsigned_key = secondary_key_parts[0].unsigned_value;
				break;
			}
		}
		bool has_secondary_equality_candidate =
			has_secondary_equality_scan;
		for (uint32_t index_no = 1;
		     !has_secondary_equality_candidate &&
		     index_no < source->space->index_count; ++index_no) {
			const struct index *index = source->space->index_map[index_no];
			if (index == NULL || index->def == NULL ||
			    index->def->type != TREE || index->def->key_def == NULL ||
			    index->def->key_def->part_count != 1)
				continue;
			const struct key_part *key_part =
				&index->def->key_def->parts[0];
			bool is_unsigned = key_part->type == FIELD_TYPE_UNSIGNED;
			if (!is_unsigned && key_part->type != FIELD_TYPE_INTEGER)
				continue;
			for (size_t term_no = 0; term_no < expr_count; ++term_no) {
				struct parsed_pk_bound parsed;
				if (exprs[term_no]->op == TK_EQ &&
				    parse_pk_bound(exprs[term_no], source->iCursor,
						   key_part->fieldno, is_unsigned,
						   &parsed) && parsed.op == SQL_PLAN_EQ) {
					has_secondary_equality_candidate = true;
					break;
				}
			}
		}
		if (!has_secondary_equality_candidate &&
		    source->space->index_map != NULL) {
			for (uint32_t index_no = 1;
			     index_no < source->space->index_count; ++index_no) {
				const struct index *index =
					source->space->index_map[index_no];
				if (index == NULL || index->def == NULL ||
				    index->def->type != TREE || index->def->key_def == NULL ||
				    index->def->key_def->part_count == 0 ||
				    index->def->key_def->part_count >
					SQL_PLAN_POINT_KEY_PART_MAX)
					continue;
				const struct key_def *key_def = index->def->key_def;
				for (uint32_t range_part_no = 0;
				     range_part_no < key_def->part_count; ++range_part_no) {
					struct sql_plan_point_key_part prefix_parts[
						SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
					const struct Expr *prefix_terms[
						SQL_PLAN_POINT_KEY_PART_MAX] = {0};
					bool complete_prefix = true;
					for (uint32_t part_no = 0;
					     part_no < range_part_no; ++part_no) {
						const struct key_part *part =
							&key_def->parts[part_no];
						bool part_unsigned = part->type ==
							FIELD_TYPE_UNSIGNED;
						if (!part_unsigned && part->type !=
						    FIELD_TYPE_INTEGER) {
							complete_prefix = false;
							break;
						}
						for (size_t term_no = 0; term_no < expr_count;
						     ++term_no) {
							const struct Expr *term = exprs[term_no];
							struct parsed_pk_bound parsed;
							if (!parse_pk_bound(term, source->iCursor,
									    part->fieldno,
									    part_unsigned,
									    &parsed) ||
							    parsed.op != SQL_PLAN_EQ)
								continue;
							prefix_terms[part_no] = expr_sources[term_no];
							prefix_parts[part_no] =
								(struct sql_plan_point_key_part) {
								.column = part->fieldno,
								.is_unsigned = part_unsigned,
								.integer_value = parsed.signed_key,
								.unsigned_value = parsed.unsigned_key,
							};
							break;
						}
						if (prefix_terms[part_no] == NULL) {
							complete_prefix = false;
							break;
						}
					}
					if (!complete_prefix)
						continue;
					const struct key_part *key_part =
						&key_def->parts[range_part_no];
					bool is_unsigned = key_part->type ==
						FIELD_TYPE_UNSIGNED;
					if ((!is_unsigned && key_part->type !=
					     FIELD_TYPE_INTEGER) ||
					    (key_part->sort_order != SORT_ORDER_ASC &&
					     key_part->sort_order != SORT_ORDER_DESC))
						continue;
					bool has_lower = false;
					bool has_upper = false;
					for (size_t term_no = 0; term_no < expr_count;
					     ++term_no) {
						const struct Expr *term = exprs[term_no];
						struct parsed_pk_bound parsed;
						if (!parse_pk_bound(term, source->iCursor,
								    key_part->fieldno, is_unsigned,
								    &parsed) ||
						    parsed.op == SQL_PLAN_EQ)
							continue;
						if (parsed.op == SQL_PLAN_GT ||
						    parsed.op == SQL_PLAN_GE) {
							if (!has_lower || pk_bound_is_stricter(&parsed,
										   &secondary_range_lower,
										   true)) {
								secondary_range_lower = parsed;
								secondary_range_terms[0] =
									expr_sources[term_no];
							}
							has_lower = true;
						} else if (parsed.op == SQL_PLAN_LT ||
							   parsed.op == SQL_PLAN_LE) {
							if (!has_upper || pk_bound_is_stricter(&parsed,
										   &secondary_range_upper,
										   false)) {
								secondary_range_upper = parsed;
								secondary_range_terms[1] =
									expr_sources[term_no];
							}
							has_upper = true;
						}
					}
					if (!has_lower && !has_upper)
						continue;
					has_secondary_range_scan = true;
					has_secondary_range_lower = has_lower;
					has_secondary_range_upper = has_upper;
					secondary_range_index_id = index->def->iid;
					secondary_range_key_column = key_part->fieldno;
					secondary_range_unsigned = is_unsigned;
					secondary_range_descending =
						key_part->sort_order == SORT_ORDER_DESC;
					secondary_range_prefix_count = range_part_no;
					memcpy(secondary_range_prefix_parts, prefix_parts,
					       range_part_no * sizeof(prefix_parts[0]));
					memcpy(secondary_range_prefix_terms, prefix_terms,
					       range_part_no * sizeof(prefix_terms[0]));
					secondary_range_term_count = (has_lower ? 1 : 0) +
						(has_upper ? 1 : 0);
					if (!has_lower) {
						secondary_range_terms[0] =
							secondary_range_terms[1];
						secondary_range_terms[1] = NULL;
					}
					break;
				}
				if (has_secondary_range_scan)
					break;
			}
		}
		if (!has_secondary_equality_candidate && !has_secondary_range_scan &&
		    source->space->index_map != NULL) {
			bool saved_prefix_candidate = false;
			for (uint32_t index_no = 1;
			     index_no < source->space->index_count; ++index_no) {
				const struct index *index =
					source->space->index_map[index_no];
				if (index == NULL || index->def == NULL ||
				    index->def->type != TREE || index->def->key_def == NULL ||
				    index->def->key_def->part_count < 2 ||
				    index->def->key_def->part_count >
					SQL_PLAN_POINT_KEY_PART_MAX)
					continue;
				const struct key_def *key_def = index->def->key_def;
				struct sql_plan_point_key_part parts[
					SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
				const struct Expr *terms[
					SQL_PLAN_POINT_KEY_PART_MAX] = {0};
				size_t prefix_count = 0;
				for (uint32_t part_no = 0;
				     part_no < key_def->part_count; ++part_no) {
					const struct key_part *part = &key_def->parts[part_no];
					bool is_unsigned = part->type == FIELD_TYPE_UNSIGNED;
					if (!is_unsigned && part->type != FIELD_TYPE_INTEGER)
						break;
					for (size_t term_no = 0; term_no < expr_count; ++term_no) {
						const struct Expr *term = exprs[term_no];
						struct parsed_pk_bound parsed;
						if (!parse_pk_bound(term, source->iCursor,
								    part->fieldno, is_unsigned,
								    &parsed) ||
						    parsed.op != SQL_PLAN_EQ)
							continue;
						parts[part_no] = (struct sql_plan_point_key_part) {
							.column = part->fieldno,
							.is_unsigned = is_unsigned,
							.integer_value = parsed.signed_key,
							.unsigned_value = parsed.unsigned_key,
						};
						terms[part_no] = expr_sources[term_no];
						break;
					}
					if (terms[part_no] == NULL)
						break;
					++prefix_count;
				}
				if (prefix_count == 0 ||
				    prefix_count == key_def->part_count)
					continue;
				bool order_matches = select->pOrderBy == NULL;
				if (!order_matches && select->pOrderBy->nExpr > 0 &&
				    (uint32_t)select->pOrderBy->nExpr <=
				    key_def->part_count - prefix_count) {
					bool natural = true;
					bool reverse = true;
					order_matches = true;
					for (int i = 0; i < select->pOrderBy->nExpr; ++i) {
						const struct Expr *expr =
							select->pOrderBy->a[i].pExpr;
						const struct key_part *part = &key_def->parts[
							prefix_count + i];
						enum sort_order requested =
							select->pOrderBy->a[i].sort_order;
						if (requested == SORT_ORDER_UNDEF)
							requested = SORT_ORDER_ASC;
						if (expr == NULL || expr->op != TK_COLUMN_REF ||
						    expr->pLeft != NULL || expr->pRight != NULL ||
						    expr->iTable != source->iCursor ||
						    expr->iColumn < 0 ||
						    (uint32_t)expr->iColumn != part->fieldno ||
						    (part->sort_order != SORT_ORDER_ASC &&
						     part->sort_order != SORT_ORDER_DESC)) {
							order_matches = false;
							break;
						}
						natural &= requested == part->sort_order;
						reverse &= requested != part->sort_order;
					}
					order_matches &= natural || reverse;
				}
				if (!order_matches && saved_prefix_candidate)
					continue;
				has_secondary_prefix_scan = true;
				secondary_prefix_index_id = index->def->iid;
				secondary_prefix_count = prefix_count;
				memcpy(secondary_prefix_parts, parts,
				       prefix_count * sizeof(parts[0]));
				memcpy(secondary_prefix_terms, terms,
				       prefix_count * sizeof(terms[0]));
				if (order_matches)
					break;
				saved_prefix_candidate = true;
			}
		}
		size_t bound_count = 0;
		for (size_t i = 0; i < expr_count; ++i) {
			const struct Expr *term = exprs[i];
			bool selected_secondary_equality = false;
			if (has_secondary_equality_scan) {
				size_t count = secondary_key_part_count == 0 ? 1 :
					secondary_key_part_count;
				for (size_t j = 0; j < count; ++j) {
					const struct Expr *source_term =
						secondary_key_part_count == 0 ? secondary_scan_term :
						secondary_scan_terms[j];
					selected_secondary_equality |= expr_sources[i] ==
						source_term;
				}
			}
			bool selected_secondary_prefix = false;
			if (has_secondary_prefix_scan) {
				for (size_t j = 0; j < secondary_prefix_count; ++j)
					selected_secondary_prefix |= expr_sources[i] ==
						secondary_prefix_terms[j];
			}
			bool selected_secondary_range_bound = false;
			for (size_t j = 0; j < secondary_range_term_count; ++j)
				selected_secondary_range_bound |= has_secondary_range_scan &&
					expr_sources[i] == secondary_range_terms[j];
			if (selected_secondary_equality || selected_secondary_prefix ||
			    selected_secondary_range_bound)
				continue;
			if ((term->op == TK_OR || term->op == TK_NOT) &&
			    is_supported_boolean_filter(term, source->iCursor,
							source->space->def->field_count, 0)) {
				if (filter_count == SQL_PLAN_FILTER_MAX)
					goto invalid_predicate;
				filters[filter_count] = (struct sql_plan_filter) {
					.op = SQL_PLAN_FILTER_EXPRESSION,
					.selectivity = 0.5,
				};
				filter_expressions[filter_count++] = term;
				continue;
			}
			if (term->op == TK_ISNULL || term->op == TK_NOTNULL) {
				if (term->pLeft != NULL && term->pRight == NULL &&
				    !is_source_column(term->pLeft, source->iCursor,
						      source->space->def->field_count) &&
				    has_only_source_columns(term->pLeft,
							   source->iCursor,
							   source->space->def->field_count,
							   0)) {
					if (filter_count == SQL_PLAN_FILTER_MAX)
						goto invalid_predicate;
					filters[filter_count] = (struct sql_plan_filter) {
						.op = SQL_PLAN_FILTER_EXPRESSION,
						.selectivity = 0.5,
					};
					filter_expressions[filter_count++] = term;
					continue;
				}
				if (term->pLeft == NULL ||
				    term->pRight != NULL ||
				    term->pLeft->op != TK_COLUMN_REF ||
				    term->pLeft->pLeft != NULL ||
				    term->pLeft->pRight != NULL ||
				    term->pLeft->iTable != source->iCursor ||
				    term->pLeft->iColumn < 0 ||
				    (uint32_t)term->pLeft->iColumn >=
					source->space->def->field_count)
					goto invalid_predicate;
				bool is_pk_column = false;
				for (uint32_t part = 0; part < pk->part_count; ++part)
					is_pk_column |= (uint32_t)term->pLeft->iColumn ==
						pk->parts[part].fieldno;
				if (is_pk_column) {
					/* Primary-key fields are never NULL, so IS NOT NULL is
					 * redundant. Keep IS NULL fail-closed here: its empty-result
					 * simplification is not represented by the conjunction's
					 * candidate route yet. */
					if (term->op == TK_ISNULL)
						goto invalid_predicate;
					primary_key_not_null = true;
					continue;
				}
				if (filter_count == SQL_PLAN_FILTER_MAX)
					goto invalid_predicate;
				filters[filter_count++] =
					(struct sql_plan_filter) {
						.column = (uint32_t)term->pLeft->iColumn,
						.op = term->op == TK_ISNULL ?
							SQL_PLAN_FILTER_IS_NULL :
							SQL_PLAN_FILTER_IS_NOT_NULL,
						.selectivity = 0.5,
					};
				continue;
			}
			if (term->op == TK_EQ || term->op == TK_NE ||
			    term->op == TK_GT || term->op == TK_GE ||
			    term->op == TK_LT || term->op == TK_LE) {
				if (term->pLeft == NULL || term->pRight == NULL)
					goto invalid_predicate;
				bool is_selected_secondary_range_term = false;
				for (size_t i = 0; i < secondary_range_term_count; ++i)
					is_selected_secondary_range_term |=
						term == secondary_range_terms[i];
				if (is_selected_secondary_range_term)
					continue;
				bool is_selected_secondary_range_prefix_term = false;
				for (size_t i = 0; i < secondary_range_prefix_count; ++i)
					is_selected_secondary_range_prefix_term |=
						term == secondary_range_prefix_terms[i];
				if (is_selected_secondary_range_prefix_term)
					continue;
				bool is_selected_secondary_prefix_term = false;
				for (size_t i = 0; i < secondary_prefix_count; ++i)
					is_selected_secondary_prefix_term |=
						term == secondary_prefix_terms[i];
				if (has_secondary_prefix_scan &&
				    is_selected_secondary_prefix_term)
					continue;
				bool is_selected_secondary_term = false;
				for (size_t part = 0; part < secondary_key_part_count; ++part)
					is_selected_secondary_term |=
						term == secondary_scan_terms[part];
				if (is_selected_secondary_term)
					continue;
				if (term->op == TK_EQ && source->space->index_map != NULL) {
					const struct Expr *column = NULL;
					if (is_source_column(term->pLeft, source->iCursor,
							     source->space->def->field_count)) {
						column = term->pLeft;
					} else if (is_source_column(term->pRight,
								   source->iCursor,
								   source->space->def->field_count)) {
						column = term->pRight;
					}
					bool is_primary_column = false;
					if (column != NULL) {
						for (uint32_t part = 0; part < pk->part_count;
						     ++part)
							is_primary_column |=
								(uint32_t)column->iColumn ==
								pk->parts[part].fieldno;
					}
					if (column != NULL && !is_primary_column) {
						bool matched_secondary = false;
						for (uint32_t index_no = 1;
						     index_no < source->space->index_count;
						     ++index_no) {
							const struct index *index =
								source->space->index_map[index_no];
							if (index == NULL || index->def == NULL ||
							    index->def->type != TREE ||
							    index->def->key_def == NULL ||
							    index->def->key_def->part_count != 1 ||
							    index->def->key_def->parts[0].fieldno !=
									(uint32_t)column->iColumn)
								continue;
							enum field_type type = index->def->
								key_def->parts[0].type;
							bool is_unsigned = type ==
								FIELD_TYPE_UNSIGNED;
							if (!is_unsigned && type !=
							    FIELD_TYPE_INTEGER)
								continue;
							struct parsed_pk_bound parsed;
							bool parsed_ok = parse_pk_bound(term,
									source->iCursor,
									(uint32_t)column->iColumn,
									is_unsigned, &parsed);
							if (!parsed_ok ||
							    parsed.op != SQL_PLAN_EQ)
								continue;
							matched_secondary = true;
							if (!has_secondary_equality_scan) {
								has_secondary_equality_scan = true;
								secondary_scan_term = term;
								secondary_index_id = index->def->iid;
								secondary_key_column =
									(uint32_t)column->iColumn;
								secondary_key_unsigned = parsed.is_unsigned;
								secondary_signed_key = parsed.signed_key;
								secondary_unsigned_key =
									parsed.unsigned_key;
							} else {
								if (filter_count == SQL_PLAN_FILTER_MAX)
									goto invalid_predicate;
								filters[filter_count] =
									(struct sql_plan_filter) {
										.op = SQL_PLAN_FILTER_EXPRESSION,
										.selectivity = 0.5,
									};
								filter_expressions[filter_count++] = term;
							}
							break;
						}
						if (matched_secondary)
							continue;
					}
				}
				bool direct_column_comparison =
					is_source_column(term->pLeft, source->iCursor,
							 source->space->def->field_count) &&
					is_source_column(term->pRight, source->iCursor,
							 source->space->def->field_count);
				bool direct_column_constant_comparison =
					(is_source_column(term->pLeft, source->iCursor,
							  source->space->def->field_count) &&
					 is_supported_filter_constant(term->pRight)) ||
					(is_source_column(term->pRight, source->iCursor,
							  source->space->def->field_count) &&
					 is_supported_filter_constant(term->pLeft));
				bool computed_comparison = !direct_column_comparison &&
					!direct_column_constant_comparison &&
					has_only_source_columns(term->pLeft,
							       source->iCursor,
							       source->space->def->field_count,
							       0) &&
					has_only_source_columns(term->pRight,
							       source->iCursor,
							       source->space->def->field_count,
							       0) &&
					(has_source_column(term->pLeft, source->iCursor,
							   source->space->def->field_count, 0) ||
					 has_source_column(term->pRight, source->iCursor,
							   source->space->def->field_count, 0));
				bool computed_primary_comparison = false;
				if (computed_comparison) {
					for (uint32_t part = 0; part < pk->part_count; ++part) {
						computed_primary_comparison |=
							(is_source_column(term->pLeft,
							 source->iCursor,
							 source->space->def->field_count) &&
							 !has_source_column(term->pRight,
							 source->iCursor,
							 source->space->def->field_count, 0) &&
							 (uint32_t)term->pLeft->iColumn ==
							 pk->parts[part].fieldno) ||
							(is_source_column(term->pRight,
							 source->iCursor,
							 source->space->def->field_count) &&
							 !has_source_column(term->pLeft,
							 source->iCursor,
							 source->space->def->field_count, 0) &&
							 (uint32_t)term->pRight->iColumn ==
							 pk->parts[part].fieldno);
					}
				}
				/* Keep primary-key terms for typed access-path extraction even
				 * when the generic scalar-filter canonicalizer rejects a wide
				 * integer literal. parse_pk_bound() handles the full unsigned
				 * range directly.
				 */
				if (direct_column_comparison ||
				    (computed_comparison &&
				     !computed_primary_comparison)) {
					if (filter_count == SQL_PLAN_FILTER_MAX)
						goto invalid_predicate;
					filters[filter_count] = (struct sql_plan_filter) {
						.op = SQL_PLAN_FILTER_EXPRESSION,
						.selectivity = 0.5,
					};
					filter_expressions[filter_count++] = term;
					continue;
				}
				const struct Expr *column = NULL;
				const struct Expr *literal = NULL;
				if (term->pLeft->op == TK_COLUMN_REF) {
					column = term->pLeft;
					literal = term->pRight;
				} else if (term->pRight->op == TK_COLUMN_REF) {
					column = term->pRight;
					literal = term->pLeft;
				}
				if (column != NULL && is_supported_filter_constant(literal) &&
				    column->pLeft == NULL && column->pRight == NULL &&
				    column->iTable == source->iCursor &&
				    column->iColumn >= 0 &&
				    (uint32_t)column->iColumn < source->space->def->field_count) {
					bool is_pk_column = false;
					for (uint32_t part = 0; part < pk->part_count; ++part)
						is_pk_column |= (uint32_t)column->iColumn ==
							pk->parts[part].fieldno;
					if (!is_pk_column) {
						if (filter_count == SQL_PLAN_FILTER_MAX)
							goto invalid_predicate;
						filters[filter_count] = (struct sql_plan_filter) {
							.op = SQL_PLAN_FILTER_EXPRESSION,
							.selectivity = 0.5,
						};
						filter_expressions[filter_count++] = term;
						continue;
					}
				}
			}
			exprs[bound_count++] = term;
		}
		expr_count = bound_count;
		if (expr_count == 0 && filter_count == 0 &&
		    !has_point_key && !primary_key_not_null &&
		    !has_secondary_equality_scan &&
		    !has_secondary_range_scan && !has_secondary_prefix_scan)
			goto invalid_predicate;
		if (expr_count == 0)
			goto predicate_parsed;
		if (expr_count != 0 &&
		    pk->parts[0].type != FIELD_TYPE_INTEGER &&
		    pk->parts[0].type != FIELD_TYPE_UNSIGNED)
			goto invalid_predicate;
		struct parsed_pk_bound parsed[SQL_PLAN_POINT_KEY_PART_MAX];
		uint32_t parsed_part[SQL_PLAN_POINT_KEY_PART_MAX];
		for (size_t i = 0; i < expr_count; ++i)
			parsed_part[i] = UINT32_MAX;
		bool has_nonleading_term = false;
		for (size_t i = 0; i < expr_count && !has_nonleading_term; ++i) {
			for (uint32_t part = 1; part < pk->part_count; ++part) {
				bool is_unsigned = pk->parts[part].type ==
					FIELD_TYPE_UNSIGNED;
				struct parsed_pk_bound probe;
				if ((is_unsigned || pk->parts[part].type ==
				     FIELD_TYPE_INTEGER) &&
				    parse_pk_bound(exprs[i], source->iCursor,
					pk->parts[part].fieldno, is_unsigned, &probe)) {
					has_nonleading_term = true;
					break;
				}
			}
		}
		bool composite_equality = expr_count > 1 &&
			expr_count <= pk->part_count &&
			pk->part_count <= SQL_PLAN_POINT_KEY_PART_MAX;
		bool seen_parts[SQL_PLAN_POINT_KEY_PART_MAX] = {false};
		if (composite_equality) {
			for (size_t i = 0; i < expr_count; ++i) {
				bool found = false;
				for (uint32_t part = 0; part < expr_count; ++part) {
					const struct key_part *key_part = &pk->parts[part];
					bool is_unsigned = key_part->type ==
						FIELD_TYPE_UNSIGNED;
					if ((!is_unsigned && key_part->type !=
					     FIELD_TYPE_INTEGER) ||
					    !parse_pk_bound(exprs[i], source->iCursor,
						   key_part->fieldno, is_unsigned,
						   &parsed[i]))
						continue;
					if (found || parsed[i].op != SQL_PLAN_EQ ||
					    seen_parts[part]) {
						composite_equality = false;
						break;
					}
					found = true;
					parsed_part[i] = part;
				}
				if (!found)
					composite_equality = false;
				else
					seen_parts[parsed_part[i]] = true;
				if (!composite_equality)
					break;
			}
		}
		if (composite_equality) {
			for (size_t i = 0; i < expr_count; ++i) {
				struct sql_plan_point_key_part *part =
					&composite_point_parts[parsed_part[i]];
				part->column = pk->parts[parsed_part[i]].fieldno;
				part->is_unsigned = parsed[i].is_unsigned;
				if (part->is_unsigned)
					part->unsigned_value = parsed[i].unsigned_key;
				else
					part->integer_value = parsed[i].signed_key;
			}
			composite_point_count = expr_count;
			if (expr_count == pk->part_count)
				has_point_key = has_composite_point = true;
			else
				has_prefix_scan = true;
		} else if (pk->part_count > 1 && has_nonleading_term &&
			   expr_count <= SQL_PLAN_POINT_KEY_PART_MAX) {
			bool seen_equal[SQL_PLAN_POINT_KEY_PART_MAX] = {false};
			struct parsed_pk_bound equal_values[
				SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
			bool has_lower_part[SQL_PLAN_POINT_KEY_PART_MAX] = {false};
			bool has_upper_part[SQL_PLAN_POINT_KEY_PART_MAX] = {false};
			struct parsed_pk_bound lower_part[
				SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
			struct parsed_pk_bound upper_part[
				SQL_PLAN_POINT_KEY_PART_MAX] = {{0}};
			bool valid = true;
			for (size_t i = 0; i < expr_count && valid; ++i) {
				bool found = false;
				for (uint32_t part = 0; part < pk->part_count; ++part) {
					bool is_unsigned = pk->parts[part].type ==
						FIELD_TYPE_UNSIGNED;
					if ((!is_unsigned && pk->parts[part].type !=
					     FIELD_TYPE_INTEGER) ||
					    !parse_pk_bound(exprs[i], source->iCursor,
						pk->parts[part].fieldno,
						is_unsigned, &parsed[i]))
						continue;
					if (found) {
						valid = false;
						break;
					}
					found = true;
					parsed_part[i] = part;
				}
				if (!valid || !found) {
					valid = false;
					break;
				}
				uint32_t part = parsed_part[i];
				if (parsed[i].op == SQL_PLAN_EQ) {
					if (seen_equal[part]) {
						valid = false;
					} else {
						seen_equal[part] = true;
						equal_values[part] = parsed[i];
					}
					continue;
				}
				if (parsed[i].op == SQL_PLAN_GT ||
				    parsed[i].op == SQL_PLAN_GE) {
					if (!has_lower_part[part] ||
					    pk_bound_is_stricter(&parsed[i],
								&lower_part[part], true))
						lower_part[part] = parsed[i];
					has_lower_part[part] = true;
				} else {
					if (!has_upper_part[part] ||
					    pk_bound_is_stricter(&parsed[i],
								&upper_part[part], false))
						upper_part[part] = parsed[i];
					has_upper_part[part] = true;
				}
			}
			uint32_t bound_part = UINT32_MAX;
			for (uint32_t part = 0; valid && part < pk->part_count; ++part) {
				if (has_lower_part[part] || has_upper_part[part]) {
					bound_part = part;
					break;
				}
			}
			if (!valid || bound_part == UINT32_MAX)
				goto invalid_predicate;
			bool has_lower = has_lower_part[bound_part];
			bool has_upper = has_upper_part[bound_part];
			struct parsed_pk_bound lower = lower_part[bound_part];
			struct parsed_pk_bound upper = upper_part[bound_part];
			for (uint32_t part = 0; part < bound_part; ++part) {
				if (!seen_equal[part])
					goto invalid_predicate;
				composite_point_parts[part] =
					(struct sql_plan_point_key_part) {
						.column = pk->parts[part].fieldno,
						.is_unsigned = equal_values[part].is_unsigned,
						.integer_value = equal_values[part].signed_key,
						.unsigned_value = equal_values[part].unsigned_key,
					};
			}
			composite_point_count = bound_part;
			if (bound_part != 0) {
				has_prefix_scan = has_prefix_range_scan = true;
				prefix_range_has_lower = has_lower;
				prefix_range_has_upper = has_upper;
			}
			/* Only the earliest varying key part bounds a contiguous B-tree
			 * interval. Preserve every predicate on later parts (and equality
			 * predicates on the ranged part) as a residual expression.
			 */
			for (size_t i = 0; i < expr_count; ++i) {
				if (parsed_part[i] < bound_part ||
				    (parsed_part[i] == bound_part &&
				     parsed[i].op != SQL_PLAN_EQ))
					continue;
				if (filter_count == SQL_PLAN_FILTER_MAX)
					goto invalid_predicate;
				filters[filter_count] = (struct sql_plan_filter) {
					.op = SQL_PLAN_FILTER_EXPRESSION,
					.selectivity = 0.5,
				};
				filter_expressions[filter_count++] = exprs[i];
			}
			has_range_key = true;
			has_range_end_key = has_lower && has_upper;
			range_unsigned = pk->parts[bound_part].type ==
				FIELD_TYPE_UNSIGNED;
			range_key_column = pk->parts[bound_part].fieldno;
			if (has_lower) {
				range_op = lower.op;
				if (range_unsigned)
					unsigned_range_key = lower.unsigned_key;
				else
					range_key = lower.signed_key;
			} else {
				range_op = upper.op;
				if (range_unsigned)
					unsigned_range_key = upper.unsigned_key;
				else
					range_key = upper.signed_key;
			}
			if (has_range_end_key) {
				range_end_op = upper.op;
				if (range_unsigned)
					unsigned_range_end_key = upper.unsigned_key;
				else
					range_end_key = upper.signed_key;
			}
		} else {
			if (expr_count == 1 && pk->part_count == 1 &&
			    !unsigned_point) {
				const struct Expr *term = exprs[0];
				const struct Expr *column = term->pLeft;
				const struct Expr *value = term->pRight;
				if (column != NULL && value != NULL &&
				    column->op != TK_COLUMN_REF &&
				    value->op == TK_COLUMN_REF) {
					const struct Expr *tmp = column;
					column = value;
					value = tmp;
				}
				if (term->op == TK_EQ && column != NULL && value != NULL &&
				    column->op == TK_COLUMN_REF &&
				    column->iTable == source->iCursor &&
				    column->iColumn >= 0 &&
				    (uint32_t)column->iColumn == primary_field &&
				    value->op == TK_VARIABLE && value->iColumn > 0 &&
				    value->pLeft == NULL && value->pRight == NULL &&
				    !ExprHasProperty(value, EP_TokenOnly | EP_Reduced)) {
					point_key_variable = (uint32_t)value->iColumn;
					has_point_key = true;
				}
			}
			if (point_key_variable != 0)
				goto point_key_ready;
			for (size_t i = 0; i < expr_count; ++i) {
				if (!parse_pk_bound(exprs[i], source->iCursor,
						    primary_field,
						    unsigned_point, &parsed[i]))
					goto invalid_predicate;
				parsed_part[i] = 0;
			}
			if (expr_count == 1 && parsed[0].op == SQL_PLAN_EQ) {
				if (pk->part_count == 1) {
					has_point_key = true;
					if (unsigned_point)
						unsigned_point_key =
							parsed[0].unsigned_key;
					else
						point_key =
							parsed[0].signed_key;
				} else {
					/* Equality on the leading part of a composite key selects a
				 * contiguous prefix range, not a one-row point lookup.
				 */
					has_range_key = has_range_end_key =
						true;
					range_op = SQL_PLAN_GE;
					range_end_op = SQL_PLAN_LE;
					if (unsigned_point) {
						unsigned_range_key =
							parsed[0].unsigned_key;
						unsigned_range_end_key =
							parsed[0].unsigned_key;
					} else {
						range_key =
							parsed[0].signed_key;
						range_end_key =
							parsed[0].signed_key;
					}
				}
			} else {
				bool has_lower = false;
				bool has_upper = false;
				struct parsed_pk_bound lower = {0};
				struct parsed_pk_bound upper = {0};
				for (size_t i = 0; i < expr_count; ++i) {
					if (parsed[i].op == SQL_PLAN_EQ)
						goto invalid_predicate;
					if (parsed[i].op == SQL_PLAN_GT ||
					    parsed[i].op == SQL_PLAN_GE) {
						if (!has_lower || pk_bound_is_stricter(
							    &parsed[i], &lower, true))
							lower = parsed[i];
						has_lower = true;
					} else {
						if (!has_upper || pk_bound_is_stricter(
							    &parsed[i], &upper, false))
							upper = parsed[i];
						has_upper = true;
					}
				}
				if (!has_lower && !has_upper)
					goto invalid_predicate;
				has_range_key = true;
				has_range_end_key = has_lower && has_upper;
				range_op = has_lower ? lower.op : upper.op;
				if (has_range_end_key)
					range_end_op = upper.op;
				if (unsigned_point) {
					unsigned_range_key = has_lower ?
						lower.unsigned_key : upper.unsigned_key;
					if (has_range_end_key)
						unsigned_range_end_key =
							upper.unsigned_key;
				} else {
					range_key = has_lower ? lower.signed_key :
						upper.signed_key;
					if (has_range_end_key)
						range_end_key = upper.signed_key;
				}
			}
		point_key_ready:
			;
		}
		if (filter_count != 0 && has_point_key &&
		    !has_composite_point && pk->part_count != 1)
			goto invalid_predicate;
		if (has_multi_point_key && expr_count != 0)
			goto invalid_predicate;
	}
	goto predicate_parsed;
invalid_predicate:
	if (reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
	return NULL;
predicate_parsed:
	;
	/* The signed minimum is a useful endpoint exception for one-sided
	 * predicates. `key <= INT64_MIN` is exactly a point lookup and
	 * `key < INT64_MIN` is empty. Treat these as such so an unordered query
	 * does not need a descending scan solely to terminate at the minimum key.
	 * Keep composite leading-part predicates as ranges: those can match more
	 * than one tuple.
	 */
	if (pk->part_count == 1 && !range_unsigned && has_range_key &&
	    !has_range_end_key && !has_prefix_scan) {
		if (range_op == SQL_PLAN_LE && range_key == INT64_MIN) {
			has_point_key = true;
			point_key = INT64_MIN;
			has_range_key = false;
		} else if (range_op == SQL_PLAN_LT && range_key == INT64_MIN) {
			force_empty = true;
			has_range_key = false;
		}
	}
	if (force_empty) {
		finalize = (struct sql_plan_finalize) {
			.kind = SQL_PLAN_LIMIT,
			.limit = 0,
			.offset = 0,
		};
	}
	enum sql_plan_direction direction = SQL_PLAN_ASC;
	bool secondary_range_order = false;
	bool secondary_prefix_order = false;
	if (has_secondary_range_scan && !has_secondary_equality_scan &&
	    !has_point_key && !has_range_key && !has_prefix_scan &&
	    !has_prefix_range_scan &&
	    select->pOrderBy != NULL &&
	    select->pOrderBy->nExpr > 0 &&
	    source->space->index_map != NULL) {
		for (uint32_t index_no = 1;
		     index_no < source->space->index_count; ++index_no) {
			const struct index *index = source->space->index_map[index_no];
			if (index == NULL || index->def == NULL ||
			    index->def->iid != secondary_range_index_id ||
			    index->def->type != TREE ||
			    index->def->key_def == NULL ||
			    index->def->key_def->part_count <=
				    secondary_range_prefix_count)
				continue;
			bool order_columns_match = true;
			bool reverse_walk = false;
			bool have_range_order = false;
			enum sort_order range_order = SORT_ORDER_ASC;
			size_t range_order_count = 0;
			for (int i = 0; i < select->pOrderBy->nExpr; ++i) {
				const struct Expr *expr =
					select->pOrderBy->a[i].pExpr;
				if (expr == NULL || expr->op != TK_COLUMN_REF ||
				    expr->pLeft != NULL ||
				    expr->pRight != NULL ||
				    expr->iTable != source->iCursor ||
				    expr->iColumn < 0) {
					order_columns_match = false;
					break;
				}
				bool fixed_by_prefix = false;
				for (size_t prefix = 0;
				     prefix < secondary_range_prefix_count;
				     ++prefix) {
					if ((uint32_t)expr->iColumn ==
					    index->def->key_def->parts[prefix]
						    .fieldno) {
						fixed_by_prefix = true;
						break;
					}
				}
				if (fixed_by_prefix)
					continue;
				uint32_t part_no =
					secondary_range_prefix_count +
					(uint32_t)range_order_count;
				if (part_no >=
				    index->def->key_def->part_count) {
					order_columns_match = false;
					break;
				}
				const struct key_part *part =
					&index->def->key_def->parts[part_no];
				enum sort_order requested =
					select->pOrderBy->a[i].sort_order;
				if (requested == SORT_ORDER_UNDEF)
					requested = SORT_ORDER_ASC;
				if ((uint32_t)expr->iColumn != part->fieldno ||
				    (part->sort_order != SORT_ORDER_ASC &&
				     part->sort_order != SORT_ORDER_DESC) ||
				    (requested != SORT_ORDER_ASC &&
				     requested != SORT_ORDER_DESC)) {
					order_columns_match = false;
					break;
				}
				bool term_reverse =
					requested != part->sort_order;
				if (!have_range_order) {
					have_range_order = true;
					reverse_walk = term_reverse;
					range_order = requested;
				} else if (term_reverse != reverse_walk) {
					order_columns_match = false;
					break;
				}
				++range_order_count;
			}
			if (!order_columns_match)
				continue;
			bool requested_desc = range_order == SORT_ORDER_DESC;
			bool index_desc =
				index->def->key_def
					->parts[secondary_range_prefix_count]
					.sort_order == SORT_ORDER_DESC;
			/* One-sided ranges can walk from either end: lower-only scans
			 * stop at the lower bound when walking backward, while upper-only
			 * scans stop at the upper bound when walking forward. */
			secondary_range_order = true;
			if (secondary_range_order && have_range_order)
				direction = index_desc != requested_desc ?
						    SQL_PLAN_DESC :
						    SQL_PLAN_ASC;
			else if (secondary_range_order) {
				bool logical_desc = !has_secondary_range_lower;
				direction = index_desc != logical_desc ?
						    SQL_PLAN_DESC :
						    SQL_PLAN_ASC;
			}
			break;
		}
	}
	if (has_secondary_prefix_scan && select->pOrderBy != NULL &&
	    select->pOrderBy->nExpr > 0 && source->space->index_map != NULL) {
		for (uint32_t index_no = 1;
		     index_no < source->space->index_count; ++index_no) {
			const struct index *index = source->space->index_map[index_no];
			if (index == NULL || index->def == NULL ||
			    index->def->iid != secondary_prefix_index_id ||
			    index->def->type != TREE || index->def->key_def == NULL ||
			    index->def->key_def->part_count <= secondary_prefix_count ||
			    (uint32_t)select->pOrderBy->nExpr >
			    index->def->key_def->part_count - secondary_prefix_count)
				continue;
			secondary_prefix_order = true;
			for (int i = 0; i < select->pOrderBy->nExpr; ++i) {
				const struct Expr *expr = select->pOrderBy->a[i].pExpr;
				const struct key_part *part = &index->def->key_def->parts[
					secondary_prefix_count + i];
				if (expr == NULL || expr->op != TK_COLUMN_REF ||
				    expr->pLeft != NULL || expr->pRight != NULL ||
				    expr->iTable != source->iCursor || expr->iColumn < 0 ||
				    part->fieldno != (uint32_t)expr->iColumn ||
				    (part->sort_order != SORT_ORDER_ASC &&
				     part->sort_order != SORT_ORDER_DESC)) {
					secondary_prefix_order = false;
					break;
				}
			}
			break;
		}
	}
	bool use_secondary_equality_scan = has_secondary_equality_scan &&
		!has_point_key && !has_range_key && !has_prefix_scan &&
		select->pOrderBy == NULL;
	bool use_secondary_range_scan = has_secondary_range_scan &&
		!has_secondary_equality_scan && !has_point_key && !has_range_key &&
		!has_prefix_scan && !has_prefix_range_scan &&
		(select->pOrderBy == NULL || secondary_range_order);
	bool use_secondary_prefix_scan = has_secondary_prefix_scan &&
		!has_secondary_equality_scan && !has_secondary_range_scan &&
		!has_point_key && !has_range_key && !has_prefix_scan &&
		!has_prefix_range_scan &&
		(select->pOrderBy == NULL || secondary_prefix_order);
	bool use_secondary_full_scan = has_secondary_full_scan &&
		!has_point_key && !has_range_key && !has_prefix_scan &&
		!has_secondary_equality_scan && !has_secondary_range_scan &&
		select->pWhere == NULL;
	if (has_secondary_prefix_scan && !use_secondary_prefix_scan) {
		for (size_t i = 0; i < secondary_prefix_count; ++i) {
			if (filter_count == SQL_PLAN_FILTER_MAX)
				goto invalid_predicate;
			filters[filter_count] = (struct sql_plan_filter) {
				.op = SQL_PLAN_FILTER_EXPRESSION,
				.selectivity = 0.5,
			};
			filter_expressions[filter_count++] = secondary_prefix_terms[i];
		}
	}
	if (has_secondary_equality_scan && !use_secondary_equality_scan) {
		for (size_t i = 0; i < (secondary_key_part_count == 0 ? 1 :
					 secondary_key_part_count); ++i) {
			if (filter_count == SQL_PLAN_FILTER_MAX)
				goto invalid_predicate;
			filters[filter_count] = (struct sql_plan_filter) {
				.op = SQL_PLAN_FILTER_EXPRESSION,
				.selectivity = 0.5,
			};
			filter_expressions[filter_count++] =
				secondary_key_part_count == 0 ? secondary_scan_term :
				secondary_scan_terms[i];
		}
	}
	if (use_secondary_range_scan) {
		has_range_key = true;
		has_range_end_key = has_secondary_range_lower &&
			has_secondary_range_upper;
		range_unsigned = secondary_range_unsigned;
		if (has_secondary_range_lower) {
			range_op = secondary_range_lower.op;
			if (range_unsigned)
				unsigned_range_key = secondary_range_lower.unsigned_key;
			else
				range_key = secondary_range_lower.signed_key;
		} else {
			range_op = secondary_range_upper.op;
			if (range_unsigned)
				unsigned_range_key = secondary_range_upper.unsigned_key;
			else
				range_key = secondary_range_upper.signed_key;
		}
		if (has_range_end_key) {
			range_end_op = secondary_range_upper.op;
			if (range_unsigned)
				unsigned_range_end_key =
					secondary_range_upper.unsigned_key;
			else
				range_end_key = secondary_range_upper.signed_key;
		}
		range_key_column = secondary_range_key_column;
		if (select->pOrderBy == NULL) {
			bool logical_desc = !has_secondary_range_lower;
			direction = secondary_range_descending != logical_desc ?
				SQL_PLAN_DESC : SQL_PLAN_ASC;
		}
	} else if (has_secondary_range_scan && !use_secondary_equality_scan) {
		for (size_t i = 0; i < secondary_range_term_count; ++i) {
			bool already_filtered = false;
			for (size_t j = 0; j < filter_count; ++j)
				already_filtered |= filter_expressions[j] ==
					secondary_range_terms[i];
			if (already_filtered)
				continue;
			if (filter_count == SQL_PLAN_FILTER_MAX)
				goto invalid_predicate;
			filters[filter_count] = (struct sql_plan_filter) {
				.op = SQL_PLAN_FILTER_EXPRESSION,
				.selectivity = 0.5,
			};
			filter_expressions[filter_count++] = secondary_range_terms[i];
		}
	}
	bool secondary_composite = use_secondary_equality_scan &&
		secondary_key_part_count > 1;
	bool secondary_prefix_composite = use_secondary_prefix_scan;
	bool secondary_range_composite = use_secondary_range_scan &&
		secondary_range_prefix_count != 0;
	size_t composite_access_count = has_composite_point || has_prefix_scan ?
		composite_point_count : secondary_composite ?
		secondary_key_part_count : secondary_prefix_composite ?
		secondary_prefix_count : secondary_range_composite ?
		secondary_range_prefix_count : 0;
	struct sql_plan_order_term *order_terms = NULL;
	size_t order_term_count = 0;
	if (select->pOrderBy != NULL) {
		const struct ExprList *order_by = select->pOrderBy;
		const struct key_def *key_def =
			source->space->index_map[0]->def->key_def;
		bool can_use_secondary_full_scan = select->pWhere == NULL ||
			(!has_point_key && !has_range_key && !has_prefix_scan &&
			 !has_prefix_range_scan && !has_secondary_equality_scan &&
			 !has_secondary_range_scan && !has_secondary_prefix_scan);
		if (can_use_secondary_full_scan && order_by->nExpr > 0) {
			const struct Expr *order_expr = order_by->a[0].pExpr;
			if (order_expr != NULL && order_expr->op == TK_COLUMN_REF &&
			    order_expr->pLeft == NULL && order_expr->pRight == NULL &&
			    order_expr->iTable == source->iCursor &&
			    order_expr->iColumn >= 0 &&
			    source->space->index_map != NULL) {
				for (uint32_t index_no = 1;
				     index_no < source->space->index_count; ++index_no) {
					const struct index *index =
						source->space->index_map[index_no];
					if (index == NULL || index->def == NULL ||
					    index->def->type != TREE ||
					    index->def->key_def == NULL ||
					    index->def->key_def->part_count == 0 ||
					    index->def->key_def->parts[0].fieldno !=
						(uint32_t)order_expr->iColumn)
						continue;
					const struct key_def *candidate =
						index->def->key_def;
					if ((uint32_t)order_by->nExpr >
					    candidate->part_count)
						continue;
					bool matches_prefix = true;
					bool natural_order = true;
					bool reverse_order = true;
					for (int term = 0; term < order_by->nExpr; ++term) {
						const struct Expr *expr =
							order_by->a[term].pExpr;
						enum sort_order key_order =
							candidate->parts[term].sort_order;
						enum sort_order requested =
							order_by->a[term].sort_order;
						if (requested == SORT_ORDER_UNDEF)
							requested = SORT_ORDER_ASC;
						if ((key_order != SORT_ORDER_ASC &&
						     key_order != SORT_ORDER_DESC) || expr == NULL ||
						    expr->op != TK_COLUMN_REF ||
						    expr->pLeft != NULL ||
						    expr->pRight != NULL ||
						    expr->iTable != source->iCursor ||
						    expr->iColumn < 0 ||
							candidate->parts[term].fieldno !=
							(uint32_t)expr->iColumn) {
							matches_prefix = false;
							break;
						}
						natural_order &= requested == key_order;
						reverse_order &= requested != key_order;
					}
					if (!matches_prefix ||
					    (!natural_order && !reverse_order))
						continue;
					if (!has_secondary_full_scan || natural_order) {
						key_def = index->def->key_def;
						has_secondary_full_scan = true;
						secondary_full_scan_natural_order = natural_order;
						secondary_full_index_id = index->def->iid;
					}
					if (natural_order)
						break;
				}
			}
		}
		if (secondary_range_order || secondary_prefix_order) {
			uint32_t selected_index_id = secondary_range_order ?
				secondary_range_index_id : secondary_prefix_index_id;
			for (uint32_t index_no = 1;
			     index_no < source->space->index_count; ++index_no) {
				const struct index *index =
					source->space->index_map[index_no];
				if (index != NULL && index->def != NULL &&
				    index->def->iid == selected_index_id) {
					key_def = index->def->key_def;
					break;
				}
			}
		}
		if (order_by->nExpr <= 0 ||
		    (uint32_t)order_by->nExpr > key_def->part_count) {
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			return NULL;
		}
		order_terms = calloc((size_t)order_by->nExpr,
				     sizeof(*order_terms));
		if (order_terms == NULL) {
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
			return NULL;
		}
		enum sql_plan_direction order_direction = SQL_PLAN_ASC;
		uint32_t first_order_part = 0;
		if (has_prefix_scan) {
			const struct Expr *first_order_expr = order_by->a[0].pExpr;
			if (first_order_expr == NULL ||
			    first_order_expr->op != TK_COLUMN_REF ||
			    first_order_expr->iColumn < 0) {
				free(order_terms);
				goto invalid_predicate;
			}
			first_order_part = UINT32_MAX;
			for (uint32_t part = 0; part < key_def->part_count; ++part) {
				if (key_def->parts[part].fieldno ==
				    (uint32_t)first_order_expr->iColumn) {
					first_order_part = part;
					break;
				}
			}
			/* The scan produces either an ordinary leading key order or,
			 * more usefully, an order beginning at the first unfixed part.
			 */
			if (first_order_part != 0 && first_order_part !=
			    composite_point_count) {
				free(order_terms);
				goto invalid_predicate;
			}
			if ((uint32_t)order_by->nExpr >
			    key_def->part_count - first_order_part) {
				free(order_terms);
				goto invalid_predicate;
			}
		} else if (secondary_range_composite ||
			   secondary_prefix_composite) {
			first_order_part =
				(uint32_t)secondary_range_prefix_count;
			if (secondary_prefix_composite)
				first_order_part =
					(uint32_t)secondary_prefix_count;
			size_t order_key_count = (size_t)order_by->nExpr;
			if (secondary_range_composite &&
			    secondary_range_order) {
				order_key_count = 0;
				for (int i = 0; i < order_by->nExpr; ++i) {
					const struct Expr *expr =
						order_by->a[i].pExpr;
					bool fixed_by_prefix = false;
					if (expr != NULL &&
					    expr->op == TK_COLUMN_REF &&
					    expr->iTable == source->iCursor &&
					    expr->iColumn >= 0) {
						for (size_t prefix = 0;
						     prefix <
						     secondary_range_prefix_count;
						     ++prefix) {
							fixed_by_prefix |=
								(uint32_t)expr
									->iColumn ==
								secondary_range_prefix_parts
									[prefix]
										.column;
						}
					}
					order_key_count += !fixed_by_prefix;
				}
			}
			if (order_key_count >
			    key_def->part_count - first_order_part) {
				free(order_terms);
				goto invalid_predicate;
			}
		}
		bool reverse_index_walk = false;
		for (int i = 0; i < order_by->nExpr; ++i) {
			const struct Expr *order_expr = order_by->a[i].pExpr;
			enum sort_order term_direction =
				order_by->a[i].sort_order;
			if (term_direction == SORT_ORDER_UNDEF)
				term_direction = SORT_ORDER_ASC;
			if (order_expr == NULL ||
			    ExprHasProperty(order_expr,
					    EP_TokenOnly | EP_Reduced) ||
			    order_expr->op != TK_COLUMN_REF ||
			    order_expr->pLeft != NULL ||
			    order_expr->pRight != NULL ||
			    order_expr->iTable != source->iCursor ||
			    order_expr->iColumn < 0 ||
			    (term_direction != SORT_ORDER_ASC &&
			     term_direction != SORT_ORDER_DESC)) {
				free(order_terms);
				if (reason != NULL)
					*reason =
						SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
				return NULL;
			}
			bool fixed_by_prefix = false;
			if (secondary_range_composite &&
			    secondary_range_order) {
				for (size_t prefix = 0;
				     prefix < secondary_range_prefix_count;
				     ++prefix) {
					fixed_by_prefix |=
						(uint32_t)order_expr->iColumn ==
						secondary_range_prefix_parts
							[prefix]
								.column;
				}
			}
			if (fixed_by_prefix)
				continue;
			size_t order_part_offset =
				secondary_range_composite &&
						secondary_range_order ?
					order_term_count :
					(size_t)i;
			const struct key_part *key_part =
				&key_def->parts[first_order_part +
						(uint32_t)order_part_offset];
			enum sort_order key_direction =
				key_part->sort_order == SORT_ORDER_DESC ?
					SORT_ORDER_DESC :
					SORT_ORDER_ASC;
			if (order_term_count == 0)
				reverse_index_walk =
					term_direction != key_direction;
			enum sort_order expected_direction =
				reverse_index_walk ?
					(key_direction == SORT_ORDER_ASC ?
						 SORT_ORDER_DESC :
						 SORT_ORDER_ASC) :
					key_direction;
			if ((uint32_t)order_expr->iColumn !=
				    key_part->fieldno ||
			    term_direction != expected_direction) {
				free(order_terms);
				if (reason != NULL)
					*reason =
						SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
				return NULL;
			}
			if (order_term_count == 0)
				order_direction =
					term_direction == SORT_ORDER_DESC ?
						SQL_PLAN_DESC :
						SQL_PLAN_ASC;
			order_terms[order_term_count++] =
				(struct sql_plan_order_term){
					.column = key_part->fieldno,
					.direction =
						term_direction ==
								SORT_ORDER_DESC ?
							SQL_PLAN_DESC :
							SQL_PLAN_ASC,
				};
		}
		direction = has_secondary_full_scan ?
				    (secondary_full_scan_natural_order ?
					     SQL_PLAN_ASC :
					     SQL_PLAN_DESC) :
				    (reverse_index_walk ? SQL_PLAN_DESC :
							  SQL_PLAN_ASC);
		if (has_secondary_range_scan) {
			if (order_term_count != 0) {
				bool requested_desc =
					order_direction == SQL_PLAN_DESC;
				direction = secondary_range_descending !=
							    requested_desc ?
						    SQL_PLAN_DESC :
						    SQL_PLAN_ASC;
			}
		}
		/* Upper-only ranges may satisfy ASC by rewinding and stopping at
		 * the upper guard, or DESC by seeking to the endpoint. Lower-only
		 * ranges may satisfy DESC by walking backward from Last and stopping
		 * at the lower guard. The VDBE range lowerer implements these walks.
		 */
		if (has_range_key && !has_prefix_range_scan &&
		    !has_secondary_range_scan &&
		    !has_range_end_key &&
		    direction != (range_op == SQL_PLAN_LT ||
				  range_op == SQL_PLAN_LE ? SQL_PLAN_DESC :
				  SQL_PLAN_ASC) &&
		    !(direction == SQL_PLAN_ASC &&
		      (range_op == SQL_PLAN_LT || range_op == SQL_PLAN_LE)) &&
		    !(direction == SQL_PLAN_DESC &&
		      (range_op == SQL_PLAN_GT || range_op == SQL_PLAN_GE))) {
			free(order_terms);
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			return NULL;
		}
	}
	/* Secondary full-scan eligibility is discovered while resolving ORDER BY
	 * against the available indexes, so finalize it only after that pass.
	 */
	use_secondary_full_scan = has_secondary_full_scan &&
		!has_point_key && !has_range_key && !has_prefix_scan &&
		!has_prefix_range_scan && !has_secondary_equality_scan &&
		!has_secondary_range_scan;
	uint32_t *columns = calloc(select->pEList->nExpr, sizeof(*columns));
	uint32_t *projection_expr_refs = calloc(select->pEList->nExpr,
						 sizeof(*projection_expr_refs));
	char **projection_canonical = calloc(select->pEList->nExpr,
					      sizeof(*projection_canonical));
	if (columns == NULL || projection_expr_refs == NULL ||
	    projection_canonical == NULL) {
		free(columns);
		free(projection_expr_refs);
		free(projection_canonical);
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
		return NULL;
	}
	uint32_t *cursor_to_relation = NULL;
	size_t cursor_count = (size_t)source->iCursor + 1;
	if (source->iCursor < 0 ||
	    cursor_count > SIZE_MAX / sizeof(*cursor_to_relation)) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		goto invalid_projection;
	}
	cursor_to_relation = malloc(cursor_count * sizeof(*cursor_to_relation));
	if (cursor_to_relation == NULL) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
		goto invalid_projection;
	}
	for (size_t i = 0; i < cursor_count; ++i)
		cursor_to_relation[i] = UINT32_MAX;
	cursor_to_relation[source->iCursor] = 0;
	size_t projection_expression_count = 0;
	for (int i = 0; i < select->pEList->nExpr; ++i) {
		const struct Expr *expr = select->pEList->a[i].pExpr;
		if (expr != NULL && expr->op == TK_COLUMN_REF &&
		    !ExprHasProperty(expr, EP_TokenOnly | EP_Reduced) &&
		    expr->pLeft == NULL && expr->pRight == NULL &&
		    expr->iTable == source->iCursor && expr->iColumn >= 0 &&
		    (uint32_t)expr->iColumn < source->space->def->field_count) {
			columns[i] = (uint32_t)expr->iColumn;
			continue;
		}
		enum sql_expr_canonical_reject canonical_reason;
		projection_canonical[i] = sql_expr_canonicalize(expr,
			cursor_to_relation, cursor_count, &canonical_reason);
		if (projection_canonical[i] == NULL) {
			if (reason != NULL)
				*reason = canonical_reason == SQL_EXPR_CANONICAL_NOMEM ?
					SQL_PHYSICAL_REJECT_INVALID_CANDIDATE :
					SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			free(cursor_to_relation);
			cursor_to_relation = NULL;
			goto invalid_projection;
		}
		columns[i] = UINT32_MAX;
		++projection_expression_count;
	}
	for (size_t i = 0; i < filter_count; ++i) {
		if (filters[i].op != SQL_PLAN_FILTER_EXPRESSION)
			continue;
		enum sql_expr_canonical_reject canonical_reason;
		filter_canonical[i] = sql_expr_canonicalize(filter_expressions[i],
			cursor_to_relation, cursor_count, &canonical_reason);
		if (filter_canonical[i] == NULL) {
			if (reason != NULL)
				*reason = canonical_reason == SQL_EXPR_CANONICAL_NOMEM ?
					SQL_PHYSICAL_REJECT_INVALID_CANDIDATE :
					SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
			goto invalid_projection;
		}
	}
	free(cursor_to_relation);
	cursor_to_relation = NULL;
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
	struct sql_plan_expression composite_point_expressions[
		SQL_PLAN_POINT_KEY_PART_MAX + 2];
	struct sql_plan_bound composite_point_bounds[
		SQL_PLAN_POINT_KEY_PART_MAX + 2];
	for (size_t i = 0; i < composite_access_count; ++i) {
		const struct sql_plan_point_key_part *key_part =
			secondary_composite ? &secondary_key_parts[i] :
			secondary_prefix_composite ? &secondary_prefix_parts[i] :
			secondary_range_composite ?
				&secondary_range_prefix_parts[i] :
			&composite_point_parts[i];
		composite_point_expressions[i] = (struct sql_plan_expression) {
			.id = (uint32_t)i + 1,
			.canonical = secondary_composite || secondary_prefix_composite ||
				secondary_range_composite ?
				(key_part->is_unsigned ? "secondary-unsigned-equality-part" :
				 "secondary-integer-equality-part") :
				"composite-primary-point-part",
		};
		composite_point_bounds[i] = (struct sql_plan_bound) {
			.side = SQL_PLAN_LOWER,
			.op = SQL_PLAN_EQ,
			.expr_ref = (uint32_t)i + 1,
		};
	}
	size_t composite_expression_count = composite_access_count;
	size_t composite_bound_count = composite_access_count;
	if (has_prefix_range_scan) {
		if (prefix_range_has_lower) {
			composite_point_expressions[composite_expression_count] =
				(struct sql_plan_expression) {
					.id = (uint32_t)composite_expression_count + 1,
					.canonical = range_unsigned ?
						"unsigned-prefix-range-lower" :
						"integer-prefix-range-lower",
				};
			composite_point_bounds[composite_bound_count++] =
				(struct sql_plan_bound) {
					.side = SQL_PLAN_LOWER,
					.op = range_op,
					.expr_ref = (uint32_t)composite_expression_count + 1,
				};
			++composite_expression_count;
		}
		if (prefix_range_has_upper) {
			composite_point_expressions[composite_expression_count] =
				(struct sql_plan_expression) {
					.id = (uint32_t)composite_expression_count + 1,
					.canonical = range_unsigned ?
						"unsigned-prefix-range-upper" :
						"integer-prefix-range-upper",
				};
			composite_point_bounds[composite_bound_count++] =
				(struct sql_plan_bound) {
					.side = SQL_PLAN_UPPER,
					.op = has_range_end_key ? range_end_op : range_op,
					.expr_ref = (uint32_t)composite_expression_count + 1,
				};
			++composite_expression_count;
		}
	}
	if (secondary_range_composite) {
		bool bounded = has_secondary_range_lower &&
			has_secondary_range_upper;
		for (size_t i = 0; i < (bounded ? 2 : 1); ++i) {
			bool lower = bounded ? i == 0 :
				has_secondary_range_lower;
			const struct parsed_pk_bound *bound = lower ?
				&secondary_range_lower : &secondary_range_upper;
			composite_point_expressions[composite_expression_count] =
				(struct sql_plan_expression) {
					.id = (uint32_t)composite_expression_count + 1,
					.canonical = secondary_range_unsigned ?
						"secondary-unsigned-prefix-range-bound" :
						"secondary-integer-prefix-range-bound",
				};
			composite_point_bounds[composite_bound_count++] =
				(struct sql_plan_bound) {
					.side = lower ? SQL_PLAN_LOWER : SQL_PLAN_UPPER,
					.op = bound->op,
					.expr_ref = (uint32_t)composite_expression_count + 1,
				};
			++composite_expression_count;
		}
	}
	/*
	 * Keep the original single-bound encoding for point and one-sided routes;
	 * bounded ranges add one independently-owned expression/bound.
	 */
	struct sql_plan_expression point_expression = {
		.id = 1,
		.canonical = use_secondary_equality_scan ?
			(secondary_key_unsigned ? "secondary-unsigned-equality-key" :
			 "secondary-integer-equality-key") :
			has_range_key ? (unsigned_point ? "unsigned-range-key" :
			"integer-range-key") :
			"integer-point-key",
	};
	const struct sql_plan_expression *base_expressions = NULL;
	size_t base_expression_count = 0;
	if (has_composite_point || has_prefix_scan || secondary_composite ||
	    secondary_prefix_composite ||
	    secondary_range_composite) {
		base_expressions = composite_point_expressions;
		base_expression_count = composite_expression_count;
	} else if (has_point_key || has_range_key ||
		   use_secondary_equality_scan) {
		base_expressions = has_range_end_key ? point_expressions :
			&point_expression;
		base_expression_count = has_range_end_key ? 2 : 1;
	}
	struct sql_plan_expression *expressions = NULL;
	size_t expression_count = base_expression_count + filter_count +
		projection_expression_count;
	if (expression_count != 0) {
		expressions = calloc(expression_count, sizeof(*expressions));
		if (expressions == NULL) {
			if (reason != NULL)
				*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
			goto invalid_projection;
		}
	}
	for (size_t i = 0; i < base_expression_count; ++i)
		expressions[i] = base_expressions[i];
	size_t next_expression = base_expression_count;
	for (size_t i = 0; i < filter_count; ++i) {
		filters[i].expr_ref = (uint32_t)next_expression + 1;
		filters[i].confidence = 0;
		expressions[next_expression++] =
			(struct sql_plan_expression) {
				.id = filters[i].expr_ref,
				.canonical = filter_canonical[i] == NULL ?
					"direct-column-null-filter" : filter_canonical[i],
			};
	}
	for (int i = 0; i < select->pEList->nExpr; ++i) {
		if (projection_canonical[i] == NULL)
			continue;
		uint32_t id = (uint32_t)next_expression + 1;
		projection_expr_refs[i] = id;
		expressions[next_expression++] = (struct sql_plan_expression) {
			.id = id,
			.canonical = projection_canonical[i],
		};
	}
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
			.kind = has_prefix_scan ? SQL_PLAN_PK_PREFIX_SCAN :
				has_point_key ? SQL_PLAN_PK_POINT_LOOKUP :
				use_secondary_equality_scan ?
				SQL_PLAN_INDEX_EQUALITY_SCAN :
				use_secondary_prefix_scan ?
				SQL_PLAN_INDEX_PREFIX_SCAN :
				use_secondary_full_scan ? SQL_PLAN_INDEX_FULL_SCAN :
				 has_range_key ? SQL_PLAN_INDEX_RANGE_SCAN :
				SQL_PLAN_TABLE_FULL_SCAN,
			.index_id = use_secondary_equality_scan ? secondary_index_id :
				use_secondary_prefix_scan ? secondary_prefix_index_id :
				use_secondary_range_scan ? secondary_range_index_id :
				use_secondary_full_scan ? secondary_full_index_id : 0,
			.bounds = has_composite_point || has_prefix_scan ||
				secondary_composite || secondary_prefix_composite ||
				secondary_range_composite ?
				composite_point_bounds :
				has_point_key || has_range_key ||
				use_secondary_equality_scan ? point_bounds : NULL,
			.bound_count = has_multi_point_key ? 0 :
				has_composite_point || has_prefix_scan ||
				secondary_composite || secondary_prefix_composite ||
				secondary_range_composite ?
				composite_bound_count :
				has_range_key ? (has_range_end_key ? 2 : 1) :
				has_point_key || use_secondary_equality_scan ? 1 : 0,
			.has_integer_point_key = (has_point_key &&
				!has_multi_point_key &&
				!has_composite_point && !unsigned_point &&
				point_key_variable == 0) ||
				(use_secondary_equality_scan && !secondary_composite &&
				 !secondary_key_unsigned),
			.integer_point_key = use_secondary_equality_scan ?
				secondary_signed_key : point_key,
			.has_unsigned_point_key = (has_point_key &&
				!has_multi_point_key &&
				!has_composite_point && unsigned_point &&
				point_key_variable == 0) ||
				(use_secondary_equality_scan && !secondary_composite &&
				 secondary_key_unsigned),
			.unsigned_point_key = use_secondary_equality_scan ?
				secondary_unsigned_key : unsigned_point_key,
			.point_key_variable = point_key_variable,
			.point_key_values = has_multi_point_key ? multi_point_values : NULL,
			.point_key_value_count = has_multi_point_key ?
				multi_point_count : 0,
			.point_key_parts = has_composite_point ?
				composite_point_parts : secondary_composite ?
				secondary_key_parts : NULL,
			.point_key_part_count = has_composite_point ?
				composite_point_count : secondary_composite ?
				secondary_key_part_count : 0,
			.prefix_key_parts = has_prefix_scan ? composite_point_parts :
				secondary_prefix_composite ? secondary_prefix_parts :
				secondary_range_composite ?
				secondary_range_prefix_parts : NULL,
			.prefix_key_part_count = has_prefix_scan ?
				composite_point_count : secondary_prefix_composite ?
				secondary_prefix_count : secondary_range_composite ?
				secondary_range_prefix_count : 0,
			.has_integer_range_key = has_range_key && !range_unsigned,
			.integer_range_key = range_key,
			.has_unsigned_range_key = has_range_key && range_unsigned,
			.unsigned_range_key = unsigned_range_key,
			.integer_range_op = range_op,
			.has_integer_range_end_key = has_range_end_key &&
				!range_unsigned,
			.integer_range_end_key = range_end_key,
			.has_unsigned_range_end_key = has_range_end_key &&
				range_unsigned,
			.unsigned_range_end_key = unsigned_range_end_key,
			.integer_range_end_op = range_end_op,
			.range_key_column = use_secondary_prefix_scan &&
				secondary_prefix_order && order_term_count != 0 ?
				order_terms[0].column : use_secondary_equality_scan ?
				secondary_key_column : range_key_column,
			.direction = direction,
			.produced_order = order_terms,
			.produced_order_count = order_term_count,
			.projected_columns = columns,
			.projected_column_count = select->pEList->nExpr,
			.est_rows = has_multi_point_key ? multi_point_count :
				has_point_key || use_secondary_equality_scan ? 1 :
				has_range_key ?
				estimate->rows / 2 : estimate->rows,
			.est_rows_confidence = estimate->confidence,
		},
		.filters = filter_count == 0 ? NULL : filters,
		.filter_count = filter_count,
		.projection_columns = columns,
		.projection_expr_refs = projection_expr_refs,
		.projection_column_count = select->pEList->nExpr,
		.expressions = expression_count == 0 ? NULL : expressions,
		.expression_count = expression_count,
		.cost_startup = estimate->startup_cost,
		.cost_total = has_multi_point_key ? multi_point_count :
			has_point_key || use_secondary_equality_scan ? 1 :
			has_range_key ?
			estimate->total_cost / 2 : estimate->total_cost,
		.cost_rows = has_multi_point_key ? multi_point_count :
			has_point_key || use_secondary_equality_scan ? 1 :
			has_range_key ?
			estimate->rows / 2 : estimate->rows,
		.cost_row_width = estimate->row_width,
		.cost_confidence = estimate->confidence,
	};
	struct sql_plan_descriptor *plan = sql_plan_descriptor_new(&input);
	free(expressions);
	for (size_t i = 0; i < filter_count; ++i)
		free(filter_canonical[i]);
	for (int i = 0; i < select->pEList->nExpr; ++i)
		free(projection_canonical[i]);
	free(projection_canonical);
	free(projection_expr_refs);
	free(columns);
	free(order_terms);
	if (plan == NULL && reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_INVALID_CANDIDATE;
	return plan;

invalid_projection:
	for (size_t i = 0; i < SQL_PLAN_FILTER_MAX; ++i)
		free(filter_canonical[i]);
	for (int i = 0; i < select->pEList->nExpr; ++i)
		free(projection_canonical[i]);
	free(cursor_to_relation);
	free(projection_canonical);
	free(projection_expr_refs);
	free(columns);
	free(order_terms);
	return NULL;
}
