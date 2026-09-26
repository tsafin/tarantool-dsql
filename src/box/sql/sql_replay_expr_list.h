#ifndef TARANTOOL_SQL_REPLAY_EXPR_LIST_H
#define TARANTOOL_SQL_REPLAY_EXPR_LIST_H

#include <stddef.h>
#include <stdint.h>

struct ExprList;

struct sql_replay_expr_list {
	char **items;
	size_t count;
};

enum sql_replay_expr_list_status {
	SQL_REPLAY_EXPR_LIST_OK = 0,
	SQL_REPLAY_EXPR_LIST_UNSUPPORTED,
	SQL_REPLAY_EXPR_LIST_MALFORMED,
	SQL_REPLAY_EXPR_LIST_NOMEM,
};

/*
 * Canonicalize an ordered SQL expression list into detached strings. The
 * caller supplies logical relation bindings for resolved column cursors.
 * `result` is reset before use and must not own a previous list. On any
 * unsupported/malformed expression, no partial list is returned.
 */
enum sql_replay_expr_list_status
sql_replay_expr_list_create(const struct ExprList *expressions,
			    const uint32_t *cursor_to_relation,
			    size_t cursor_count,
			    struct sql_replay_expr_list *result);

void
sql_replay_expr_list_destroy(struct sql_replay_expr_list *list);

#endif /* TARANTOOL_SQL_REPLAY_EXPR_LIST_H */
