#include "sql_replay_expr_list.h"

#include <stdlib.h>
#include <string.h>

#include "sqlInt.h"
#include "sql_expr_canonical.h"

void
sql_replay_expr_list_destroy(struct sql_replay_expr_list *list)
{
	if (list == NULL)
		return;
	for (size_t i = 0; i < list->count; i++)
		free(list->items[i]);
	free(list->items);
	*list = (struct sql_replay_expr_list){};
}

enum sql_replay_expr_list_status
sql_replay_expr_list_create(const struct ExprList *expressions,
			    const uint32_t *cursor_to_relation,
			    size_t cursor_count,
			    struct sql_replay_expr_list *result)
{
	if (result == NULL)
		return SQL_REPLAY_EXPR_LIST_MALFORMED;
	*result = (struct sql_replay_expr_list){};
	if (expressions == NULL)
		return SQL_REPLAY_EXPR_LIST_OK;
	if (expressions->nExpr < 0 || (expressions->nExpr != 0 &&
	    expressions->a == NULL) || (cursor_count != 0 &&
	    cursor_to_relation == NULL))
		return SQL_REPLAY_EXPR_LIST_MALFORMED;
	if ((size_t)expressions->nExpr > SIZE_MAX / sizeof(char *))
		return SQL_REPLAY_EXPR_LIST_MALFORMED;
	struct sql_replay_expr_list list = {
		.count = (size_t)expressions->nExpr,
	};
	if (list.count != 0) {
		list.items = calloc(list.count, sizeof(*list.items));
		if (list.items == NULL)
			return SQL_REPLAY_EXPR_LIST_NOMEM;
	}
	for (size_t i = 0; i < list.count; i++) {
		enum sql_expr_canonical_reject reason;
		list.items[i] = sql_expr_canonicalize(expressions->a[i].pExpr,
			cursor_to_relation, cursor_count, &reason);
		if (list.items[i] != NULL)
			continue;
		sql_replay_expr_list_destroy(&list);
		switch (reason) {
		case SQL_EXPR_CANONICAL_UNSUPPORTED:
			return SQL_REPLAY_EXPR_LIST_UNSUPPORTED;
		case SQL_EXPR_CANONICAL_NOMEM:
			return SQL_REPLAY_EXPR_LIST_NOMEM;
		case SQL_EXPR_CANONICAL_OK:
		case SQL_EXPR_CANONICAL_MALFORMED:
			return SQL_REPLAY_EXPR_LIST_MALFORMED;
		}
	}
	*result = list;
	return SQL_REPLAY_EXPR_LIST_OK;
}
