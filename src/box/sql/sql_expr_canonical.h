#ifndef TARANTOOL_SQL_EXPR_CANONICAL_H
#define TARANTOOL_SQL_EXPR_CANONICAL_H

#include <stddef.h>
#include <stdint.h>

struct Expr;

enum sql_expr_canonical_reject {
	SQL_EXPR_CANONICAL_OK = 0,
	SQL_EXPR_CANONICAL_UNSUPPORTED,
	SQL_EXPR_CANONICAL_MALFORMED,
	SQL_EXPR_CANONICAL_NOMEM,
};

/* Return an owned canonical encoding, or NULL and a conservative reason. */
char *
sql_expr_canonicalize(const struct Expr *expr,
		      const uint32_t *cursor_to_relation,
		      size_t cursor_count,
		      enum sql_expr_canonical_reject *reason);

#endif
