#include <stdlib.h>
#include <string.h>
#include "box/sql/sqlInt.h"
#include "box/sql/sql_expr_canonical.h"
#include "unit.h"
static void test_supported(void) {
	plan(15); header();
	const uint32_t cursor_map[] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, 0};
	struct Expr col = {.op=TK_COLUMN_REF,.flags=EP_Resolved,.iTable=3,.iColumn=1};
	struct Expr a = {.op=TK_INTEGER,.flags=EP_Resolved|EP_IntValue}; a.u.iValue=7;
	struct Expr b = {.op=TK_INTEGER,.flags=EP_Resolved,.u.zToken="007"};
	struct Expr min_operand = {.op=TK_INTEGER,.flags=EP_Resolved,
		.u.zToken="9223372036854775808"};
	struct Expr min_value = {.op=TK_UMINUS,.flags=EP_Resolved,
		.pLeft=&min_operand};
	struct Expr plus = {.op=TK_PLUS,.flags=EP_Resolved,.pLeft=&col,.pRight=&a};
	char *s=sql_expr_canonicalize(&col,cursor_map,4,NULL);
	char *s2=sql_expr_canonicalize(&col,cursor_map,4,NULL);
	ok(s && s2 && strcmp(s,s2)==0 && strcmp(s,"col(r0,c1)")==0,"mapped column stable");
	struct Expr lookup_col = {.op=TK_COLUMN_REF,
		.flags=EP_Resolved|EP_Lookup2,.iTable=3,.iColumn=1};
	char *lookup = sql_expr_canonicalize(&lookup_col, cursor_map, 4, NULL);
	ok(lookup && strcmp(lookup, "col(r0,c1)") == 0,
	   "resolved identifier lookup marker has no canonical meaning");
	struct Expr no_reduce_col = {.op=TK_COLUMN_REF,
		.flags=EP_Resolved|EP_NoReduce,.iTable=3,.iColumn=1};
	char *no_reduce = sql_expr_canonicalize(&no_reduce_col, cursor_map, 4,
						NULL);
	ok(no_reduce && strcmp(no_reduce, "col(r0,c1)") == 0,
	   "column size-optimization marker has no canonical meaning");
	char *n=sql_expr_canonicalize(&a,NULL,0,NULL), *n2=sql_expr_canonicalize(&b,NULL,0,NULL);
	ok(n && n2 && strcmp(n,n2)==0 && strcmp(n,"int(7)")==0,"integer normalized");
	char *min=sql_expr_canonicalize(&min_value,NULL,0,NULL);
	ok(min && strcmp(min,"int(-9223372036854775808)")==0,
	   "minimum signed integer literal canonicalized exactly");
	char *o=sql_expr_canonicalize(&plus,cursor_map,4,NULL);
	ok(o && strcmp(o,"plus(col(r0,c1),int(7))")==0,"operator structure encoded");
	struct Expr nul={.op=TK_NULL,.flags=EP_Resolved};
	struct Expr str={.op=TK_STRING,.flags=EP_Resolved,.u.zToken="a\"b"};
	struct Expr f={.op=TK_FLOAT,.flags=EP_Resolved,.u.zToken="1.0"};
	struct Expr f2={.op=TK_FLOAT,.flags=EP_Resolved,.u.zToken="1e0"};
	struct Expr blob={.op=TK_BLOB,.flags=EP_Resolved,.u.zToken="X'A0Ff'"};
	struct Expr boolean={.op=TK_TRUE,.flags=EP_Resolved};
	struct Expr bound_low={.op=TK_STRING,.flags=EP_Resolved,.u.zToken="a"};
	struct Expr bound_high={.op=TK_STRING,.flags=EP_Resolved,.u.zToken="z"};
	struct ExprList_item between_items[] = {
		{.pExpr = &bound_low}, {.pExpr = &bound_high},
	};
	struct ExprList between_list={.nExpr=2,.a=between_items};
	struct Expr between={.op=TK_BETWEEN,.flags=EP_Resolved,
		.pLeft=&col,.x.pList=&between_list};
	struct ExprList_item in_items[] = {
		{.pExpr = &bound_low}, {.pExpr = &bound_high},
	};
	struct ExprList in_list={.nExpr=2,.a=in_items};
	struct Expr in={.op=TK_IN,.flags=EP_Resolved,
		.pLeft=&col,.x.pList=&in_list};
	struct Expr function_arg = {.op=TK_COLUMN_REF,.flags=EP_Resolved,
		.iTable=3,.iColumn=1};
	struct ExprList_item function_items[] = {{.pExpr=&function_arg}};
	struct ExprList function_list={.nExpr=1,.a=function_items};
	struct Expr function={.op=TK_FUNCTION,
		.flags=EP_Resolved|EP_ConstFunc|EP_Lookup2,.u.zToken="ABS",
		.x.pList=&function_list};
	struct Expr collated={.op=TK_COLLATE,
		.flags=EP_Resolved|EP_Collate|EP_Skip,.u.zToken="unicode_ci",
		.pLeft=&col};
	char *ns=sql_expr_canonicalize(&nul,NULL,0,NULL), *ss=sql_expr_canonicalize(&str,NULL,0,NULL);
	char *fs=sql_expr_canonicalize(&f,NULL,0,NULL), *fs2=sql_expr_canonicalize(&f2,NULL,0,NULL);
	ok(ns && strcmp(ns,"null")==0,"NULL encoded");
	ok(ss && strcmp(ss,"str(612262)")==0,"string bytes hex encoded");
	ok(fs && fs2 && strcmp(fs,fs2)==0,"float spelling normalized");
	char *bs=sql_expr_canonicalize(&blob,NULL,0,NULL);
	ok(bs && strcmp(bs,"blob(a0ff)")==0,"blob hex canonicalized case-insensitively");
	char *bools=sql_expr_canonicalize(&boolean,NULL,0,NULL);
	ok(bools && strcmp(bools,"bool(true)")==0,"boolean literal canonicalized");
	char *between_s=sql_expr_canonicalize(&between,cursor_map,4,NULL);
	ok(between_s && strcmp(between_s,
		"between(col(r0,c1),str(61),str(7a))")==0,
	   "BETWEEN expression and bounds canonicalized");
	char *in_s=sql_expr_canonicalize(&in,cursor_map,4,NULL);
	ok(in_s && strcmp(in_s, "in(col(r0,c1),str(61),str(7a))")==0,
	   "IN expression and list canonicalized");
	char *function_s=sql_expr_canonicalize(&function,cursor_map,4,NULL);
	ok(function_s && strcmp(function_s, "func3:616273(col(r0,c1))")==0,
	   "deterministic function identity and ordered args canonicalized");
	char *collated_s=sql_expr_canonicalize(&collated,cursor_map,4,NULL);
	ok(collated_s && strcmp(collated_s,
		"collate10:756e69636f64655f6369(col(r0,c1))")==0,
	   "explicit collation and operand canonicalized case-insensitively");
	free(s);free(s2);free(lookup);free(no_reduce);free(n);free(n2);free(min);free(o);free(ns);free(ss);free(fs);free(fs2);free(bs);free(bools);free(between_s);free(in_s);free(function_s);free(collated_s);
	footer(); check_plan();
}
static void test_rejects(void) {
	plan(11); header(); enum sql_expr_canonical_reject r;
	struct Expr fn={.op=TK_FUNCTION,.flags=EP_Resolved,.u.zToken="abs"};
	ok(!sql_expr_canonicalize(&fn,NULL,0,&r)&&r==SQL_EXPR_CANONICAL_UNSUPPORTED,"nondeterministic function rejected");
	struct Expr j={.op=TK_COLUMN_REF,.flags=EP_Resolved|EP_FromJoin,.iTable=0,.iColumn=1};
	ok(!sql_expr_canonicalize(&j,NULL,0,&r)&&r==SQL_EXPR_CANONICAL_UNSUPPORTED,"join annotation rejected");
	struct Expr red={.op=TK_COLUMN_REF,.flags=EP_Resolved|EP_Reduced,.iTable=0,.iColumn=1};
	ok(!sql_expr_canonicalize(&red,NULL,0,&r),"reduced node rejected");
	struct Expr un={.op=TK_INTEGER,.u.zToken="3"};
	ok(!sql_expr_canonicalize(&un,NULL,0,&r),"unresolved node rejected");
	struct Expr unk={.op=TK_SELECT,.flags=EP_Resolved};
	ok(!sql_expr_canonicalize(&unk,NULL,0,&r),"unknown operator rejected");
	struct Expr bad={.op=TK_PLUS,.flags=EP_Resolved};
	ok(!sql_expr_canonicalize(&bad,NULL,0,&r)&&r==SQL_EXPR_CANONICAL_MALFORMED,"bad arity rejected");
	struct Expr col={.op=TK_COLUMN_REF,.flags=EP_Resolved,.iTable=0,.iColumn=0};
	ok(!sql_expr_canonicalize(&col,NULL,0,&r)&&r==SQL_EXPR_CANONICAL_UNSUPPORTED,
	   "column without cursor binding rejected");
	struct Expr bad_blob={.op=TK_BLOB,.flags=EP_Resolved,.u.zToken="X'0G'"};
	ok(!sql_expr_canonicalize(&bad_blob,NULL,0,&r)&&
	   r==SQL_EXPR_CANONICAL_MALFORMED,"malformed blob hex rejected");
	struct Expr malformed_between={.op=TK_BETWEEN,.flags=EP_Resolved};
	ok(!sql_expr_canonicalize(&malformed_between,NULL,0,&r)&&
	   r==SQL_EXPR_CANONICAL_MALFORMED,"malformed BETWEEN rejected");
	struct Expr malformed_in={.op=TK_IN,.flags=EP_Resolved};
	ok(!sql_expr_canonicalize(&malformed_in,NULL,0,&r)&&
		r==SQL_EXPR_CANONICAL_MALFORMED,"malformed IN rejected");
	struct Expr malformed_collate={.op=TK_COLLATE,
		.flags=EP_Resolved|EP_Collate|EP_Skip,.u.zToken="unicode_ci"};
	ok(!sql_expr_canonicalize(&malformed_collate,NULL,0,&r)&&
		r==SQL_EXPR_CANONICAL_MALFORMED,"malformed COLLATE rejected");
	footer(); check_plan();
}
int main(void) { test_supported(); test_rejects(); return 0; }
