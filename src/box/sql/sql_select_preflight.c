#include "sql_select_preflight.h"

#include "sqlInt.h"
#include "sql_plan_descriptor.h"
#include "box/index.h"
#include "box/index_def.h"
#include "box/key_def.h"
#include "box/space.h"

static bool
is_comparison_predicate(const struct Expr *expr)
{
	return expr != NULL && expr->pLeft != NULL && expr->pRight != NULL &&
		(expr->op == TK_EQ || expr->op == TK_GT || expr->op == TK_GE ||
		 expr->op == TK_LT || expr->op == TK_LE);
}

static bool
is_comparison_conjunction(const struct Expr *expr, size_t *term_count,
			  size_t depth)
{
	if (expr == NULL || term_count == NULL ||
	    depth >= SQL_PLAN_POINT_KEY_PART_MAX)
		return false;
	if (expr->op == TK_AND && expr->pLeft != NULL && expr->pRight != NULL)
		return is_comparison_conjunction(expr->pLeft, term_count,
						 depth + 1) &&
			is_comparison_conjunction(expr->pRight, term_count,
						   depth + 1);
	if (!is_comparison_predicate(expr) ||
	    *term_count == SQL_PLAN_POINT_KEY_PART_MAX)
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
	if (dest == NULL || dest->eDest != SRT_Output ||
	    dest->pOrderBy != NULL)
		return SQL_SELECT_PREFLIGHT_DESTINATION;
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
		    source->space->index_map[0]->def->key_def == NULL ||
		    (uint32_t)order->nExpr >
			source->space->index_map[0]->def->key_def->part_count)
			return SQL_SELECT_PREFLIGHT_SHAPE;
		const struct key_def *key_def =
			source->space->index_map[0]->def->key_def;
		enum sort_order order_direction = SORT_ORDER_UNDEF;
		for (int i = 0; i < order->nExpr; ++i) {
			const struct Expr *expr = order->a[i].pExpr;
			enum sort_order direction = order->a[i].sort_order;
			if (direction == SORT_ORDER_UNDEF)
				direction = SORT_ORDER_ASC;
			if (expr == NULL || ExprHasProperty(expr,
							EP_TokenOnly | EP_Reduced) ||
			    expr->op != TK_COLUMN_REF || expr->pLeft != NULL ||
			    expr->pRight != NULL || expr->iTable != source->iCursor ||
			    expr->iColumn < 0 ||
			    (uint32_t)expr->iColumn != key_def->parts[i].fieldno ||
			    (direction != SORT_ORDER_ASC && direction != SORT_ORDER_DESC) ||
			    (order_direction != SORT_ORDER_UNDEF &&
			     direction != order_direction))
				return SQL_SELECT_PREFLIGHT_SHAPE;
			order_direction = direction;
		}
	}
	if (select->pWhere != NULL) {
		const struct Expr *where = select->pWhere;
		if ((where->op == TK_NOTNULL || where->op == TK_ISNULL) &&
		    where->pLeft != NULL &&
		    where->pRight == NULL &&
		    where->pLeft->op == TK_COLUMN_REF &&
		    where->pLeft->pLeft == NULL && where->pLeft->pRight == NULL) {
			/* The producer validates that this is the primary-key column.
			 * Only that column is guaranteed non-null by the schema. */
		} else if (where->op == TK_AND) {
			size_t term_count = 0;
			if (!is_comparison_conjunction(where, &term_count, 0))
				return SQL_SELECT_PREFLIGHT_SHAPE;
		} else if (!is_comparison_predicate(where)) {
			return SQL_SELECT_PREFLIGHT_SHAPE;
		}
	}
	if (source->iCursor < 0 || select->pEList == NULL ||
	    select->pEList->nExpr <= 0)
		return SQL_SELECT_PREFLIGHT_PROJECTION;
	for (int i = 0; i < select->pEList->nExpr; ++i) {
		const struct Expr *expr = select->pEList->a[i].pExpr;
		if (expr == NULL || ExprHasProperty(expr, EP_TokenOnly | EP_Reduced) ||
		    expr->op != TK_COLUMN_REF || expr->pLeft != NULL ||
		    expr->pRight != NULL)
			return SQL_SELECT_PREFLIGHT_PROJECTION;
		if (expr->iTable != source->iCursor || expr->iColumn < 0 ||
		    (uint32_t)expr->iColumn >= source->space->def->field_count)
			return SQL_SELECT_PREFLIGHT_COLUMN_BINDING;
	}
	return SQL_SELECT_PREFLIGHT_OK;
}
