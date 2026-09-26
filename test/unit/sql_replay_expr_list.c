#include <stdlib.h>
#include <string.h>

#include "box/sql/sqlInt.h"
#include "box/sql/sql_replay_expr_list.h"
#include "unit.h"

static void
test_ordered_detached_list(void)
{
	plan(3);
	header();
	const uint32_t cursor_map[] = {UINT32_MAX, 0};
	struct Expr column = {
		.op = TK_COLUMN_REF, .flags = EP_Resolved, .iTable = 1,
		.iColumn = 0,
	};
	struct Expr integer = {
		.op = TK_INTEGER, .flags = EP_Resolved | EP_IntValue,
		.u.iValue = 7,
	};
	struct Expr plus = {
		.op = TK_PLUS, .flags = EP_Resolved, .pLeft = &column,
		.pRight = &integer,
	};
	struct ExprList_item items[] = {{.pExpr = &column}, {.pExpr = &plus}};
	struct ExprList expressions = {.nExpr = 2, .a = items};
	struct sql_replay_expr_list list;
	ok(sql_replay_expr_list_create(&expressions, cursor_map, 2, &list) ==
	   SQL_REPLAY_EXPR_LIST_OK && list.count == 2 &&
	   strcmp(list.items[0], "col(r0,c0)") == 0 &&
	   strcmp(list.items[1], "plus(col(r0,c0),int(7))") == 0,
	   "ordered resolved expressions become detached canonical strings");
	items[0].pExpr = NULL;
	ok(strcmp(list.items[0], "col(r0,c0)") == 0,
	   "detached expressions do not borrow the source AST");
	sql_replay_expr_list_destroy(&list);
	ok(list.items == NULL && list.count == 0,
	   "detached expression-list ownership releases cleanly");
	footer();
	check_plan();
}

static void
test_rejects_partial_lists(void)
{
	plan(3);
	header();
	struct Expr integer = {
		.op = TK_INTEGER, .flags = EP_Resolved | EP_IntValue,
		.u.iValue = 1,
	};
	struct Expr function = {
		.op = TK_FUNCTION, .flags = EP_Resolved | EP_ConstFunc,
		.u.zToken = "abs",
	};
	struct ExprList_item items[] = {{.pExpr = &integer}, {.pExpr = &function}};
	struct ExprList expressions = {.nExpr = 2, .a = items};
	struct sql_replay_expr_list list = {};
	ok(sql_replay_expr_list_create(&expressions, NULL, 0, &list) ==
	   SQL_REPLAY_EXPR_LIST_UNSUPPORTED && list.items == NULL &&
	   list.count == 0,
	   "unsupported member rejects the whole list without partial ownership");
	expressions.nExpr = -1;
	ok(sql_replay_expr_list_create(&expressions, NULL, 0, &list) ==
	   SQL_REPLAY_EXPR_LIST_MALFORMED,
	   "malformed expression-list metadata is rejected");
	ok(sql_replay_expr_list_create(NULL, NULL, 0, &list) ==
	   SQL_REPLAY_EXPR_LIST_OK && list.count == 0,
	   "an absent optional expression list becomes an empty detached list");
	sql_replay_expr_list_destroy(&list);
	footer();
	check_plan();
}

int
main(void)
{
	test_ordered_detached_list();
	test_rejects_partial_lists();
	return 0;
}
