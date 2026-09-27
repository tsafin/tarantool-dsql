#ifndef TARANTOOL_SQL_LOGICAL_PLAN_H
#define TARANTOOL_SQL_LOGICAL_PLAN_H

#include <stdint.h>

struct Select;
struct Expr;
struct ExprList;

enum sql_logical_op {
	SQL_LOGICAL_SCAN,
	SQL_LOGICAL_FILTER,
	SQL_LOGICAL_PROJECT,
	SQL_LOGICAL_SORT,
	SQL_LOGICAL_LIMIT,
};

enum sql_logical_reject_reason {
	SQL_LOGICAL_REJECT_NONE,
	SQL_LOGICAL_REJECT_UNRESOLVED,
	SQL_LOGICAL_REJECT_RELATION_COUNT,
	SQL_LOGICAL_REJECT_SUBQUERY,
	SQL_LOGICAL_REJECT_AGGREGATE,
	SQL_LOGICAL_REJECT_COMPOUND,
	SQL_LOGICAL_REJECT_CTE,
	SQL_LOGICAL_REJECT_DISTINCT,
	SQL_LOGICAL_REJECT_NONDETERMINISTIC,
	SQL_LOGICAL_REJECT_ACCESS_HINT,
	SQL_LOGICAL_REJECT_FUNCTION,
	SQL_LOGICAL_REJECT_COLLATION,
	SQL_LOGICAL_REJECT_EXPRESSION,
};

struct sql_logical_node {
	enum sql_logical_op op;
	const struct sql_logical_node *input;
	uint32_t space_id;
	const char *space_name;
	/* Borrowed from the resolved statement; valid for its statement lifetime. */
	const struct Expr *expr;
	const struct Expr *expr2;
	const struct ExprList *expr_list;
};

struct sql_logical_plan;

/*
 * Build a statement-lifetime logical operator chain from a resolved SELECT.
 * Structurally supported shapes are one resolved base relation with
 * WHERE/projection, ORDER BY, LIMIT and OFFSET. Explicit INDEXED BY / NOT
 * INDEXED access constraints are rejected because this prototype does not
 * model them. The caller remains responsible for excluding non-deterministic
 * or side-effecting expressions; this prototype does not inspect function
 * semantics. Expression trees are intentionally borrowed, not copied; there
 * are no rewrites or execution routing.
 */
struct sql_logical_plan *
sql_logical_plan_from_select(const struct Select *select,
			     enum sql_logical_reject_reason *reason);
void sql_logical_plan_delete(struct sql_logical_plan *plan);
const struct sql_logical_node *
sql_logical_plan_root(const struct sql_logical_plan *plan);
enum sql_logical_reject_reason
sql_logical_plan_reject_reason(const struct sql_logical_plan *plan);

#endif
