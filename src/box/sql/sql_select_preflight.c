#include "sql_select_preflight.h"

#include "sqlInt.h"
#include "box/space.h"

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
	if (select->pWhere != NULL &&
	    (select->pWhere->op != TK_EQ || select->pWhere->pLeft == NULL ||
	     select->pWhere->pRight == NULL))
		return SQL_SELECT_PREFLIGHT_SHAPE;
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
