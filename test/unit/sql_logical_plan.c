#include <stdlib.h>
#include <string.h>

#include "box/sql/sqlInt.h"
#include "box/sql/sql_logical_plan.h"
#include "box/space.h"
#include "unit.h"

int
main(void)
{
	plan(11);
	struct space_def *def = calloc(1, sizeof(*def) + sizeof("t1"));
	strcpy(def->name, "t1");
	def->id = 512;
	struct space space = {.def = def};
	struct SrcList source = {.nSrc = 1};
	source.a[0].space = &space;
	struct Expr predicate = {0};
	struct Expr projection_expr = {0};
	struct ExprList_item projection_item = {.pExpr = &projection_expr};
	struct ExprList projection = {.nExpr = 1, .a = &projection_item};
	struct Expr sort_expr = {0};
	struct ExprList_item sort_item = {.pExpr = &sort_expr};
	struct ExprList sort = {.nExpr = 1, .a = &sort_item};
	struct Expr limit = {0}, offset = {0};
	struct Select select = {
		.selFlags = SF_Resolved,
		.pEList = &projection,
		.pSrc = &source,
		.pWhere = &predicate,
		.pOrderBy = &sort,
		.pLimit = &limit,
		.pOffset = &offset,
	};
	enum sql_logical_reject_reason reason;
	struct sql_logical_plan *plan =
		sql_logical_plan_from_select(&select, &reason);
	ok(plan != NULL, "resolved single-table SELECT builds logical IR");
	ok(reason == SQL_LOGICAL_REJECT_NONE, "supported shape has no reject reason");
	const struct sql_logical_node *node = sql_logical_plan_root(plan);
	ok(node != NULL && node->op == SQL_LOGICAL_LIMIT, "limit is root operator");
	ok(node->expr == &limit && node->expr2 == &offset,
	   "limit and offset remain borrowed statement expressions");
	node = node->input;
	ok(node->op == SQL_LOGICAL_SORT && node->expr_list == &sort,
	   "sort retains resolved order expression list");
	node = node->input;
	ok(node->op == SQL_LOGICAL_PROJECT && node->expr_list == &projection,
	   "project retains resolved target list");
	node = node->input;
	ok(node->op == SQL_LOGICAL_FILTER && node->expr == &predicate,
	   "filter retains resolved predicate");
	node = node->input;
	ok(node->op == SQL_LOGICAL_SCAN && node->space_id == 512 &&
	    strcmp(node->space_name, "t1") == 0,
	   "scan identifies its resolved base relation");
	sql_logical_plan_delete(plan);

	select.pSrc = NULL;
	plan = sql_logical_plan_from_select(&select, &reason);
	ok(plan == NULL && reason == SQL_LOGICAL_REJECT_RELATION_COUNT,
	   "missing source rejected with stable reason");
	select.pSrc = &source;
	source.a[0].fg.notIndexed = true;
	plan = sql_logical_plan_from_select(&select, &reason);
	ok(plan == NULL && reason == SQL_LOGICAL_REJECT_ACCESS_HINT,
	   "NOT INDEXED rejected because access constraints are not modeled");
	source.a[0].fg.notIndexed = false;
	source.a[0].fg.isIndexedBy = true;
	plan = sql_logical_plan_from_select(&select, &reason);
	ok(plan == NULL && reason == SQL_LOGICAL_REJECT_ACCESS_HINT,
	   "INDEXED BY rejected because access constraints are not modeled");

	free(def);
	check_plan();
	return 0;
}
