#include <stdlib.h>
#include <string.h>

#include "box/sql/sqlInt.h"
#include "box/sql/sql_select_preflight.h"
#include "box/space.h"
#include "unit.h"

static void
test_preflight(void)
{
	plan(14);
	header();
	struct space_def *def = calloc(1, sizeof(*def) + sizeof("preflight_t"));
	strcpy(def->name, "preflight_t");
	def->id = 512;
	def->field_count = 3;
	struct space space = {.def = def};
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
	select.pWhere = NULL;
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
	footer();
	check_plan();
}

int
main(void)
{
	test_preflight();
	return 0;
}
