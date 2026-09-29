#include "sql_select_preflight.h"

#include "sqlInt.h"
#include "sql_expr_canonical.h"
#include "sql_plan_descriptor.h"
#include "box/index.h"
#include "box/index_def.h"
#include "box/key_def.h"
#include "box/space.h"

static bool
is_comparison_predicate(const struct Expr *expr)
{
	return expr != NULL && expr->pLeft != NULL && expr->pRight != NULL &&
		(expr->op == TK_EQ || expr->op == TK_NE || expr->op == TK_GT ||
		 expr->op == TK_GE || expr->op == TK_LT || expr->op == TK_LE);
}

static bool
is_between_predicate(const struct Expr *expr)
{
	return expr != NULL && expr->op == TK_BETWEEN && expr->pLeft != NULL &&
		expr->pRight == NULL &&
		!ExprHasProperty(expr, EP_TokenOnly | EP_Reduced | EP_xIsSelect) &&
		expr->x.pList != NULL && expr->x.pList->nExpr == 2 &&
		expr->x.pList->a[0].pExpr != NULL &&
		expr->x.pList->a[1].pExpr != NULL;
}

static bool
has_only_source_columns(const struct Expr *expr, int cursor,
			uint32_t field_count, size_t depth);
static bool
has_source_column(const struct Expr *expr, int cursor, uint32_t field_count,
		  size_t depth);

static bool
is_in_predicate(const struct Expr *expr, int cursor, uint32_t field_count)
{
	if (expr == NULL || expr->op != TK_IN || expr->pRight != NULL ||
	    ExprHasProperty(expr, EP_TokenOnly | EP_Reduced | EP_xIsSelect) ||
	    expr->x.pList == NULL || expr->x.pList->nExpr <= 0 ||
	    !has_only_source_columns(expr->pLeft, cursor, field_count, 0) ||
	    !has_source_column(expr->pLeft, cursor, field_count, 0))
		return false;
	for (int i = 0; i < expr->x.pList->nExpr; ++i) {
		if (expr->x.pList->a[i].pExpr == NULL)
			return false;
	}
	return true;
}

static bool
has_unsupported_in_operand(const struct Expr *expr, int cursor,
			   uint32_t field_count, size_t depth)
{
	if (expr == NULL || depth >= SQL_PLAN_POINT_KEY_PART_MAX)
		return false;
	if (ExprHasProperty(expr, EP_SingletonIn))
		return true;
	if (expr->op == TK_IN)
		return !is_in_predicate(expr, cursor, field_count);
	if (expr->op == TK_AND || expr->op == TK_OR)
		return has_unsupported_in_operand(expr->pLeft, cursor, field_count,
						  depth + 1) ||
			has_unsupported_in_operand(expr->pRight, cursor, field_count,
						   depth + 1);
	if (expr->op == TK_NOT)
		return has_unsupported_in_operand(expr->pLeft, cursor, field_count,
						  depth + 1);
	return false;
}

static bool
is_supported_projection_expr(const struct Expr *expr, int cursor,
			     uint32_t field_count,
			     const uint32_t *cursor_to_relation,
			     size_t cursor_count)
{
	if (expr == NULL || ExprHasProperty(expr, EP_TokenOnly | EP_Reduced))
		return false;
	if (expr->op == TK_COLUMN_REF)
		return expr->pLeft == NULL && expr->pRight == NULL &&
			expr->iTable == cursor && expr->iColumn >= 0 &&
			(uint32_t)expr->iColumn < field_count;
	enum sql_expr_canonical_reject reason;
	char *canonical = sql_expr_canonicalize(expr, cursor_to_relation,
						cursor_count, &reason);
	free(canonical);
	return reason == SQL_EXPR_CANONICAL_OK ||
		reason == SQL_EXPR_CANONICAL_NOMEM;
}

static bool
is_direct_null_predicate(const struct Expr *expr, int cursor,
			 uint32_t field_count)
{
	return expr != NULL && (expr->op == TK_ISNULL || expr->op == TK_NOTNULL) &&
		expr->pLeft != NULL && expr->pRight == NULL &&
		expr->pLeft->op == TK_COLUMN_REF &&
		expr->pLeft->pLeft == NULL && expr->pLeft->pRight == NULL &&
		expr->pLeft->iTable == cursor && expr->pLeft->iColumn >= 0 &&
		(uint32_t)expr->pLeft->iColumn < field_count;
}

static bool
has_only_source_columns(const struct Expr *expr, int cursor,
			uint32_t field_count, size_t depth)
{
	if (expr == NULL || depth >= SQL_PLAN_POINT_KEY_PART_MAX ||
	    ExprHasProperty(expr, EP_TokenOnly | EP_Reduced | EP_xIsSelect))
		return false;
	if (expr->op == TK_COLUMN_REF)
		return expr->pLeft == NULL && expr->pRight == NULL &&
			expr->iTable == cursor && expr->iColumn >= 0 &&
			(uint32_t)expr->iColumn < field_count;
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
	if (expr->op == TK_COLUMN_REF)
		return expr->pLeft == NULL && expr->pRight == NULL &&
			expr->iTable == cursor && expr->iColumn >= 0 &&
			(uint32_t)expr->iColumn < field_count;
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
	return has_source_column(expr->pLeft, cursor, field_count, depth + 1) ||
		has_source_column(expr->pRight, cursor, field_count, depth + 1);
}

static bool
is_filter_predicate_tree(const struct Expr *expr, int cursor,
			 uint32_t field_count, size_t *term_count,
			 size_t depth)
{
	if (expr == NULL || term_count == NULL ||
	    depth >= SQL_PLAN_POINT_KEY_PART_MAX)
		return false;
	if ((expr->op == TK_AND || expr->op == TK_OR) &&
	    expr->pLeft != NULL && expr->pRight != NULL)
		return is_filter_predicate_tree(expr->pLeft, cursor, field_count,
						term_count, depth + 1) &&
			is_filter_predicate_tree(expr->pRight, cursor, field_count,
						  term_count, depth + 1);
	if (expr->op == TK_NOT && expr->pLeft != NULL && expr->pRight == NULL)
		return is_filter_predicate_tree(expr->pLeft, cursor, field_count,
						term_count, depth + 1);
	bool expression_null_predicate = expr->pRight == NULL &&
		(expr->op == TK_ISNULL || expr->op == TK_NOTNULL) &&
		has_only_source_columns(expr->pLeft, cursor, field_count, 0);
	if (!is_comparison_predicate(expr) && !is_between_predicate(expr) &&
	    !is_in_predicate(expr, cursor, field_count) &&
	    !is_direct_null_predicate(expr, cursor, field_count) &&
	    !expression_null_predicate)
		return false;
	if (*term_count == SQL_PLAN_POINT_KEY_PART_MAX)
		return false;
	++*term_count;
	return true;
}

enum sql_select_preflight_reject
sql_select_preflight_table_scan(const struct Select *select,
				const struct SelectDest *dest)
{
	if (select == NULL || (select->selFlags & SF_Resolved) == 0)
		return SQL_SELECT_PREFLIGHT_UNRESOLVED;
	bool supported_destination = dest != NULL && dest->eDest == SRT_Output &&
		dest->pOrderBy == NULL;
	if (select->pSrc == NULL || select->pSrc->nSrc != 1 ||
	    select->pSrc->a[0].space == NULL ||
	    select->pSrc->a[0].space->def == NULL ||
	    select->pSrc->a[0].space->def->opts.is_view)
		return SQL_SELECT_PREFLIGHT_RELATION;
	const struct SrcList_item *source = &select->pSrc->a[0];
	if (select->pPrior != NULL || select->pValuesTail != NULL ||
	    select->pWith != NULL || select->pGroupBy != NULL ||
	    select->pHaving != NULL ||
	    (select->selFlags & (SF_Values | SF_NestedFrom | SF_Compound |
			 SF_Aggregate | SF_HasAgg | SF_Distinct)) != 0 ||
	    source->pSelect != NULL || source->fg.isTabFunc || source->pOn != NULL ||
	    source->pUsing != NULL || source->fg.isIndexedBy ||
	    source->fg.notIndexed)
		return SQL_SELECT_PREFLIGHT_SHAPE;
	if (select->pOrderBy != NULL) {
		const struct ExprList *order = select->pOrderBy;
		if (order->nExpr <= 0 || source->space->index_map == NULL ||
		    source->space->index_map[0] == NULL ||
		    source->space->index_map[0]->def == NULL ||
		    source->space->index_map[0]->def->type != TREE ||
		    source->space->index_map[0]->def->key_def == NULL)
			return SQL_SELECT_PREFLIGHT_SHAPE;
		if (order->a[0].pExpr == NULL ||
		    ExprHasProperty(order->a[0].pExpr, EP_TokenOnly | EP_Reduced) ||
		    order->a[0].pExpr->op != TK_COLUMN_REF ||
		    order->a[0].pExpr->iColumn < 0)
			return SQL_SELECT_PREFLIGHT_SHAPE;
		const struct key_def *key_def =
			source->space->index_map[0]->def->key_def;
		/* A leading prefix of a TREE secondary index may provide
		 * order for an unordered full scan or a compatible leading-key range.
		 * The physical producer validates which access actually applies.
		 */
		if ((select->pWhere == NULL || order->nExpr == 1) &&
		    order->a[0].pExpr->iTable == source->iCursor) {
			for (uint32_t index_no = 1;
			     index_no < source->space->index_count; ++index_no) {
				const struct index *index =
					source->space->index_map[index_no];
				if (index == NULL || index->def == NULL ||
				    index->def->type != TREE ||
				    index->def->key_def == NULL ||
				    index->def->key_def->part_count == 0 ||
				    (uint32_t)order->nExpr >
					index->def->key_def->part_count)
					continue;
				bool matches_prefix = true;
				bool natural_order = true;
				bool reverse_order = true;
				for (int term = 0; term < order->nExpr; ++term) {
					const struct Expr *expr = order->a[term].pExpr;
					const struct key_def *candidate =
						index->def->key_def;
					enum sort_order index_order =
						candidate->parts[term].sort_order;
					enum sort_order requested_order =
						order->a[term].sort_order;
					if (requested_order == SORT_ORDER_UNDEF)
						requested_order = SORT_ORDER_ASC;
					bool invalid_index_order =
						index_order != SORT_ORDER_ASC &&
						index_order != SORT_ORDER_DESC;
					if (invalid_index_order || expr == NULL ||
					    expr->op != TK_COLUMN_REF ||
					    expr->pLeft != NULL || expr->pRight != NULL ||
					    expr->iTable != source->iCursor ||
					    expr->iColumn < 0 ||
					    candidate->parts[term].fieldno !=
							(uint32_t)expr->iColumn) {
						matches_prefix = false;
						break;
					}
					natural_order &= requested_order == index_order;
					reverse_order &= requested_order != index_order;
				}
				if (!matches_prefix ||
				    (!natural_order && !reverse_order))
					continue;
				key_def = index->def->key_def;
				break;
			}
		}
		uint32_t first_part = UINT32_MAX;
		for (uint32_t part = 0; part < key_def->part_count; ++part) {
			if (key_def->parts[part].fieldno ==
			    (uint32_t)order->a[0].pExpr->iColumn) {
				first_part = part;
				break;
			}
		}
		if (first_part == UINT32_MAX ||
		    (uint32_t)order->nExpr > key_def->part_count - first_part)
			return SQL_SELECT_PREFLIGHT_SHAPE;
		bool reverse_walk = false;
		for (int i = 0; i < order->nExpr; ++i) {
			const struct Expr *expr = order->a[i].pExpr;
			enum sort_order direction = order->a[i].sort_order;
			if (direction == SORT_ORDER_UNDEF)
				direction = SORT_ORDER_ASC;
			const struct key_part *part =
				&key_def->parts[first_part + (uint32_t)i];
			enum sort_order key_direction = part->sort_order ==
				SORT_ORDER_DESC ? SORT_ORDER_DESC : SORT_ORDER_ASC;
			if (i == 0)
				reverse_walk = direction != key_direction;
			enum sort_order expected_direction = reverse_walk ?
				(key_direction == SORT_ORDER_ASC ? SORT_ORDER_DESC :
				 SORT_ORDER_ASC) : key_direction;
			if (expr == NULL || ExprHasProperty(expr,
							EP_TokenOnly | EP_Reduced) ||
			    expr->op != TK_COLUMN_REF || expr->pLeft != NULL ||
			    expr->pRight != NULL || expr->iTable != source->iCursor ||
			    expr->iColumn < 0 ||
			    (uint32_t)expr->iColumn !=
				key_def->parts[first_part + (uint32_t)i].fieldno ||
			    (direction != SORT_ORDER_ASC && direction != SORT_ORDER_DESC) ||
			    direction != expected_direction)
				return SQL_SELECT_PREFLIGHT_SHAPE;
		}
	}
	if (select->pWhere != NULL) {
		const struct Expr *where = select->pWhere;
		if (has_unsupported_in_operand(where, source->iCursor,
					       source->space->def->field_count, 0))
			return SQL_SELECT_PREFLIGHT_FILTER;
		if ((where->op == TK_NOTNULL || where->op == TK_ISNULL) &&
		    where->pLeft != NULL &&
		    where->pRight == NULL &&
		    where->pLeft->op == TK_COLUMN_REF &&
		    where->pLeft->pLeft == NULL && where->pLeft->pRight == NULL &&
		    where->pLeft->iTable == source->iCursor &&
		    where->pLeft->iColumn >= 0 &&
		    (uint32_t)where->pLeft->iColumn <
			source->space->def->field_count) {
			/* The producer validates that this is the primary-key column.
			 * Only that column is guaranteed non-null by the schema. */
		} else {
			size_t term_count = 0;
			if (!is_filter_predicate_tree(where, source->iCursor,
						       source->space->def->field_count,
						       &term_count, 0)) {
				/* Validate the mixed conjunction below without allowing
				 * arbitrary expression terms. */
				return SQL_SELECT_PREFLIGHT_SHAPE;
			}
		}
	}
	if (source->iCursor < 0 || select->pEList == NULL ||
	    select->pEList->nExpr <= 0)
		return SQL_SELECT_PREFLIGHT_PROJECTION;
	size_t cursor_count = (size_t)source->iCursor + 1;
	if (cursor_count > SIZE_MAX / sizeof(uint32_t))
		return SQL_SELECT_PREFLIGHT_PROJECTION;
	uint32_t *cursor_to_relation = malloc(cursor_count * sizeof(uint32_t));
	if (cursor_to_relation == NULL)
		return SQL_SELECT_PREFLIGHT_PROJECTION;
	for (size_t i = 0; i < cursor_count; ++i)
		cursor_to_relation[i] = UINT32_MAX;
	cursor_to_relation[source->iCursor] = 0;
	bool supported_projection = true;
	bool column_binding_failure = false;
	for (int i = 0; i < select->pEList->nExpr; ++i) {
		const struct Expr *expr = select->pEList->a[i].pExpr;
		if (!is_supported_projection_expr(expr, source->iCursor,
					  source->space->def->field_count,
					  cursor_to_relation, cursor_count)) {
			supported_projection = false;
			column_binding_failure = expr != NULL &&
				expr->op == TK_COLUMN_REF;
			break;
		}
	}
	free(cursor_to_relation);
	if (!supported_projection)
		return column_binding_failure ?
			SQL_SELECT_PREFLIGHT_COLUMN_BINDING :
			SQL_SELECT_PREFLIGHT_PROJECTION;
	return supported_destination ? SQL_SELECT_PREFLIGHT_OK :
		SQL_SELECT_PREFLIGHT_DESTINATION;
}
