#include "sql_expr_canonical.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlInt.h"

struct buffer {
	char *data;
	size_t len;
	size_t cap;
};

static bool
append(struct buffer *b, const char *s, size_t n)
{
	if (n > SIZE_MAX - b->len - 1)
		return false;
	size_t need = b->len + n + 1;
	if (need > b->cap) {
		size_t cap = b->cap == 0 ? 64 : b->cap;
		while (cap < need) {
			if (cap > SIZE_MAX / 2) {
				cap = need;
				break;
			}
			cap *= 2;
		}
		char *data = realloc(b->data, cap);
		if (data == NULL)
			return false;
		b->data = data;
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return true;
}

static const char *
operator_name(int op)
{
	switch (op) {
	case TK_OR: return "or";
	case TK_AND: return "and";
	case TK_NOT: return "not";
	case TK_IS: return "is";
	case TK_NE: return "ne";
	case TK_EQ: return "eq";
	case TK_GT: return "gt";
	case TK_LE: return "le";
	case TK_LT: return "lt";
	case TK_GE: return "ge";
	case TK_BITAND: return "bitand";
	case TK_BITOR: return "bitor";
	case TK_LSHIFT: return "lshift";
	case TK_RSHIFT: return "rshift";
	case TK_PLUS: return "plus";
	case TK_MINUS: return "minus";
	case TK_STAR: return "multiply";
	case TK_SLASH: return "divide";
	case TK_REM: return "remainder";
	case TK_CONCAT: return "concat";
	case TK_ISNULL: return "isnull";
	case TK_NOTNULL: return "notnull";
	case TK_UMINUS: return "unary_minus";
	case TK_UPLUS: return "unary_plus";
	default: return NULL;
	}
}

static enum sql_expr_canonical_reject
encode(const struct Expr *expr, struct buffer *b, unsigned int depth,
       const uint32_t *cursor_to_relation, size_t cursor_count)
{
	if (expr == NULL || depth > 256)
		return SQL_EXPR_CANONICAL_MALFORMED;
	/* Reduced nodes omit fields; allow only fully resolved plain nodes. */
	uint32_t allowed = EP_Resolved | EP_IntValue | EP_Leaf;
	/* EP_Lookup2 remembers whether an identifier was quoted; EP_NoReduce
	 * prevents a harmless size optimization. Neither has a remaining semantic
	 * effect on a canonical TK_COLUMN_REF once name resolution has bound its
	 * cursor and field ordinal. Do not allow them on other operators.
	 */
	if (expr->op == TK_COLUMN_REF)
		allowed |= EP_Lookup2 | EP_NoReduce;
	if ((expr->flags & EP_Resolved) == 0 ||
	    (expr->flags & (EP_Reduced | EP_TokenOnly)) != 0)
		return SQL_EXPR_CANONICAL_UNSUPPORTED;
	if ((expr->flags & ~allowed) != 0)
		return SQL_EXPR_CANONICAL_UNSUPPORTED;
	if (expr->op == TK_COLUMN_REF) {
		if (expr->pLeft != NULL || expr->pRight != NULL ||
		    expr->iTable < 0 || expr->iColumn < 0)
			return SQL_EXPR_CANONICAL_MALFORMED;
		if (cursor_to_relation == NULL ||
		    (size_t)expr->iTable >= cursor_count ||
		    cursor_to_relation[expr->iTable] == UINT32_MAX)
			return SQL_EXPR_CANONICAL_UNSUPPORTED;
		char tmp[80];
		int n = snprintf(tmp, sizeof(tmp), "col(r%" PRIu32 ",c%d)",
				 cursor_to_relation[expr->iTable], expr->iColumn);
		if (n < 0 || (size_t)n >= sizeof(tmp) ||
		    !append(b, tmp, (size_t)n))
			return SQL_EXPR_CANONICAL_NOMEM;
		return SQL_EXPR_CANONICAL_OK;
	}
	if (expr->op == TK_NULL) {
		if (expr->pLeft != NULL || expr->pRight != NULL)
			return SQL_EXPR_CANONICAL_MALFORMED;
		return append(b, "null", 4) ? SQL_EXPR_CANONICAL_OK :
			SQL_EXPR_CANONICAL_NOMEM;
	}
	if (expr->op == TK_TRUE || expr->op == TK_FALSE) {
		if (expr->pLeft != NULL || expr->pRight != NULL)
			return SQL_EXPR_CANONICAL_MALFORMED;
		const char *value = expr->op == TK_TRUE ? "bool(true)" :
			"bool(false)";
		return append(b, value, strlen(value)) ? SQL_EXPR_CANONICAL_OK :
			SQL_EXPR_CANONICAL_NOMEM;
	}
	if (expr->op == TK_INTEGER) {
		if (expr->pLeft != NULL || expr->pRight != NULL)
			return SQL_EXPR_CANONICAL_MALFORMED;
		char tmp[48];
		int n;
		if ((expr->flags & EP_IntValue) != 0) {
			n = snprintf(tmp, sizeof(tmp), "int(%d)", expr->u.iValue);
		} else {
			if (expr->u.zToken == NULL)
				return SQL_EXPR_CANONICAL_MALFORMED;
			char *end;
			errno = 0;
			long long value = strtoll(expr->u.zToken, &end, 10);
			if (*expr->u.zToken == '\0' || *end != '\0' || errno == ERANGE)
				return SQL_EXPR_CANONICAL_UNSUPPORTED;
			n = snprintf(tmp, sizeof(tmp), "int(%lld)", value);
		}
		if (n < 0 || (size_t)n >= sizeof(tmp) ||
		    !append(b, tmp, (size_t)n))
			return SQL_EXPR_CANONICAL_NOMEM;
		return SQL_EXPR_CANONICAL_OK;
	}
	if (expr->op == TK_FLOAT) {
		if (expr->pLeft != NULL || expr->pRight != NULL ||
		    expr->u.zToken == NULL)
			return SQL_EXPR_CANONICAL_MALFORMED;
		char *end;
		errno = 0;
		double value = strtod(expr->u.zToken, &end);
		if (*expr->u.zToken == '\0' || *end != '\0' || errno == ERANGE ||
		    !isfinite(value))
			return SQL_EXPR_CANONICAL_UNSUPPORTED;
		char tmp[64];
		int n = snprintf(tmp, sizeof(tmp), "float(%a)", value);
		if (n < 0 || (size_t)n >= sizeof(tmp) ||
		    !append(b, tmp, (size_t)n))
			return SQL_EXPR_CANONICAL_NOMEM;
		return SQL_EXPR_CANONICAL_OK;
	}
	if (expr->op == TK_STRING) {
		if (expr->pLeft != NULL || expr->pRight != NULL ||
		    expr->u.zToken == NULL)
			return SQL_EXPR_CANONICAL_MALFORMED;
		static const char hex[] = "0123456789abcdef";
		if (!append(b, "str(", 4))
			return SQL_EXPR_CANONICAL_NOMEM;
		for (const unsigned char *p = (const unsigned char *)expr->u.zToken;
		     *p != '\0'; ++p) {
			char pair[] = {hex[*p >> 4], hex[*p & 0xf]};
			if (!append(b, pair, sizeof(pair)))
				return SQL_EXPR_CANONICAL_NOMEM;
		}
		return append(b, ")", 1) ? SQL_EXPR_CANONICAL_OK :
			SQL_EXPR_CANONICAL_NOMEM;
	}
	if (expr->op == TK_BLOB) {
		const char *token = expr->u.zToken;
		if (expr->pLeft != NULL || expr->pRight != NULL || token == NULL ||
		    (token[0] != 'x' && token[0] != 'X') || token[1] != '\'')
			return SQL_EXPR_CANONICAL_MALFORMED;
		size_t token_len = strlen(token);
		if (token_len < 3 || token[token_len - 1] != '\'')
			return SQL_EXPR_CANONICAL_MALFORMED;
		size_t hex_len = token_len - 3;
		if (hex_len % 2 != 0)
			return SQL_EXPR_CANONICAL_MALFORMED;
		if (!append(b, "blob(", 5))
			return SQL_EXPR_CANONICAL_NOMEM;
		for (size_t i = 0; i < hex_len; ++i) {
			char c = token[i + 2];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
			      (c >= 'A' && c <= 'F')))
				return SQL_EXPR_CANONICAL_MALFORMED;
			if (c >= 'A' && c <= 'F')
				c = (char)(c - 'A' + 'a');
			if (!append(b, &c, 1))
				return SQL_EXPR_CANONICAL_NOMEM;
		}
		return append(b, ")", 1) ? SQL_EXPR_CANONICAL_OK :
			SQL_EXPR_CANONICAL_NOMEM;
	}
	if (expr->op == TK_BETWEEN) {
		if (expr->pLeft == NULL || expr->pRight != NULL ||
		    expr->x.pList == NULL || expr->x.pList->nExpr != 2 ||
		    expr->x.pList->a[0].pExpr == NULL ||
		    expr->x.pList->a[1].pExpr == NULL ||
		    ExprHasProperty(expr, EP_xIsSelect))
			return SQL_EXPR_CANONICAL_MALFORMED;
		if (!append(b, "between(", 8))
			return SQL_EXPR_CANONICAL_NOMEM;
		const struct Expr *parts[] = {
			expr->pLeft,
			expr->x.pList->a[0].pExpr,
			expr->x.pList->a[1].pExpr,
		};
		for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
			if (i != 0 && !append(b, ",", 1))
				return SQL_EXPR_CANONICAL_NOMEM;
			enum sql_expr_canonical_reject rc = encode(parts[i], b,
				depth + 1, cursor_to_relation, cursor_count);
			if (rc != SQL_EXPR_CANONICAL_OK)
				return rc;
		}
		return append(b, ")", 1) ? SQL_EXPR_CANONICAL_OK :
			SQL_EXPR_CANONICAL_NOMEM;
	}
	if (expr->op == TK_IN) {
		if (expr->pLeft == NULL || expr->pRight != NULL ||
		    expr->x.pList == NULL || expr->x.pList->nExpr <= 0 ||
		    ExprHasProperty(expr, EP_xIsSelect))
			return SQL_EXPR_CANONICAL_MALFORMED;
		if (!append(b, "in(", 3))
			return SQL_EXPR_CANONICAL_NOMEM;
		enum sql_expr_canonical_reject rc = encode(expr->pLeft, b,
			depth + 1, cursor_to_relation, cursor_count);
		if (rc != SQL_EXPR_CANONICAL_OK)
			return rc;
		for (int i = 0; i < expr->x.pList->nExpr; ++i) {
			const struct Expr *item = expr->x.pList->a[i].pExpr;
			if (item == NULL)
				return SQL_EXPR_CANONICAL_MALFORMED;
			if (!append(b, ",", 1))
				return SQL_EXPR_CANONICAL_NOMEM;
			rc = encode(item, b, depth + 1, cursor_to_relation,
				    cursor_count);
			if (rc != SQL_EXPR_CANONICAL_OK)
				return rc;
		}
		return append(b, ")", 1) ? SQL_EXPR_CANONICAL_OK :
			SQL_EXPR_CANONICAL_NOMEM;
	}
	/* Expr stores a source token, not a stable resolved function identity. */
	const char *op = operator_name(expr->op);
	if (op == NULL)
		return SQL_EXPR_CANONICAL_UNSUPPORTED;
	bool unary = expr->op == TK_NOT || expr->op == TK_ISNULL ||
		expr->op == TK_NOTNULL || expr->op == TK_UMINUS ||
		expr->op == TK_UPLUS;
	if (expr->x.pList != NULL || expr->pLeft == NULL ||
	    (unary ? expr->pRight != NULL : expr->pRight == NULL))
		return SQL_EXPR_CANONICAL_MALFORMED;
	/* The parser represents INT64_MIN as unary minus applied to the
	 * otherwise out-of-range positive token 9223372036854775808. Canonicalize
	 * that one valid signed value directly instead of rejecting its operand as
	 * an unsupported positive integer literal.
	 */
	if (expr->op == TK_UMINUS && expr->pLeft->op == TK_INTEGER &&
	    expr->pLeft->pLeft == NULL && expr->pLeft->pRight == NULL &&
	    (expr->pLeft->flags & EP_Resolved) != 0 &&
	    (expr->pLeft->flags & (EP_Reduced | EP_TokenOnly)) == 0 &&
	    (expr->pLeft->flags & ~(EP_Resolved | EP_IntValue | EP_Leaf)) == 0 &&
	    (expr->pLeft->flags & EP_IntValue) == 0 &&
	    expr->pLeft->u.zToken != NULL &&
	    strcmp(expr->pLeft->u.zToken, "9223372036854775808") == 0) {
		static const char value[] = "int(-9223372036854775808)";
		return append(b, value, sizeof(value) - 1) ?
			SQL_EXPR_CANONICAL_OK : SQL_EXPR_CANONICAL_NOMEM;
	}
	if (!append(b, op, strlen(op)) || !append(b, "(", 1))
		return SQL_EXPR_CANONICAL_NOMEM;
	enum sql_expr_canonical_reject rc = encode(expr->pLeft, b, depth + 1,
						    cursor_to_relation,
						    cursor_count);
	if (rc != SQL_EXPR_CANONICAL_OK)
		return rc;
	if (!unary) {
		if (!append(b, ",", 1))
			return SQL_EXPR_CANONICAL_NOMEM;
		rc = encode(expr->pRight, b, depth + 1, cursor_to_relation,
			     cursor_count);
		if (rc != SQL_EXPR_CANONICAL_OK)
			return rc;
	}
	return append(b, ")", 1) ? SQL_EXPR_CANONICAL_OK :
		SQL_EXPR_CANONICAL_NOMEM;
}

char *
sql_expr_canonicalize(const struct Expr *expr,
		      const uint32_t *cursor_to_relation,
		      size_t cursor_count,
		      enum sql_expr_canonical_reject *reason)
{
	if (reason != NULL)
		*reason = SQL_EXPR_CANONICAL_OK;
	struct buffer b = {0};
	enum sql_expr_canonical_reject rc = encode(expr, &b, 0,
						    cursor_to_relation,
						    cursor_count);
	if (rc != SQL_EXPR_CANONICAL_OK) {
		free(b.data);
		if (reason != NULL)
			*reason = rc;
		return NULL;
	}
	return b.data;
}
