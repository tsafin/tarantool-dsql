#include <stdlib.h>
#include <string.h>

#include "box/sql/sqlInt.h"
#include "box/sql/sql_select_preflight.h"
#include "box/index.h"
#include "box/index_def.h"
#include "box/key_def.h"
#include "box/space.h"
#include "unit.h"

static void
test_preflight(void)
{
	plan(22);
	header();
	struct space_def *def = calloc(1, sizeof(*def) + sizeof("preflight_t"));
	strcpy(def->name, "preflight_t");
	def->id = 512;
	def->field_count = 3;
	struct key_def *key_def = calloc(1, sizeof(*key_def) +
					 2 * sizeof(key_def->parts[0]));
	key_def->part_count = 2;
	key_def->parts[0].fieldno = 0;
	key_def->parts[1].fieldno = 2;
	struct index_def index_def = {.type = TREE, .key_def = key_def};
	struct index index = {.def = &index_def};
	struct index *indexes[] = {&index};
	struct space space = {.def = def, .index_map = indexes};
	struct SrcList source = {.nSrc = 1};
	source.a[0].space = &space;
	source.a[0].iCursor = 4;
	struct Expr expr = {.op = TK_COLUMN_REF, .iTable = 4, .iColumn = 2};
	struct ExprList_item item = {.pExpr = &expr};
	struct ExprList projection = {.nExpr = 1, .a = &item};
	struct Select select = {
		.selFlags = SF_Resolved,
		.pEList = &projection,
		.pSrc = &source,
	};
	struct SelectDest dest = {.eDest = SRT_Output};
	struct Select select_before;
	struct SelectDest dest_before;
	struct SrcList source_before;
	struct Expr expr_before;
	memcpy(&select_before, &select, sizeof(select));
	memcpy(&dest_before, &dest, sizeof(dest));
	memcpy(&source_before, &source, sizeof(source));
	memcpy(&expr_before, &expr, sizeof(expr));
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "resolved one-table column projection is preflight eligible");
	ok(memcmp(&select, &select_before, sizeof(select)) == 0 &&
	   memcmp(&dest, &dest_before, sizeof(dest)) == 0 &&
	   memcmp(&source, &source_before, sizeof(source)) == 0 &&
	   memcmp(&expr, &expr_before, sizeof(expr)) == 0,
	   "successful preflight leaves SELECT, destination, and AST unchanged");

	select.selFlags &= ~SF_Resolved;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_UNRESOLVED,
	   "unresolved SELECT is rejected explicitly");
	select.selFlags |= SF_Resolved;
	dest.eDest = SRT_EphemTab;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_DESTINATION,
	   "non-output destination is rejected explicitly");
	dest.eDest = SRT_Output;
	select.pWhere = &expr;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_SHAPE,
	   "filtered SELECT is rejected explicitly");
	struct Expr null_test_column = {
		.op = TK_COLUMN_REF, .iTable = 4, .iColumn = 0,
	};
	struct Expr null_test = {.op = TK_ISNULL, .pLeft = &null_test_column};
	select.pWhere = &null_test;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "unary IS NULL test reaches producer validation");
	null_test.op = TK_NOTNULL;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "unary IS NOT NULL test reaches producer validation");
	struct Expr range_column = {
		.op = TK_COLUMN_REF, .iTable = 4, .iColumn = 0,
	};
	struct Expr range_value = {.op = TK_INTEGER};
	struct Expr range_test = {
		.op = TK_GT, .pLeft = &range_column, .pRight = &range_value,
	};
	struct Expr combined_filter = {
		.op = TK_AND, .pLeft = &null_test, .pRight = &range_test,
	};
	select.pWhere = &combined_filter;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "null residual plus primary-key bound conjunction reaches producer");
	struct Expr disjunctive_filter = {
		.op = TK_OR, .pLeft = &range_test, .pRight = &null_test,
	};
	select.pWhere = &disjunctive_filter;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "bounded comparison/null disjunction reaches producer validation");
	struct Expr negated_filter = {
		.op = TK_NOT, .pLeft = &disjunctive_filter,
	};
	select.pWhere = &negated_filter;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "NOT over a bounded comparison/null tree reaches producer validation");
	select.pWhere = NULL;
	struct Expr order_expr = {
		.op = TK_COLUMN_REF, .iTable = 4, .iColumn = 0,
	};
	struct ExprList_item order_item = {.pExpr = &order_expr};
	struct ExprList order_list = {.nExpr = 1, .a = &order_item};
	select.pOrderBy = &order_list;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "primary-key prefix ordering reaches producer validation");
	struct Expr order_expr2 = {
		.op = TK_COLUMN_REF, .iTable = 4, .iColumn = 2,
	};
	struct ExprList_item order_items[] = {
		{.pExpr = &order_expr}, {.pExpr = &order_expr2},
	};
	order_list.nExpr = 2;
	order_list.a = order_items;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_OK,
	   "complete composite primary-key ordering reaches producer validation");
	order_items[1].sort_order = SORT_ORDER_DESC;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_SHAPE,
	   "mixed-direction composite primary-key ordering is rejected");
	order_items[1].sort_order = SORT_ORDER_UNDEF;
	order_expr.iColumn = 2;
	order_expr2.iColumn = 0;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_SHAPE,
	   "non-prefix composite primary-key ordering is rejected");
	order_expr.iColumn = 0;
	order_expr2.iColumn = 2;
	order_list.nExpr = 1;
	order_list.a = &order_item;
	order_expr.iColumn = 1;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_SHAPE,
	   "non-primary ordering is rejected before physical attempt");
	order_expr.iColumn = 0;
	select.pOrderBy = NULL;
	expr.op = TK_PLUS;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_PROJECTION,
	   "computed projection is rejected explicitly");
	expr.op = TK_COLUMN_REF;
	expr.iTable = 5;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_COLUMN_BINDING,
	   "projection bound to another cursor is rejected");
	expr.iTable = 4;
	expr.iColumn = 3;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_COLUMN_BINDING,
	   "out-of-range projection column is rejected");
	expr.iColumn = 2;
	source.a[0].fg.notIndexed = true;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_SHAPE,
	   "explicit access hint is rejected");
	source.a[0].fg.notIndexed = false;
	dest.pOrderBy = &projection;
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_DESTINATION,
	   "ordered SELECT is rejected");
	dest.pOrderBy = NULL;
	space.def->opts.is_view = true;
	memcpy(&select_before, &select, sizeof(select));
	memcpy(&dest_before, &dest, sizeof(dest));
	memcpy(&source_before, &source, sizeof(source));
	memcpy(&expr_before, &expr, sizeof(expr));
	ok(sql_select_preflight_table_scan(&select, &dest) ==
	   SQL_SELECT_PREFLIGHT_RELATION,
	   "view source is rejected as non-base relation");
	ok(memcmp(&select, &select_before, sizeof(select)) == 0 &&
	   memcmp(&dest, &dest_before, sizeof(dest)) == 0 &&
	   memcmp(&source, &source_before, sizeof(source)) == 0 &&
	   memcmp(&expr, &expr_before, sizeof(expr)) == 0,
	   "rejected preflight also leaves all caller inputs unchanged");
	free(def);
	free(key_def);
	footer();
	check_plan();
}

int
main(void)
{
	test_preflight();
	return 0;
}
