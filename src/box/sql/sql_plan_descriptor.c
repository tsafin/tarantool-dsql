#include "sql_plan_descriptor.h"

#include <limits.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct sql_plan_descriptor {
	struct sql_plan_descriptor_input value;
	char *space_name;
	struct sql_plan_bound *bounds;
	struct sql_plan_point_key_part *point_key_parts;
	struct sql_plan_point_key_part *prefix_key_parts;
	uint32_t *access_columns, *projection_columns;
	struct sql_plan_order_term *order;
	struct sql_plan_filter *filters;
	struct sql_plan_finalize *finalize;
	struct sql_plan_expression *expressions;
};

const char *
sql_plan_fallback_reason_name(uint32_t reason)
{
	switch (reason) {
	case SQL_PLAN_FALLBACK_UNRESOLVED_INPUT:
		return "UNRESOLVED_INPUT";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_RELATION_COUNT:
		return "UNSUPPORTED_RELATION_COUNT";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_SUBQUERY:
		return "UNSUPPORTED_SUBQUERY";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_AGGREGATE:
		return "UNSUPPORTED_AGGREGATE";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_COMPOUND:
		return "UNSUPPORTED_COMPOUND";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_CTE:
		return "UNSUPPORTED_CTE";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_DISTINCT:
		return "UNSUPPORTED_DISTINCT";
	case SQL_PLAN_FALLBACK_INVALID_LOGICAL_PLAN:
		return "INVALID_LOGICAL_PLAN";
	case SQL_PLAN_FALLBACK_NO_ACCESS_PATH:
		return "NO_ACCESS_PATH";
	case SQL_PLAN_FALLBACK_INVALID_CANDIDATE:
		return "INVALID_CANDIDATE";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_NONDETERMINISTIC:
		return "UNSUPPORTED_NONDETERMINISTIC";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_ACCESS_HINT:
		return "UNSUPPORTED_ACCESS_HINT";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_FUNCTION:
		return "UNSUPPORTED_FUNCTION";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_COLLATION:
		return "UNSUPPORTED_COLLATION";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_EXPRESSION:
		return "UNSUPPORTED_EXPRESSION";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_FILTER:
		return "UNSUPPORTED_FILTER";
	case SQL_PLAN_FALLBACK_UNSUPPORTED_DESTINATION:
		return "UNSUPPORTED_DESTINATION";
	default:
		return NULL;
	}
}

static bool
valid_array(const void *p, size_t n, size_t item)
{
	return (n == 0 || p != NULL) && n <= SIZE_MAX / item;
}

static bool
has_expr(const struct sql_plan_descriptor_input *in, uint32_t id)
{
	for (size_t i = 0; i < in->expression_count; ++i)
		if (in->expressions[i].id == id)
			return true;
	return false;
}

static bool
valid_terms(const struct sql_plan_order_term *terms, size_t n)
{
	if (!valid_array(terms, n, sizeof(*terms))) return false;
	for (size_t i = 0; i < n; ++i)
		if (terms[i].direction < SQL_PLAN_ASC ||
		    terms[i].direction > SQL_PLAN_DESC ||
		    (terms[i].nulls_first != 0 && terms[i].nulls_first != 1))
			return false;
	return true;
}

static void
free_descriptor(struct sql_plan_descriptor *d)
{
	if (d == NULL) return;
	for (size_t i = 0; d->finalize != NULL &&
	     i < d->value.finalize_count; ++i)
		free((void *)d->finalize[i].keys);
	for (size_t i = 0; d->expressions != NULL &&
	     i < d->value.expression_count; ++i)
		free((void *)d->expressions[i].canonical);
	free(d->expressions); free(d->finalize); free(d->filters);
	free(d->order); free(d->projection_columns); free(d->access_columns);
	free(d->prefix_key_parts); free(d->point_key_parts); free(d->bounds);
	free(d->space_name); free(d);
}

struct sql_plan_descriptor *
sql_plan_descriptor_new(const struct sql_plan_descriptor_input *in)
{
	if (in == NULL || in->descriptor_version != 1 ||
	    in->planner_version == 0 ||
	    in->path_class < SQL_PLAN_CURRENT_WHERE_C ||
	    in->path_class > SQL_PLAN_FALLBACK ||
	    (in->path_class == SQL_PLAN_FALLBACK) != (in->fallback_reason != 0) ||
	    (in->fallback_reason != SQL_PLAN_FALLBACK_NONE &&
	     sql_plan_fallback_reason_name(in->fallback_reason) == NULL) ||
	    in->access.kind < SQL_PLAN_PK_POINT_LOOKUP ||
	    in->access.kind > SQL_PLAN_PK_PREFIX_SCAN ||
	    in->access.direction < SQL_PLAN_ASC ||
	    in->access.direction > SQL_PLAN_DESC ||
	    !valid_array(in->access.bounds, in->access.bound_count,
			 sizeof(*in->access.bounds)) ||
	    !valid_array(in->access.point_key_parts,
			 in->access.point_key_part_count,
			 sizeof(*in->access.point_key_parts)) ||
	    in->access.point_key_part_count > SQL_PLAN_POINT_KEY_PART_MAX ||
	    !valid_array(in->access.prefix_key_parts,
			 in->access.prefix_key_part_count,
			 sizeof(*in->access.prefix_key_parts)) ||
	    in->access.prefix_key_part_count > SQL_PLAN_POINT_KEY_PART_MAX ||
	    !valid_array(in->access.projected_columns,
			 in->access.projected_column_count, sizeof(uint32_t)) ||
	    !valid_terms(in->access.produced_order,
			 in->access.produced_order_count) ||
	    !valid_array(in->filters, in->filter_count, sizeof(*in->filters)) ||
	    !valid_array(in->projection_columns, in->projection_column_count,
			 sizeof(uint32_t)) ||
	    !valid_array(in->finalize, in->finalize_count, sizeof(*in->finalize)) ||
	    !valid_array(in->expressions, in->expression_count,
			 sizeof(*in->expressions)) || !isfinite(in->access.est_rows) ||
	    in->access.est_rows < 0 || !isfinite(in->access.est_rows_confidence) ||
	    in->access.est_rows_confidence < 0 || in->access.est_rows_confidence > 1 ||
	    !isfinite(in->cost_startup) || in->cost_startup < 0 ||
	    !isfinite(in->cost_total) || in->cost_total < in->cost_startup ||
	    !isfinite(in->cost_rows) || in->cost_rows < 0 ||
	    !isfinite(in->cost_row_width) || in->cost_row_width < 0 ||
	    !isfinite(in->cost_confidence) || in->cost_confidence < 0 ||
	    in->cost_confidence > 1 ||
	    (in->space_name == NULL && in->space_id != 0))
		return NULL;
	for (size_t i = 0; i < in->access.bound_count; ++i) {
		const struct sql_plan_bound *b = &in->access.bounds[i];
		if (b->side < SQL_PLAN_LOWER || b->side > SQL_PLAN_UPPER ||
		    b->op < SQL_PLAN_EQ || b->op > SQL_PLAN_LE ||
		    !has_expr(in, b->expr_ref)) return NULL;
	}
	if (in->access.kind == SQL_PLAN_PK_POINT_LOOKUP) {
		bool has_composite_key = in->access.point_key_part_count != 0;
		if ((has_composite_key &&
		     (in->access.has_integer_point_key ||
		      in->access.has_unsigned_point_key ||
		      in->access.bound_count !=
			in->access.point_key_part_count)) ||
		    (!has_composite_key &&
		     (in->access.has_integer_point_key ==
		      in->access.has_unsigned_point_key ||
		      in->access.bound_count != 1)))
			return NULL;
		for (size_t i = 0; i < in->access.bound_count; ++i)
			if (in->access.bounds[i].op != SQL_PLAN_EQ)
				return NULL;
	} else if (in->access.kind == SQL_PLAN_INDEX_POINT_LOOKUP &&
		   (in->access.bound_count != 1 ||
		    in->access.bounds[0].op != SQL_PLAN_EQ ||
		    in->access.point_key_part_count != 0)) {
		return NULL;
	}
	if (in->access.kind != SQL_PLAN_PK_POINT_LOOKUP &&
	    in->access.point_key_part_count != 0)
		return NULL;
	if (in->access.kind == SQL_PLAN_PK_PREFIX_SCAN) {
		bool has_prefix_range = in->access.has_integer_range_key ||
			in->access.has_unsigned_range_key;
		bool has_prefix_range_end =
			in->access.has_integer_range_end_key ||
			in->access.has_unsigned_range_end_key;
		size_t expected_bounds = in->access.prefix_key_part_count +
			(has_prefix_range ? (has_prefix_range_end ? 2 : 1) : 0);
		if (in->access.prefix_key_part_count == 0 ||
		    in->access.has_integer_point_key ||
		    in->access.has_unsigned_point_key ||
		    ((in->access.has_integer_range_key ==
		      in->access.has_unsigned_range_key) && has_prefix_range) ||
		    in->access.point_key_part_count != 0 ||
		    in->access.direction != SQL_PLAN_ASC ||
		    in->access.bound_count != expected_bounds ||
		    (has_prefix_range && in->access.range_key_column > INT_MAX))
			return NULL;
		for (size_t i = 0; i < in->access.prefix_key_part_count; ++i)
			if (in->access.bounds[i].op != SQL_PLAN_EQ ||
			    in->access.bounds[i].side != SQL_PLAN_LOWER)
				return NULL;
		if (has_prefix_range) {
			size_t range_bound = in->access.prefix_key_part_count;
			if (has_prefix_range_end) {
				if ((in->access.has_integer_range_key !=
				     in->access.has_integer_range_end_key) ||
				    (in->access.has_unsigned_range_key !=
				     in->access.has_unsigned_range_end_key) ||
				    (in->access.integer_range_op != SQL_PLAN_GT &&
				     in->access.integer_range_op != SQL_PLAN_GE) ||
				    (in->access.integer_range_end_op != SQL_PLAN_LT &&
				     in->access.integer_range_end_op != SQL_PLAN_LE) ||
				    in->access.bounds[range_bound].side != SQL_PLAN_LOWER ||
				    in->access.bounds[range_bound].op !=
					in->access.integer_range_op ||
				    in->access.bounds[range_bound + 1].side != SQL_PLAN_UPPER ||
				    in->access.bounds[range_bound + 1].op !=
					in->access.integer_range_end_op)
					return NULL;
			} else {
				enum sql_plan_bound_op op =
					in->access.integer_range_op;
				if ((op != SQL_PLAN_GT && op != SQL_PLAN_GE &&
				     op != SQL_PLAN_LT && op != SQL_PLAN_LE) ||
				    in->access.bounds[range_bound].op != op ||
				    in->access.bounds[range_bound].side !=
					((op == SQL_PLAN_GT || op == SQL_PLAN_GE) ?
					 SQL_PLAN_LOWER : SQL_PLAN_UPPER))
					return NULL;
			}
		} else if (in->access.has_integer_range_end_key ||
			   in->access.has_unsigned_range_end_key) {
			return NULL;
		}
	} else if (in->access.prefix_key_part_count != 0) {
		return NULL;
	}
	if ((in->access.kind == SQL_PLAN_INDEX_FULL_SCAN ||
	     in->access.kind == SQL_PLAN_TABLE_FULL_SCAN) &&
	    in->access.bound_count != 0)
		return NULL;
	if (in->access.kind == SQL_PLAN_INDEX_RANGE_SCAN &&
	    (in->access.bound_count == 0 || in->access.bound_count > 2))
		return NULL;
	bool has_range_end = in->access.has_integer_range_end_key ||
		in->access.has_unsigned_range_end_key;
	if (has_range_end) {
		size_t prefix_bounds = in->access.kind == SQL_PLAN_PK_PREFIX_SCAN ?
			in->access.prefix_key_part_count : 0;
		if ((in->access.kind != SQL_PLAN_INDEX_RANGE_SCAN &&
		     in->access.kind != SQL_PLAN_PK_PREFIX_SCAN) ||
		    in->access.has_integer_range_key ==
		    in->access.has_unsigned_range_key ||
		    in->access.has_integer_range_end_key ==
		    in->access.has_unsigned_range_end_key ||
		    in->access.has_integer_range_key !=
		    in->access.has_integer_range_end_key ||
		    in->access.bound_count != prefix_bounds + 2 ||
		    (in->access.integer_range_op != SQL_PLAN_GT &&
		     in->access.integer_range_op != SQL_PLAN_GE) ||
		    (in->access.integer_range_end_op != SQL_PLAN_LT &&
		     in->access.integer_range_end_op != SQL_PLAN_LE))
			return NULL;
		bool has_lower = false;
		bool has_upper = false;
		for (size_t i = prefix_bounds; i < in->access.bound_count; ++i) {
			const struct sql_plan_bound *bound = &in->access.bounds[i];
			if (bound->side == SQL_PLAN_LOWER &&
			    (bound->op == SQL_PLAN_GT || bound->op == SQL_PLAN_GE) &&
			    bound->op == in->access.integer_range_op)
				has_lower = true;
			else if (bound->side == SQL_PLAN_UPPER &&
				 (bound->op == SQL_PLAN_LT || bound->op == SQL_PLAN_LE) &&
				 bound->op == in->access.integer_range_end_op)
				has_upper = true;
			else
				return NULL;
		}
		if (!has_lower || !has_upper)
			return NULL;
	}
	for (size_t i = 0; i < in->filter_count; ++i)
		if (!has_expr(in, in->filters[i].expr_ref) ||
		    !isfinite(in->filters[i].selectivity) ||
		    in->filters[i].selectivity < 0 || in->filters[i].selectivity > 1 ||
		    !isfinite(in->filters[i].confidence) ||
		    in->filters[i].confidence < 0 || in->filters[i].confidence > 1 ||
		    in->filters[i].op < SQL_PLAN_FILTER_EXPRESSION ||
		    in->filters[i].op > SQL_PLAN_FILTER_IS_NOT_NULL ||
		    (in->filters[i].op != SQL_PLAN_FILTER_EXPRESSION &&
		     in->filters[i].column > INT_MAX))
			return NULL;
	for (size_t i = 0; i < in->expression_count; ++i) {
		if (in->expressions[i].canonical == NULL) return NULL;
		for (size_t j = 0; j < i; ++j)
			if (in->expressions[i].id == in->expressions[j].id) return NULL;
	}
	for (size_t i = 0; i < in->finalize_count; ++i) {
		const struct sql_plan_finalize *f = &in->finalize[i];
		if (f->kind < SQL_PLAN_SORT || f->kind > SQL_PLAN_LIMIT ||
		    !valid_terms(f->keys, f->key_count) ||
		    (f->kind == SQL_PLAN_SORT && f->key_count == 0) ||
		    (f->kind == SQL_PLAN_LIMIT && f->key_count != 0)) return NULL;
	}
	struct sql_plan_descriptor *d = calloc(1, sizeof(*d));
	if (d == NULL) return NULL;
	d->value = *in;
#define COPY_FIELD(dst, src, n) do { \
	if ((n) != 0) { \
		dst = malloc((n) * sizeof(*(dst))); \
		if (dst == NULL) goto error; \
		memcpy(dst, src, (n) * sizeof(*(dst))); \
	} \
} while (0)
	if (in->space_name != NULL) {
	d->space_name = strdup(in->space_name); if (d->space_name == NULL) goto error;
	d->value.space_name = d->space_name;
	}
	COPY_FIELD(d->bounds, in->access.bounds, in->access.bound_count);
	d->value.access.bounds = d->bounds;
	COPY_FIELD(d->point_key_parts, in->access.point_key_parts,
		   in->access.point_key_part_count);
	d->value.access.point_key_parts = d->point_key_parts;
	COPY_FIELD(d->prefix_key_parts, in->access.prefix_key_parts,
		   in->access.prefix_key_part_count);
	d->value.access.prefix_key_parts = d->prefix_key_parts;
	COPY_FIELD(d->access_columns, in->access.projected_columns,
		   in->access.projected_column_count);
	d->value.access.projected_columns = d->access_columns;
	COPY_FIELD(d->order, in->access.produced_order,
		   in->access.produced_order_count);
	d->value.access.produced_order = d->order;
	COPY_FIELD(d->filters, in->filters, in->filter_count);
	d->value.filters = d->filters;
	COPY_FIELD(d->projection_columns, in->projection_columns,
		   in->projection_column_count);
	d->value.projection_columns = d->projection_columns;
	if (in->finalize_count != 0) {
		d->finalize = calloc(in->finalize_count, sizeof(*d->finalize));
		if (d->finalize == NULL) goto error;
		for (size_t i = 0; i < in->finalize_count; ++i) {
			d->finalize[i] = in->finalize[i];
			d->finalize[i].keys = NULL;
			if (in->finalize[i].key_count != 0) {
				struct sql_plan_order_term *keys = malloc(
					in->finalize[i].key_count * sizeof(*keys));
				if (keys == NULL) goto error;
				memcpy(keys, in->finalize[i].keys,
				       in->finalize[i].key_count * sizeof(*keys));
				d->finalize[i].keys = keys;
			}
		}
	}
	d->value.finalize = d->finalize;
	if (in->expression_count != 0) {
		d->expressions = calloc(in->expression_count, sizeof(*d->expressions));
		if (d->expressions == NULL) goto error;
		for (size_t i = 0; i < in->expression_count; ++i) {
			d->expressions[i].id = in->expressions[i].id;
			d->expressions[i].canonical = strdup(in->expressions[i].canonical);
			if (d->expressions[i].canonical == NULL) goto error;
		}
	}
	d->value.expressions = d->expressions;
#undef COPY_FIELD
	return d;
error:
#undef COPY_FIELD
	free_descriptor(d);
	return NULL;
}

void sql_plan_descriptor_delete(struct sql_plan_descriptor *d) { free_descriptor(d); }
uint32_t sql_plan_descriptor_version(const struct sql_plan_descriptor *d)
{ return d == NULL ? 0 : d->value.descriptor_version; }
const struct sql_plan_descriptor_input *
sql_plan_descriptor_get_input(const struct sql_plan_descriptor *d)
{ return d == NULL ? NULL : &d->value; }
uint32_t sql_plan_descriptor_space_id(const struct sql_plan_descriptor *d)
{ return d == NULL ? 0 : d->value.space_id; }
const char *sql_plan_descriptor_space_name(const struct sql_plan_descriptor *d)
{ return d == NULL ? NULL : d->value.space_name; }
enum sql_plan_access_kind sql_plan_descriptor_access_kind(const struct sql_plan_descriptor *d)
{ return d == NULL ? SQL_PLAN_TABLE_FULL_SCAN : d->value.access.kind; }
size_t sql_plan_descriptor_filter_count(const struct sql_plan_descriptor *d)
{ return d == NULL ? 0 : d->value.filter_count; }
size_t sql_plan_descriptor_expression_count(const struct sql_plan_descriptor *d)
{ return d == NULL ? 0 : d->value.expression_count; }
