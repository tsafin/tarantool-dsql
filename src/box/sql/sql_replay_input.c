#include "sql_replay_input.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msgpuck.h"

struct replay_writer {
	char *data;
	size_t size;
	size_t capacity;
	bool invalid_size;
};

static bool
writer_reserve(struct replay_writer *w, size_t add)
{
	if (add > SIZE_MAX - w->size)
		return false;
	size_t needed = w->size + add;
	if (needed <= w->capacity)
		return true;
	size_t capacity = w->capacity == 0 ? 128 : w->capacity;
	while (capacity < needed) {
		if (capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	char *data = realloc(w->data, capacity);
	if (data == NULL)
		return false;
	w->data = data;
	w->capacity = capacity;
	return true;
}

static bool
put_encoded(struct replay_writer *w, size_t size,
	    char *(*encode)(char *, uint64_t), uint64_t value)
{
	if (!writer_reserve(w, size))
		return false;
	char *end = encode(w->data + w->size, value);
	w->size = end - w->data;
	return true;
}

static bool
put_uint(struct replay_writer *w, uint64_t value)
{
	return put_encoded(w, mp_sizeof_uint(value), mp_encode_uint, value);
}

static bool
put_int(struct replay_writer *w, int64_t value)
{
	if (!writer_reserve(w, mp_sizeof_int(value)))
		return false;
	w->size = mp_encode_int(w->data + w->size, value) - w->data;
	return true;
}

static bool
put_bool(struct replay_writer *w, bool value)
{
	if (!writer_reserve(w, mp_sizeof_bool(value)))
		return false;
	w->size = mp_encode_bool(w->data + w->size, value) - w->data;
	return true;
}

static bool
put_double(struct replay_writer *w, double value)
{
	if (!isfinite(value) || !writer_reserve(w, mp_sizeof_double(value)))
		return false;
	/* Canonicalize negative zero, whose IEEE bit pattern differs from +0. */
	if (value == 0)
		value = 0.0;
	w->size = mp_encode_double(w->data + w->size, value) - w->data;
	return true;
}

static bool
put_nil(struct replay_writer *w)
{
	if (!writer_reserve(w, mp_sizeof_nil()))
		return false;
	w->size = mp_encode_nil(w->data + w->size) - w->data;
	return true;
}

static bool
put_array(struct replay_writer *w, size_t count)
{
	if (count > UINT32_MAX || !writer_reserve(w, mp_sizeof_array(count))) {
		w->invalid_size |= count > UINT32_MAX;
		return false;
	}
	w->size = mp_encode_array(w->data + w->size, count) - w->data;
	return true;
}

static bool
put_map(struct replay_writer *w, size_t count)
{
	if (count > UINT32_MAX || !writer_reserve(w, mp_sizeof_map(count))) {
		w->invalid_size |= count > UINT32_MAX;
		return false;
	}
	w->size = mp_encode_map(w->data + w->size, count) - w->data;
	return true;
}

static bool
put_string(struct replay_writer *w, const char *value)
{
	if (value == NULL)
		return false;
	size_t len = strlen(value);
	if (len > UINT32_MAX) {
		w->invalid_size = true;
		return false;
	}
	if (!writer_reserve(w, mp_sizeof_str(len)))
		return false;
	w->size = mp_encode_str(w->data + w->size, value, len) - w->data;
	return true;
}

#define PUT(expr)                                                              \
	do {                                                                   \
		if (!(expr))                                                   \
			goto fail;                                             \
	} while (0)

struct expression_parser {
	const char *p;
	size_t column_count;
};

static bool
parse_canonical_expression(struct expression_parser *parser,
			   unsigned int depth);

static bool
parse_decimal_ordinal(struct expression_parser *parser, uint32_t *value)
{
	if (*parser->p < '0' || *parser->p > '9')
		return false;
	if (*parser->p == '0' && parser->p[1] >= '0' && parser->p[1] <= '9')
		return false;
	uint64_t number = 0;
	do {
		unsigned int digit = (unsigned int)(*parser->p - '0');
		if (number > (UINT32_MAX - digit) / 10)
			return false;
		number = number * 10 + digit;
		parser->p++;
	} while (*parser->p >= '0' && *parser->p <= '9');
	*value = (uint32_t)number;
	return true;
}

static bool
parse_literal(struct expression_parser *parser, const char *prefix,
	      bool floating)
{
	size_t prefix_len = strlen(prefix);
	if (strncmp(parser->p, prefix, prefix_len) != 0)
		return false;
	parser->p += prefix_len;
	const char *start = parser->p;
	while (*parser->p != '\0' && *parser->p != ')')
		parser->p++;
	size_t len = parser->p - start;
	if (len == 0 || *parser->p != ')')
		return false;
	char token[128];
	if (len >= sizeof(token))
		return false;
	memcpy(token, start, len);
	token[len] = '\0';
	char canonical[160];
	if (floating) {
		errno = 0;
		char *end;
		double value = strtod(token, &end);
		if (*end != '\0' || errno == ERANGE || !isfinite(value))
			return false;
		int n = snprintf(canonical, sizeof(canonical), "float(%a)",
				 value);
		if (n < 0 || (size_t)n >= sizeof(canonical) ||
		    (size_t)n != len + strlen("float()") ||
		    memcmp(canonical, start - strlen("float("), (size_t)n) != 0)
			return false;
	} else {
		errno = 0;
		char *end;
		long long value = strtoll(token, &end, 10);
		if (*end != '\0' || errno == ERANGE)
			return false;
		int n = snprintf(canonical, sizeof(canonical), "int(%lld)",
				 value);
		if (n < 0 || (size_t)n >= sizeof(canonical) ||
		    (size_t)n != len + strlen("int()") ||
		    memcmp(canonical, start - strlen("int("), (size_t)n) != 0)
			return false;
	}
	parser->p++;
	return true;
}

static bool
parse_operator(struct expression_parser *parser, unsigned int depth)
{
	static const struct {
		const char *name;
		bool unary;
	} operators[] = {
		{ "and", false },	{ "bitand", false },
		{ "bitor", false },      { "concat", false },
		{ "divide", false },     { "eq", false },
		{ "ge", false },	 { "gt", false },
		{ "is", false },	 { "isnull", true },
		{ "le", false },	 { "lshift", false },
		{ "lt", false },	 { "minus", false },
		{ "multiply", false },   { "ne", false },
		{ "not", true },	 { "notnull", true },
		{ "or", false },	 { "plus", false },
		{ "remainder", false },  { "rshift", false },
		{ "unary_minus", true }, { "unary_plus", true },
	};
	const char *start = parser->p;
	while ((*parser->p >= 'a' && *parser->p <= 'z') || *parser->p == '_')
		parser->p++;
	size_t len = parser->p - start;
	if (len == 0 || *parser->p++ != '(')
		return false;
	bool unary = false;
	bool found = false;
	for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); i++) {
		if (strlen(operators[i].name) == len &&
		    memcmp(start, operators[i].name, len) == 0) {
			unary = operators[i].unary;
			found = true;
			break;
		}
	}
	if (!found || !parse_canonical_expression(parser, depth + 1))
		return false;
	if (unary)
		return *parser->p++ == ')';
	if (*parser->p++ != ',' ||
	    !parse_canonical_expression(parser, depth + 1))
		return false;
	return *parser->p++ == ')';
}

static bool
parse_canonical_expression(struct expression_parser *parser, unsigned int depth)
{
	if (depth > 256)
		return false;
	if (strncmp(parser->p, "null", 4) == 0) {
		parser->p += 4;
		return true;
	}
	if (strncmp(parser->p, "str(", 4) == 0) {
		parser->p += 4;
		const char *hex_start = parser->p;
		while ((*parser->p >= '0' && *parser->p <= '9') ||
		       (*parser->p >= 'a' && *parser->p <= 'f'))
			parser->p++;
		if (((size_t)(parser->p - hex_start) & 1) != 0)
			return false;
		return *parser->p++ == ')';
	}
	if (strncmp(parser->p, "int(", 4) == 0)
		return parse_literal(parser, "int(", false);
	if (strncmp(parser->p, "float(", 6) == 0)
		return parse_literal(parser, "float(", true);
	if (strncmp(parser->p, "col(r", 5) == 0) {
		parser->p += 5;
		uint32_t relation, column;
		if (!parse_decimal_ordinal(parser, &relation) ||
		    relation != 0 || *parser->p++ != ',' ||
		    *parser->p++ != 'c' ||
		    !parse_decimal_ordinal(parser, &column) ||
		    column >= parser->column_count || *parser->p++ != ')')
			return false;
		return true;
	}
	return parse_operator(parser, depth);
}

static bool
canonical_expression_valid(const char *expression, size_t column_count)
{
	if (expression == NULL || expression[0] == '\0')
		return false;
	struct expression_parser parser = {
		.p = expression,
		.column_count = column_count,
	};
	return parse_canonical_expression(&parser, 0) && *parser.p == '\0';
}

static int
compare_replay_index_ptr(const void *lhs, const void *rhs)
{
	const struct sql_replay_index *a =
		*(const struct sql_replay_index *const *)lhs;
	const struct sql_replay_index *b =
		*(const struct sql_replay_index *const *)rhs;
	return strcmp(a->logical_key, b->logical_key);
}

static bool
put_relation_statistics(struct replay_writer *w,
			const struct sql_replay_input *in)
{
	if (!in->statistics_present)
		return put_nil(w);
	/* Keys are encoded in lexical order for canonical map representation. */
	return put_map(w, 10) && put_string(w, "average_row_width") &&
	       put_double(w, in->average_row_width) &&
	       put_string(w, "cardinality_semantics") &&
	       put_uint(w, in->cardinality_semantics) &&
	       put_string(w, "collected_at") && put_uint(w, in->collected_at) &&
	       put_string(w, "confidence_ppm") &&
	       put_uint(w, in->confidence_ppm) &&
	       put_string(w, "confidence_source") &&
	       put_string(w, in->confidence_source) &&
	       put_string(w, "modification_epoch") &&
	       put_uint(w, in->modification_epoch) &&
	       put_string(w, "population_basis") &&
	       put_string(w, in->population_basis) &&
	       put_string(w, "row_count") && put_uint(w, in->row_count) &&
	       put_string(w, "width_basis") && put_string(w, in->width_basis) &&
	       put_string(w, "width_denominator_count") &&
	       put_uint(w, in->width_denominator_count);
}

static bool
put_index(struct replay_writer *w, const struct sql_replay_index *idx)
{
	if (!put_map(w, 4) || !put_string(w, "canonical_definition") ||
	    !put_string(w, idx->canonical_definition) ||
	    !put_string(w, "logical_key") || !put_string(w, idx->logical_key) ||
	    !put_string(w, "part_columns") || !put_array(w, idx->part_count))
		return false;
	for (size_t i = 0; i < idx->part_count; i++)
		if (!put_uint(w, idx->part_columns[i]))
			return false;
	if (!put_string(w, "statistics"))
		return false;
	if (!idx->statistics_present)
		return put_nil(w);
	if (!put_map(w, 6) || !put_string(w, "definition_version") ||
	    !put_uint(w, idx->definition_version) ||
	    !put_string(w, "distinct_prefixes") ||
	    !put_array(w, idx->prefix_count))
		return false;
	for (size_t i = 0; i < idx->prefix_count; i++)
		if (!put_uint(w, idx->distinct_prefixes[i]))
			return false;
	return put_string(w, "ndv_basis") && put_string(w, idx->ndv_basis) &&
	       put_string(w, "population_basis") &&
	       put_string(w, idx->population_basis) &&
	       put_string(w, "tuple_count") && put_uint(w, idx->tuple_count) &&
	       put_string(w, "tuple_count_semantics") &&
	       put_uint(w, idx->tuple_count_semantics);
}

static bool
put_relation(struct replay_writer *w, const struct sql_replay_input *in,
	     const struct sql_replay_index **indexes)
{
	if (!put_map(w, 5) || !put_string(w, "columns") ||
	    !put_array(w, in->column_count))
		return false;
	for (size_t i = 0; i < in->column_count; i++) {
		if (!put_map(w, 2) || !put_string(w, "collation") ||
		    !put_string(w, in->columns[i].collation) ||
		    !put_string(w, "type") ||
		    !put_string(w, in->columns[i].type))
			return false;
	}
	if (!put_string(w, "definition") ||
	    !put_string(w, in->relation_definition) ||
	    !put_string(w, "indexes") || !put_array(w, in->index_count))
		return false;
	for (size_t i = 0; i < in->index_count; i++)
		if (!put_index(w, indexes[i]))
			return false;
	return put_string(w, "key") && put_string(w, in->relation_key) &&
	       put_string(w, "statistics") && put_relation_statistics(w, in);
}

enum sql_replay_input_status
sql_replay_input_check_replay_ready(const struct sql_replay_input *input)
{
	if (input == NULL || input->planner_algorithm_version == 0 ||
	    input->planner_config_version == 0 || input->beam_width == 0 ||
	    (input->final_path_count != 0 && input->final_paths == NULL) ||
	    (!input->final_paths_present &&
	     (input->final_path_count != 0 || input->selector_version != 0)) ||
	    (input->final_paths_present && input->selector_version !=
					      SQL_REPLAY_SELECTOR_FINAL_PATH_V1))
		return SQL_REPLAY_INPUT_INVALID;
	if (!input->final_paths_present)
		return SQL_REPLAY_INPUT_INCOMPLETE;
	return SQL_REPLAY_INPUT_OK;
}

enum sql_replay_input_status
sql_replay_input_select_final_path(const struct sql_replay_input *input,
				   const struct sql_replay_final_path **selected)
{
	if (selected == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*selected = NULL;
	if (input == NULL || input->selector_version !=
				     SQL_REPLAY_SELECTOR_FINAL_PATH_V1 ||
	    (input->final_path_count != 0 && input->final_paths == NULL) ||
	    (!input->final_paths_present && input->final_path_count != 0))
		return SQL_REPLAY_INPUT_INVALID;
	if (!input->final_paths_present)
		return SQL_REPLAY_INPUT_INCOMPLETE;
	if (input->final_path_count == 0)
		return SQL_REPLAY_INPUT_NO_PLAN;
	const struct sql_replay_final_path *best = &input->final_paths[0];
	for (size_t i = 1; i < input->final_path_count; i++) {
		const struct sql_replay_final_path *candidate =
			&input->final_paths[i];
		/* wherePathSolver's final scan uses strict >, so ties retain the
		 * first path in the final beam array. */
		if (candidate->path_cost_logest < best->path_cost_logest)
			best = candidate;
	}
	*selected = best;
	return SQL_REPLAY_INPUT_OK;
}

enum sql_replay_input_status
sql_replay_input_serialize(const struct sql_replay_input *in, char **data,
			   size_t *size)
{
	if (data == NULL || size == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*data = NULL;
	*size = 0;
	if (in == NULL || in->relation_key == NULL ||
	    in->relation_key[0] == '\0' || in->relation_definition == NULL ||
	    in->relation_definition[0] == '\0' || in->predicate == NULL ||
	    in->predicate[0] == '\0' || in->planner_algorithm_version == 0 ||
	    in->planner_config_version == 0 || in->beam_width == 0 ||
	    (!in->limit_present && in->limit != 0) ||
	    (!in->offset_present && in->offset != 0) ||
	    (in->offset_present && !in->limit_present) ||
	    in->column_count == 0 || in->columns == NULL ||
	    (in->index_count != 0 && in->indexes == NULL) ||
	    in->projection_count == 0 || in->projections == NULL ||
	    (in->order_by_count != 0 && in->order_by == NULL) ||
	    in->index_count > UINT32_MAX || in->column_count > UINT32_MAX ||
	    in->projection_count > UINT32_MAX ||
	    in->order_by_count > UINT32_MAX ||
	    in->access_candidate_count > UINT32_MAX ||
	    (in->access_candidate_count != 0 &&
	     in->access_candidates == NULL) ||
	    (!in->access_candidates_present && in->access_candidate_count != 0))
		return SQL_REPLAY_INPUT_INVALID;
	if ((in->final_path_count != 0 && in->final_paths == NULL) ||
	    (!in->final_paths_present &&
	     (in->final_path_count != 0 || in->selector_version != 0)) ||
	    (in->final_paths_present && in->selector_version !=
					      SQL_REPLAY_SELECTOR_FINAL_PATH_V1) ||
	    in->final_path_count > UINT32_MAX)
		return SQL_REPLAY_INPUT_INVALID;
	if (in->statistics_present) {
		if (in->cardinality_semantics <
			    SQL_REPLAY_CARDINALITY_VISIBLE_ROWS ||
		    in->cardinality_semantics >
			    SQL_REPLAY_CARDINALITY_ESTIMATE ||
		    !isfinite(in->average_row_width) ||
		    in->average_row_width < 0 ||
		    in->population_basis == NULL ||
		    in->population_basis[0] == '\0' ||
		    in->width_basis == NULL || in->width_basis[0] == '\0' ||
		    in->width_denominator_count == 0 ||
		    in->confidence_ppm > 1000000 ||
		    in->confidence_source == NULL ||
		    in->confidence_source[0] == '\0')
			return SQL_REPLAY_INPUT_INVALID;
	} else if (in->row_count != 0 || in->cardinality_semantics != 0 ||
		   in->population_basis != NULL || in->average_row_width != 0 ||
		   in->width_basis != NULL ||
		   in->width_denominator_count != 0 ||
		   in->confidence_ppm != 0 || in->confidence_source != NULL ||
		   in->collected_at != 0 || in->modification_epoch != 0) {
		return SQL_REPLAY_INPUT_INVALID;
	}
	for (size_t i = 0; i < in->column_count; i++) {
		if (in->columns[i].type == NULL ||
		    in->columns[i].type[0] == '\0' ||
		    in->columns[i].collation == NULL ||
		    in->columns[i].collation[0] == '\0')
			return SQL_REPLAY_INPUT_INVALID;
	}
	for (size_t i = 0; i < in->index_count; i++) {
		const struct sql_replay_index *idx = &in->indexes[i];
		if (idx->logical_key == NULL || idx->logical_key[0] == '\0' ||
		    idx->canonical_definition == NULL ||
		    idx->canonical_definition[0] == '\0' ||
		    idx->part_count == 0 || idx->part_count > UINT32_MAX ||
		    idx->part_columns == NULL)
			return SQL_REPLAY_INPUT_INVALID;
		for (size_t j = 0; j < idx->part_count; j++)
			if (idx->part_columns[j] >= in->column_count)
				return SQL_REPLAY_INPUT_INVALID;
		for (size_t j = i + 1; j < in->index_count; j++)
			if (in->indexes[j].logical_key == NULL ||
			    strcmp(idx->logical_key,
				   in->indexes[j].logical_key) == 0)
				return SQL_REPLAY_INPUT_INVALID;
		if (idx->statistics_present) {
			if (!in->statistics_present ||
			    idx->definition_version == 0 ||
			    idx->tuple_count_semantics <
				    SQL_REPLAY_CARDINALITY_VISIBLE_ROWS ||
			    idx->tuple_count_semantics >
				    SQL_REPLAY_CARDINALITY_ESTIMATE ||
			    (idx->tuple_count_semantics ==
				     SQL_REPLAY_CARDINALITY_VISIBLE_ROWS &&
			     in->cardinality_semantics ==
				     SQL_REPLAY_CARDINALITY_VISIBLE_ROWS &&
			     idx->tuple_count > in->row_count) ||
			    idx->population_basis == NULL ||
			    idx->population_basis[0] == '\0' ||
			    idx->ndv_basis == NULL ||
			    idx->ndv_basis[0] == '\0' ||
			    idx->prefix_count != idx->part_count ||
			    idx->prefix_count > UINT32_MAX ||
			    idx->distinct_prefixes == NULL)
				return SQL_REPLAY_INPUT_INVALID;
			for (size_t j = 0; j < idx->prefix_count; j++)
				if (idx->distinct_prefixes[j] >
					    idx->tuple_count ||
				    (j != 0 &&
				     idx->distinct_prefixes[j] <
					     idx->distinct_prefixes[j - 1]))
					return SQL_REPLAY_INPUT_INVALID;
		} else if (idx->tuple_count != 0 ||
			   idx->tuple_count_semantics != 0 ||
			   idx->definition_version != 0 ||
			   idx->population_basis != NULL ||
			   idx->ndv_basis != NULL || idx->prefix_count != 0 ||
			   idx->distinct_prefixes != NULL) {
			return SQL_REPLAY_INPUT_INVALID;
		}
	}
	for (size_t i = 0; i < in->projection_count; i++) {
		if (!canonical_expression_valid(in->projections[i],
						in->column_count))
			return SQL_REPLAY_INPUT_INVALID;
	}
	for (size_t i = 0; i < in->order_by_count; i++) {
		if (!canonical_expression_valid(
			    in->order_by[i].canonical_expression,
			    in->column_count))
			return SQL_REPLAY_INPUT_INVALID;
	}
	for (size_t i = 0; i < in->access_candidate_count; i++) {
		const struct sql_replay_access_candidate *c =
			&in->access_candidates[i];
		if (c->logical_key == NULL || c->logical_key[0] == '\0' ||
		    c->kind < SQL_REPLAY_ACCESS_PRIMARY_KEY_POINT ||
		    c->kind > SQL_REPLAY_ACCESS_TABLE_FULL ||
		    (c->kind == SQL_REPLAY_ACCESS_TABLE_FULL &&
		     c->index_key != NULL) ||
		    (c->kind != SQL_REPLAY_ACCESS_TABLE_FULL &&
		     (c->index_key == NULL || c->index_key[0] == '\0')) ||
		    (c->constraint_count != 0 && c->constraints == NULL) ||
		    (c->projected_column_count != 0 &&
		     c->projected_columns == NULL) ||
		    (c->produced_order_count != 0 &&
		     c->produced_order == NULL) ||
		    !isfinite(c->estimated_rows) || c->estimated_rows < 0 ||
		    !isfinite(c->startup_cost) || c->startup_cost < 0 ||
		    !isfinite(c->total_cost) ||
		    c->total_cost < c->startup_cost ||
		    !isfinite(c->estimated_width) || c->estimated_width < 0 ||
		    !isfinite(c->cost_rows) || c->cost_rows < 0 ||
		    c->estimate_confidence_ppm > 1000000 ||
		    c->cost_confidence_ppm > 1000000 ||
		    c->constraint_count > UINT32_MAX ||
		    c->projected_column_count > UINT32_MAX ||
		    c->produced_order_count > UINT32_MAX)
			return SQL_REPLAY_INPUT_INVALID;
		for (size_t j = i + 1; j < in->access_candidate_count; j++)
			if (in->access_candidates[j].logical_key == NULL ||
			    strcmp(c->logical_key,
				   in->access_candidates[j].logical_key) == 0)
				return SQL_REPLAY_INPUT_INVALID;
		if (c->index_key != NULL) {
			bool found = false;
			for (size_t j = 0; j < in->index_count; j++)
				found |=
					strcmp(c->index_key,
					       in->indexes[j].logical_key) == 0;
			if (!found)
				return SQL_REPLAY_INPUT_INVALID;
		}
		for (size_t j = 0; j < c->constraint_count; j++)
			if (c->constraints[j].op <
				    SQL_REPLAY_CONSTRAINT_EQUAL ||
			    c->constraints[j].op >
				    SQL_REPLAY_CONSTRAINT_UPPER ||
			    !canonical_expression_valid(
				    c->constraints[j].canonical_expression,
				    in->column_count))
				return SQL_REPLAY_INPUT_INVALID;
		for (size_t j = 0; j < c->projected_column_count; j++)
			if (c->projected_columns[j] >= in->column_count)
				return SQL_REPLAY_INPUT_INVALID;
		for (size_t j = 0; j < c->produced_order_count; j++)
			if (!canonical_expression_valid(
				    c->produced_order[j].canonical_expression,
				    in->column_count))
				return SQL_REPLAY_INPUT_INVALID;
	}
	for (size_t i = 0; i < in->final_path_count; i++) {
		const struct sql_replay_final_path *path = &in->final_paths[i];
		if (path->plan_fingerprint == NULL ||
		    path->plan_fingerprint[0] == '\0' || path->is_ordered < -1 ||
		    (path->is_ordered >= 0 &&
	     (uint32_t)path->is_ordered > in->order_by_count) ||
		    path->reverse_mask > 1)
			return SQL_REPLAY_INPUT_INVALID;
		for (size_t j = i + 1; j < in->final_path_count; j++)
			if (in->final_paths[j].plan_fingerprint == NULL ||
			    strcmp(path->plan_fingerprint,
				   in->final_paths[j].plan_fingerprint) == 0)
				return SQL_REPLAY_INPUT_INVALID;
	}
	if (!canonical_expression_valid(in->predicate, in->column_count))
		return SQL_REPLAY_INPUT_INVALID;
	if (in->statistics_present &&
	    (in->population_basis == NULL || in->width_basis == NULL ||
	     in->confidence_source == NULL))
		return SQL_REPLAY_INPUT_INVALID;
	const struct sql_replay_index **indexes = NULL;
	if (in->index_count != 0) {
		if (in->index_count > SIZE_MAX / sizeof(*indexes))
			return SQL_REPLAY_INPUT_INVALID;
		indexes = malloc(in->index_count * sizeof(*indexes));
		if (indexes == NULL)
			return SQL_REPLAY_INPUT_NOMEM;
		for (size_t i = 0; i < in->index_count; i++) {
			if (in->indexes[i].logical_key == NULL) {
				free(indexes);
				return SQL_REPLAY_INPUT_INVALID;
			}
			indexes[i] = &in->indexes[i];
		}
		qsort(indexes, in->index_count, sizeof(*indexes),
		      compare_replay_index_ptr);
	}
	struct replay_writer w = { 0 };
	/* Top-level keys are lexicographically ordered; candidate array order
	 * preserves provider rank because planners may consume tie-break order. */
	PUT(put_map(&w, 10));
	PUT(put_string(&w, "access_candidates"));
	if (!in->access_candidates_present) {
		PUT(put_nil(&w));
	} else {
		PUT(put_array(&w, in->access_candidate_count));
		for (size_t i = 0; i < in->access_candidate_count; i++) {
			const struct sql_replay_access_candidate *c =
				&in->access_candidates[i];
			PUT(put_map(&w, 14));
			PUT(put_string(&w, "cost_confidence_ppm"));
			PUT(put_uint(&w, c->cost_confidence_ppm));
			PUT(put_string(&w, "cost_rows"));
			PUT(put_double(&w, c->cost_rows));
			PUT(put_string(&w, "constraints"));
			PUT(put_array(&w, c->constraint_count));
			for (size_t j = 0; j < c->constraint_count; j++) {
				PUT(put_map(&w, 2));
				PUT(put_string(&w, "expression"));
				PUT(put_string(
					&w,
					c->constraints[j].canonical_expression));
				PUT(put_string(&w, "operator"));
				PUT(put_uint(&w, c->constraints[j].op));
			}
			PUT(put_string(&w, "descending"));
			PUT(put_bool(&w, c->descending));
			PUT(put_string(&w, "estimate_confidence_ppm"));
			PUT(put_uint(&w, c->estimate_confidence_ppm));
			PUT(put_string(&w, "estimated_rows"));
			PUT(put_double(&w, c->estimated_rows));
			PUT(put_string(&w, "estimated_width"));
			PUT(put_double(&w, c->estimated_width));
			PUT(put_string(&w, "index_key"));
			PUT(c->index_key == NULL ?
				    put_nil(&w) :
				    put_string(&w, c->index_key));
			PUT(put_string(&w, "key"));
			PUT(put_string(&w, c->logical_key));
			PUT(put_string(&w, "kind"));
			PUT(put_uint(&w, c->kind));
			PUT(put_string(&w, "projected_columns"));
			PUT(put_array(&w, c->projected_column_count));
			for (size_t j = 0; j < c->projected_column_count; j++)
				PUT(put_uint(&w, c->projected_columns[j]));
			PUT(put_string(&w, "produced_order"));
			PUT(put_array(&w, c->produced_order_count));
			for (size_t j = 0; j < c->produced_order_count; j++) {
				PUT(put_map(&w, 3));
				PUT(put_string(&w, "descending"));
				PUT(put_bool(&w,
					     c->produced_order[j].descending));
				PUT(put_string(&w, "expression"));
				PUT(put_string(&w,
					       c->produced_order[j]
						       .canonical_expression));
				PUT(put_string(&w, "nulls_first"));
				PUT(put_bool(&w,
					     c->produced_order[j].nulls_first));
			}
			PUT(put_string(&w, "startup_cost"));
			PUT(put_double(&w, c->startup_cost));
			PUT(put_string(&w, "total_cost"));
			PUT(put_double(&w, c->total_cost));
		}
	}
	PUT(put_string(&w, "final_path_candidates"));
	if (!in->final_paths_present) {
		PUT(put_nil(&w));
	} else {
		PUT(put_array(&w, in->final_path_count));
		for (size_t i = 0; i < in->final_path_count; i++) {
			const struct sql_replay_final_path *path =
				&in->final_paths[i];
			PUT(put_map(&w, 6));
			PUT(put_string(&w, "is_ordered"));
			PUT(put_int(&w, path->is_ordered));
			PUT(put_string(&w, "output_rows_logest"));
			PUT(put_int(&w, path->output_rows_logest));
			PUT(put_string(&w, "path_cost_logest"));
			PUT(put_int(&w, path->path_cost_logest));
			PUT(put_string(&w, "plan_fingerprint"));
			PUT(put_string(&w, path->plan_fingerprint));
			PUT(put_string(&w, "reverse_mask"));
			PUT(put_uint(&w, path->reverse_mask));
			PUT(put_string(&w, "unsorted_cost_logest"));
			PUT(put_int(&w, path->unsorted_cost_logest));
		}
	}
	PUT(put_string(&w, "limit"));
	PUT(in->limit_present ? put_uint(&w, in->limit) : put_nil(&w));
	PUT(put_string(&w, "offset"));
	PUT(in->offset_present ? put_uint(&w, in->offset) : put_nil(&w));
	PUT(put_string(&w, "order_by"));
	PUT(put_array(&w, in->order_by_count));
	for (size_t i = 0; i < in->order_by_count; i++) {
		PUT(put_map(&w, 3));
		PUT(put_string(&w, "descending"));
		PUT(put_bool(&w, in->order_by[i].descending));
		PUT(put_string(&w, "expression"));
		PUT(put_string(&w, in->order_by[i].canonical_expression));
		PUT(put_string(&w, "nulls_first"));
		PUT(put_bool(&w, in->order_by[i].nulls_first));
	}
	PUT(put_string(&w, "planner"));
	PUT(put_map(&w, 4));
	PUT(put_string(&w, "algorithm_version"));
	PUT(put_uint(&w, in->planner_algorithm_version));
	PUT(put_string(&w, "beam_width"));
	PUT(put_uint(&w, in->beam_width));
	PUT(put_string(&w, "config_version"));
	PUT(put_uint(&w, in->planner_config_version));
	PUT(put_string(&w, "final_path_selector_version"));
	PUT(put_uint(&w, in->selector_version));
	PUT(put_string(&w, "predicate"));
	PUT(put_string(&w, in->predicate));
	PUT(put_string(&w, "projections"));
	PUT(put_array(&w, in->projection_count));
	for (size_t i = 0; i < in->projection_count; i++)
		PUT(put_string(&w, in->projections[i]));
	PUT(put_string(&w, "relation"));
	PUT(put_relation(&w, in, indexes));
	PUT(put_string(&w, "version"));
	PUT(put_uint(&w, 5));
	free(indexes);
	*data = w.data;
	*size = w.size;
	return SQL_REPLAY_INPUT_OK;
fail:
	free(indexes);
	free(w.data);
	return w.invalid_size ? SQL_REPLAY_INPUT_INVALID :
				SQL_REPLAY_INPUT_NOMEM;
}

#undef PUT

static char *
copy_nonempty(const char *s)
{
	if (s == NULL || s[0] == '\0')
		return NULL;
	size_t n = strlen(s);
	if (n == SIZE_MAX)
		return NULL;
	char *copy = malloc(n + 1);
	if (copy != NULL)
		memcpy(copy, s, n + 1);
	return copy;
}

static bool
valid_stats(const struct sql_replay_relation_spec *r)
{
	if (!r->statistics_present)
		return r->row_count == 0 && r->cardinality_semantics == 0 &&
		       r->population_basis == NULL &&
		       r->average_row_width == 0 && r->width_basis == NULL &&
		       r->width_denominator_count == 0 &&
		       r->confidence_ppm == 0 && r->confidence_source == NULL &&
		       r->collected_at == 0 && r->modification_epoch == 0;
	return r->cardinality_semantics >=
				       SQL_REPLAY_CARDINALITY_VISIBLE_ROWS &&
			       r->cardinality_semantics <=
				       SQL_REPLAY_CARDINALITY_ESTIMATE &&
			       isfinite(r->average_row_width) &&
			       r->average_row_width >= 0 ?
		       r->population_basis != NULL &&
			       r->population_basis[0] != '\0' &&
			       r->width_basis != NULL &&
			       r->width_basis[0] != '\0' &&
			       r->width_denominator_count != 0 &&
			       r->confidence_ppm <= 1000000 &&
			       r->confidence_source != NULL &&
			       r->confidence_source[0] != '\0' :
		       false;
}

void
sql_replay_input_delete(struct sql_replay_input *input)
{
	if (input == NULL)
		return;
	free(input->relation_key);
	free(input->relation_definition);
	for (size_t i = 0; input->columns != NULL && i < input->column_count;
	     i++) {
		free(input->columns[i].type);
		free(input->columns[i].collation);
	}
	free(input->columns);
	for (size_t i = 0; input->indexes != NULL && i < input->index_count;
	     i++) {
		free(input->indexes[i].logical_key);
		free(input->indexes[i].canonical_definition);
		free(input->indexes[i].part_columns);
		free(input->indexes[i].population_basis);
		free(input->indexes[i].ndv_basis);
		free(input->indexes[i].distinct_prefixes);
	}
	free(input->indexes);
	free(input->population_basis);
	free(input->width_basis);
	free(input->confidence_source);
	free(input->predicate);
	for (size_t i = 0;
	     input->projections != NULL && i < input->projection_count; i++)
		free(input->projections[i]);
	free(input->projections);
	for (size_t i = 0; input->order_by != NULL && i < input->order_by_count;
	     i++)
		free(input->order_by[i].canonical_expression);
	free(input->order_by);
	for (size_t i = 0; input->access_candidates != NULL &&
			   i < input->access_candidate_count;
	     i++) {
		struct sql_replay_access_candidate *c =
			&input->access_candidates[i];
		free(c->logical_key);
		free(c->index_key);
		for (size_t j = 0;
		     c->constraints != NULL && j < c->constraint_count; j++)
			free(c->constraints[j].canonical_expression);
		free(c->constraints);
		free(c->projected_columns);
		for (size_t j = 0;
		     c->produced_order != NULL && j < c->produced_order_count;
		     j++)
			free(c->produced_order[j].canonical_expression);
		free(c->produced_order);
	}
	free(input->access_candidates);
	for (size_t i = 0; input->final_paths != NULL &&
				   i < input->final_path_count; i++)
		free(input->final_paths[i].plan_fingerprint);
	free(input->final_paths);
	free(input);
}

static bool
valid_index_specs(const struct sql_replay_input_spec *spec)
{
	const struct sql_replay_relation_spec *r = &spec->relation;
	for (size_t i = 0; i < r->index_count; i++) {
		const struct sql_replay_index_spec *idx = &r->indexes[i];
		if (idx->logical_key == NULL || idx->logical_key[0] == '\0' ||
		    idx->canonical_definition == NULL ||
		    idx->canonical_definition[0] == '\0' ||
		    idx->part_count == 0 || idx->part_columns == NULL ||
		    idx->part_count > SIZE_MAX / sizeof(uint32_t) ||
		    idx->prefix_count > SIZE_MAX / sizeof(uint64_t) ||
		    (idx->prefix_count != 0 && idx->distinct_prefixes == NULL))
			return false;
		for (size_t j = 0; j < idx->part_count; j++) {
			if (idx->part_columns[j] >= r->column_count)
				return false;
		}
		for (size_t j = i + 1; j < r->index_count; j++) {
			if (r->indexes[j].logical_key == NULL ||
			    strcmp(idx->logical_key,
				   r->indexes[j].logical_key) == 0)
				return false;
		}
		if (idx->statistics_present) {
			if (idx->population_basis == NULL ||
			    idx->population_basis[0] == '\0' ||
			    idx->ndv_basis == NULL ||
			    idx->ndv_basis[0] == '\0' ||
			    idx->definition_version == 0 ||
			    idx->tuple_count_semantics <
				    SQL_REPLAY_CARDINALITY_VISIBLE_ROWS ||
			    idx->tuple_count_semantics >
				    SQL_REPLAY_CARDINALITY_ESTIMATE ||
			    idx->prefix_count != idx->part_count ||
			    !r->statistics_present)
				return false;
			uint64_t prev = 0;
			for (size_t j = 0; j < idx->prefix_count; j++) {
				uint64_t ndv = idx->distinct_prefixes[j];
				if (ndv > idx->tuple_count ||
				    (j != 0 && ndv < prev))
					return false;
				prev = ndv;
			}
		} else if (idx->tuple_count != 0 ||
			   idx->tuple_count_semantics != 0 ||
			   idx->definition_version != 0 ||
			   idx->population_basis != NULL ||
			   idx->ndv_basis != NULL || idx->prefix_count != 0) {
			return false;
		}
	}
	return true;
}

static bool
valid_spec(const struct sql_replay_input_spec *spec)
{
	if (spec == NULL || spec->relation.logical_key == NULL ||
	    spec->relation.logical_key[0] == '\0' ||
	    spec->relation.canonical_definition == NULL ||
	    spec->relation.canonical_definition[0] == '\0' ||
	    spec->relation.column_count == 0 ||
	    spec->relation.columns == NULL ||
	    spec->relation.column_count >
		    SIZE_MAX / sizeof(struct sql_replay_column) ||
	    spec->relation.index_count >
		    SIZE_MAX / sizeof(struct sql_replay_index) ||
	    (spec->relation.index_count != 0 &&
	     spec->relation.indexes == NULL) ||
	    !valid_stats(&spec->relation) || !valid_index_specs(spec) ||
	    !canonical_expression_valid(spec->predicate,
					spec->relation.column_count) ||
	    spec->projection_count == 0 || spec->projections == NULL ||
	    spec->projection_count > SIZE_MAX / sizeof(char *) ||
	    (spec->order_by_count != 0 && spec->order_by == NULL) ||
	    (spec->access_candidate_count != 0 &&
	     spec->access_candidates == NULL) ||
	    (!spec->access_candidates_present &&
	     spec->access_candidate_count != 0) ||
	    (spec->final_path_count != 0 && spec->final_paths == NULL) ||
	    (!spec->final_paths_present &&
	     (spec->final_path_count != 0 || spec->selector_version != 0)) ||
	    (spec->final_paths_present &&
	     spec->selector_version != SQL_REPLAY_SELECTOR_FINAL_PATH_V1) ||
	    spec->final_path_count >
		    SIZE_MAX / sizeof(struct sql_replay_final_path) ||
	    spec->access_candidate_count >
		    SIZE_MAX / sizeof(struct sql_replay_access_candidate) ||
	    spec->order_by_count > SIZE_MAX / sizeof(struct sql_replay_order) ||
	    (!spec->limit_present && spec->limit != 0) ||
	    (!spec->offset_present && spec->offset != 0) ||
	    (spec->offset_present && !spec->limit_present) ||
	    spec->planner_algorithm_version == 0 ||
	    spec->planner_config_version == 0 || spec->beam_width == 0)
		return false;
	for (size_t i = 0; i < spec->relation.column_count; i++) {
		if (spec->relation.columns[i].type == NULL ||
		    spec->relation.columns[i].type[0] == '\0' ||
		    spec->relation.columns[i].collation == NULL ||
		    spec->relation.columns[i].collation[0] == '\0')
			return false;
	}
	for (size_t i = 0; i < spec->projection_count; i++) {
		if (!canonical_expression_valid(spec->projections[i],
						spec->relation.column_count))
			return false;
	}
	for (size_t i = 0; i < spec->order_by_count; i++) {
		if (!canonical_expression_valid(
			    spec->order_by[i].canonical_expression,
			    spec->relation.column_count))
			return false;
	}
	for (size_t i = 0; i < spec->access_candidate_count; i++) {
		const struct sql_replay_access_candidate_spec *c =
			&spec->access_candidates[i];
		if (c->logical_key == NULL || c->logical_key[0] == '\0' ||
		    c->kind < SQL_REPLAY_ACCESS_PRIMARY_KEY_POINT ||
		    c->kind > SQL_REPLAY_ACCESS_TABLE_FULL ||
		    (c->kind == SQL_REPLAY_ACCESS_TABLE_FULL &&
		     c->index_key != NULL) ||
		    (c->kind != SQL_REPLAY_ACCESS_TABLE_FULL &&
		     (c->index_key == NULL || c->index_key[0] == '\0')) ||
		    (c->constraint_count != 0 && c->constraints == NULL) ||
		    c->constraint_count >
			    SIZE_MAX / sizeof(struct sql_replay_constraint) ||
		    (c->projected_column_count != 0 &&
		     c->projected_columns == NULL) ||
		    c->projected_column_count > SIZE_MAX / sizeof(uint32_t) ||
		    (c->produced_order_count != 0 &&
		     c->produced_order == NULL) ||
		    c->produced_order_count >
			    SIZE_MAX / sizeof(struct sql_replay_order) ||
		    !isfinite(c->estimated_rows) || c->estimated_rows < 0 ||
		    !isfinite(c->startup_cost) || c->startup_cost < 0 ||
		    !isfinite(c->total_cost) ||
		    c->total_cost < c->startup_cost ||
		    !isfinite(c->estimated_width) || c->estimated_width < 0 ||
		    !isfinite(c->cost_rows) || c->cost_rows < 0 ||
		    c->estimate_confidence_ppm > 1000000 ||
		    c->cost_confidence_ppm > 1000000)
			return false;
		for (size_t j = i + 1; j < spec->access_candidate_count; j++)
			if (spec->access_candidates[j].logical_key == NULL ||
			    strcmp(c->logical_key,
				   spec->access_candidates[j].logical_key) == 0)
				return false;
		if (c->index_key != NULL) {
			bool found = false;
			for (size_t j = 0; j < spec->relation.index_count; j++)
				found |= strcmp(c->index_key,
						spec->relation.indexes[j]
							.logical_key) == 0;
			if (!found)
				return false;
		}
		for (size_t j = 0; j < c->constraint_count; j++)
			if (c->constraints[j].op <
				    SQL_REPLAY_CONSTRAINT_EQUAL ||
			    c->constraints[j].op >
				    SQL_REPLAY_CONSTRAINT_UPPER ||
			    !canonical_expression_valid(
				    c->constraints[j].canonical_expression,
				    spec->relation.column_count))
				return false;
		for (size_t j = 0; j < c->projected_column_count; j++)
			if (c->projected_columns[j] >=
			    spec->relation.column_count)
				return false;
		for (size_t j = 0; j < c->produced_order_count; j++)
			if (!canonical_expression_valid(
				    c->produced_order[j].canonical_expression,
				    spec->relation.column_count))
				return false;
	}
	for (size_t i = 0; i < spec->final_path_count; i++) {
		const struct sql_replay_final_path_spec *path =
			&spec->final_paths[i];
		if (path->plan_fingerprint == NULL ||
		    path->plan_fingerprint[0] == '\0' || path->is_ordered < -1 ||
		    (path->is_ordered >= 0 &&
		     (uint32_t)path->is_ordered > spec->order_by_count) ||
		    path->reverse_mask > 1)
			return false;
		for (size_t j = i + 1; j < spec->final_path_count; j++)
			if (spec->final_paths[j].plan_fingerprint == NULL ||
			    strcmp(path->plan_fingerprint,
				   spec->final_paths[j].plan_fingerprint) == 0)
				return false;
	}
	return true;
}

enum sql_replay_input_status
sql_replay_input_create(const struct sql_replay_input_spec *spec,
			struct sql_replay_input **result)
{
	if (result == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*result = NULL;
	if (!valid_spec(spec))
		return SQL_REPLAY_INPUT_INVALID;
	struct sql_replay_input *input = calloc(1, sizeof(*input));
	if (input == NULL)
		return SQL_REPLAY_INPUT_NOMEM;
	const struct sql_replay_relation_spec *r = &spec->relation;
	input->column_count = r->column_count;
	input->index_count = r->index_count;
	input->projection_count = spec->projection_count;
	input->order_by_count = spec->order_by_count;
	input->access_candidate_count = spec->access_candidate_count;
	input->access_candidates_present = spec->access_candidates_present;
	input->final_path_count = spec->final_path_count;
	input->final_paths_present = spec->final_paths_present;
	input->selector_version = spec->selector_version;
	input->relation_key = copy_nonempty(r->logical_key);
	input->relation_definition = copy_nonempty(r->canonical_definition);
	input->columns = calloc(r->column_count, sizeof(*input->columns));
	input->indexes = r->index_count == 0 ? NULL :
					       calloc(r->index_count,
						      sizeof(*input->indexes));
	input->population_basis = r->statistics_present ?
					  copy_nonempty(r->population_basis) :
					  NULL;
	input->width_basis =
		r->statistics_present ? copy_nonempty(r->width_basis) : NULL;
	input->confidence_source = r->statistics_present ?
					   copy_nonempty(r->confidence_source) :
					   NULL;
	input->predicate = copy_nonempty(spec->predicate);
	input->projections =
		calloc(spec->projection_count, sizeof(*input->projections));
	input->order_by =
		spec->order_by_count == 0 ?
			NULL :
			calloc(spec->order_by_count, sizeof(*input->order_by));
	input->access_candidates =
		spec->access_candidate_count == 0 ?
			NULL :
			calloc(spec->access_candidate_count,
			       sizeof(*input->access_candidates));
	input->final_paths = spec->final_path_count == 0 ?
				     NULL :
				     calloc(spec->final_path_count,
					    sizeof(*input->final_paths));
	if (input->relation_key == NULL || input->relation_definition == NULL ||
	    input->columns == NULL ||
	    (r->index_count != 0 && input->indexes == NULL) ||
	    (r->statistics_present &&
	     (input->population_basis == NULL || input->width_basis == NULL ||
	      input->confidence_source == NULL)) ||
	    input->predicate == NULL || input->projections == NULL ||
	    (spec->order_by_count != 0 && input->order_by == NULL) ||
	    (spec->access_candidate_count != 0 &&
	     input->access_candidates == NULL) ||
	    (spec->final_path_count != 0 && input->final_paths == NULL))
		goto nomem;
	for (size_t i = 0; i < r->column_count; i++) {
		input->columns[i].type = copy_nonempty(r->columns[i].type);
		input->columns[i].collation =
			copy_nonempty(r->columns[i].collation);
		if (input->columns[i].type == NULL ||
		    input->columns[i].collation == NULL)
			goto nomem;
	}
	for (size_t i = 0; i < r->index_count; i++) {
		const struct sql_replay_index_spec *src = &r->indexes[i];
		struct sql_replay_index *dst = &input->indexes[i];
		dst->logical_key = copy_nonempty(src->logical_key);
		dst->canonical_definition =
			copy_nonempty(src->canonical_definition);
		dst->part_count = src->part_count;
		dst->statistics_present = src->statistics_present;
		dst->tuple_count = src->tuple_count;
		dst->tuple_count_semantics = src->tuple_count_semantics;
		dst->definition_version = src->definition_version;
		dst->prefix_count = src->prefix_count;
		dst->part_columns =
			malloc(src->part_count * sizeof(*dst->part_columns));
		if (dst->part_columns == NULL)
			goto nomem;
		memcpy(dst->part_columns, src->part_columns,
		       src->part_count * sizeof(*dst->part_columns));
		if (src->statistics_present) {
			dst->population_basis =
				copy_nonempty(src->population_basis);
			dst->ndv_basis = copy_nonempty(src->ndv_basis);
			dst->distinct_prefixes =
				malloc(src->prefix_count *
				       sizeof(*dst->distinct_prefixes));
			if (dst->population_basis == NULL ||
			    dst->ndv_basis == NULL ||
			    dst->distinct_prefixes == NULL)
				goto nomem;
			memcpy(dst->distinct_prefixes, src->distinct_prefixes,
			       src->prefix_count *
				       sizeof(*dst->distinct_prefixes));
		}
		if (dst->logical_key == NULL ||
		    dst->canonical_definition == NULL ||
		    dst->part_columns == NULL ||
		    (src->statistics_present &&
		     (dst->population_basis == NULL || dst->ndv_basis == NULL ||
		      dst->distinct_prefixes == NULL)))
			goto nomem;
	}
	for (size_t i = 0; i < spec->projection_count; i++) {
		input->projections[i] = copy_nonempty(spec->projections[i]);
		if (input->projections[i] == NULL)
			goto nomem;
	}
	for (size_t i = 0; i < spec->order_by_count; i++) {
		input->order_by[i].canonical_expression =
			copy_nonempty(spec->order_by[i].canonical_expression);
		input->order_by[i].descending = spec->order_by[i].descending;
		input->order_by[i].nulls_first = spec->order_by[i].nulls_first;
		if (input->order_by[i].canonical_expression == NULL)
			goto nomem;
	}
	for (size_t i = 0; i < spec->access_candidate_count; i++) {
		const struct sql_replay_access_candidate_spec *src =
			&spec->access_candidates[i];
		struct sql_replay_access_candidate *dst =
			&input->access_candidates[i];
		dst->logical_key = copy_nonempty(src->logical_key);
		dst->kind = src->kind;
		dst->index_key = src->index_key == NULL ?
					 NULL :
					 copy_nonempty(src->index_key);
		dst->constraint_count = src->constraint_count;
		dst->constraints = src->constraint_count == 0 ?
					   NULL :
					   calloc(src->constraint_count,
						  sizeof(*dst->constraints));
		dst->descending = src->descending;
		dst->projected_column_count = src->projected_column_count;
		dst->projected_columns =
			src->projected_column_count == 0 ?
				NULL :
				malloc(src->projected_column_count *
				       sizeof(*dst->projected_columns));
		dst->produced_order_count = src->produced_order_count;
		dst->produced_order =
			src->produced_order_count == 0 ?
				NULL :
				calloc(src->produced_order_count,
				       sizeof(*dst->produced_order));
		dst->estimated_rows = src->estimated_rows;
		dst->estimate_confidence_ppm = src->estimate_confidence_ppm;
		dst->startup_cost = src->startup_cost;
		dst->total_cost = src->total_cost;
		dst->estimated_width = src->estimated_width;
		dst->cost_confidence_ppm = src->cost_confidence_ppm;
		dst->cost_rows = src->cost_rows;
		if (dst->logical_key == NULL ||
		    (src->index_key != NULL && dst->index_key == NULL) ||
		    (src->constraint_count != 0 && dst->constraints == NULL) ||
		    (src->projected_column_count != 0 &&
		     dst->projected_columns == NULL) ||
		    (src->produced_order_count != 0 &&
		     dst->produced_order == NULL))
			goto nomem;
		if (src->projected_column_count != 0)
			memcpy(dst->projected_columns, src->projected_columns,
			       src->projected_column_count *
				       sizeof(*dst->projected_columns));
		for (size_t j = 0; j < src->constraint_count; j++) {
			dst->constraints[j].canonical_expression = copy_nonempty(
				src->constraints[j].canonical_expression);
			dst->constraints[j].op = src->constraints[j].op;
			if (dst->constraints[j].canonical_expression == NULL)
				goto nomem;
		}
		for (size_t j = 0; j < src->produced_order_count; j++) {
			dst->produced_order[j].canonical_expression =
				copy_nonempty(src->produced_order[j]
						      .canonical_expression);
			dst->produced_order[j].descending =
				src->produced_order[j].descending;
			dst->produced_order[j].nulls_first =
				src->produced_order[j].nulls_first;
			if (dst->produced_order[j].canonical_expression == NULL)
				goto nomem;
		}
	}
	for (size_t i = 0; i < spec->final_path_count; i++) {
		const struct sql_replay_final_path_spec *src =
			&spec->final_paths[i];
		struct sql_replay_final_path *dst = &input->final_paths[i];
		dst->plan_fingerprint = copy_nonempty(src->plan_fingerprint);
		dst->path_cost_logest = src->path_cost_logest;
		dst->unsorted_cost_logest = src->unsorted_cost_logest;
		dst->output_rows_logest = src->output_rows_logest;
		dst->is_ordered = src->is_ordered;
		dst->reverse_mask = src->reverse_mask;
		if (dst->plan_fingerprint == NULL)
			goto nomem;
	}
	input->statistics_present = r->statistics_present;
	input->row_count = r->row_count;
	input->cardinality_semantics = r->cardinality_semantics;
	input->average_row_width = r->average_row_width;
	input->width_denominator_count = r->width_denominator_count;
	input->confidence_ppm = r->confidence_ppm;
	input->collected_at = r->collected_at;
	input->modification_epoch = r->modification_epoch;
	input->limit_present = spec->limit_present;
	input->limit = spec->limit;
	input->offset_present = spec->offset_present;
	input->offset = spec->offset;
	input->planner_algorithm_version = spec->planner_algorithm_version;
	input->planner_config_version = spec->planner_config_version;
	input->beam_width = spec->beam_width;
	input->selector_version = spec->selector_version;
	*result = input;
	return SQL_REPLAY_INPUT_OK;
nomem:
	sql_replay_input_delete(input);
	return SQL_REPLAY_INPUT_NOMEM;
}
