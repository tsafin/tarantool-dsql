#include "sql_replay_input.h"

#include <stdlib.h>
#include <string.h>

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
		       r->population_basis == NULL && r->average_row_width == 0 &&
		       r->width_basis == NULL && r->width_denominator_count == 0 &&
		       r->confidence_ppm == 0 && r->confidence_source == NULL &&
		       r->collected_at == 0 && r->modification_epoch == 0;
	return r->cardinality_semantics == SQL_REPLAY_CARDINALITY_VISIBLE_ROWS ||
	       r->cardinality_semantics == SQL_REPLAY_CARDINALITY_ESTIMATE ?
	       r->population_basis != NULL && r->population_basis[0] != '\0' &&
	       r->width_basis != NULL && r->width_basis[0] != '\0' &&
	       r->width_denominator_count != 0 && r->confidence_ppm <= 1000000 &&
	       r->confidence_source != NULL && r->confidence_source[0] != '\0' :
	       false;
}

void
sql_replay_input_delete(struct sql_replay_input *input)
{
	if (input == NULL)
		return;
	free(input->relation_key);
	free(input->relation_definition);
	for (size_t i = 0; input->columns != NULL && i < input->column_count; i++) {
		free(input->columns[i].type);
		free(input->columns[i].collation);
	}
	free(input->columns);
	for (size_t i = 0; input->indexes != NULL && i < input->index_count; i++) {
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
	for (size_t i = 0; input->projections != NULL &&
	     i < input->projection_count; i++)
		free(input->projections[i]);
	free(input->projections);
	for (size_t i = 0; input->order_by != NULL && i < input->order_by_count; i++)
		free(input->order_by[i].canonical_expression);
	free(input->order_by);
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
		    idx->canonical_definition[0] == '\0' || idx->part_count == 0 ||
		    idx->part_count > r->column_count || idx->part_columns == NULL ||
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
			    strcmp(idx->logical_key, r->indexes[j].logical_key) == 0)
				return false;
		}
		if (idx->statistics_present) {
			if (idx->population_basis == NULL || idx->population_basis[0] == '\0' ||
			    idx->ndv_basis == NULL || idx->ndv_basis[0] == '\0' ||
			    idx->prefix_count != idx->part_count ||
			    !r->statistics_present)
				return false;
			uint64_t prev = 0;
			for (size_t j = 0; j < idx->prefix_count; j++) {
				uint64_t ndv = idx->distinct_prefixes[j];
				if (ndv > idx->tuple_count || (j != 0 && ndv < prev))
					return false;
				prev = ndv;
			}
		} else if (idx->tuple_count != 0 || idx->population_basis != NULL ||
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
	    spec->relation.column_count == 0 || spec->relation.columns == NULL ||
	    spec->relation.column_count > SIZE_MAX / sizeof(struct sql_replay_column) ||
	    spec->relation.index_count > SIZE_MAX / sizeof(struct sql_replay_index) ||
	    (spec->relation.index_count != 0 && spec->relation.indexes == NULL) ||
	    !valid_stats(&spec->relation) || !valid_index_specs(spec) ||
	    spec->predicate == NULL || spec->predicate[0] == '\0' ||
	    spec->projection_count == 0 || spec->projections == NULL ||
	    spec->projection_count > SIZE_MAX / sizeof(char *) ||
	    (spec->order_by_count != 0 && spec->order_by == NULL) ||
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
		if (spec->projections[i] == NULL || spec->projections[i][0] == '\0')
			return false;
	}
	for (size_t i = 0; i < spec->order_by_count; i++) {
		if (spec->order_by[i].canonical_expression == NULL ||
		    spec->order_by[i].canonical_expression[0] == '\0')
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
	input->relation_key = copy_nonempty(r->logical_key);
	input->relation_definition = copy_nonempty(r->canonical_definition);
	input->columns = calloc(r->column_count, sizeof(*input->columns));
	input->indexes = r->index_count == 0 ? NULL :
		calloc(r->index_count, sizeof(*input->indexes));
	input->population_basis = r->statistics_present ?
		copy_nonempty(r->population_basis) : NULL;
	input->width_basis = r->statistics_present ? copy_nonempty(r->width_basis) : NULL;
	input->confidence_source = r->statistics_present ?
		copy_nonempty(r->confidence_source) : NULL;
	input->predicate = copy_nonempty(spec->predicate);
	input->projections = calloc(spec->projection_count,
				    sizeof(*input->projections));
	input->order_by = spec->order_by_count == 0 ? NULL :
		calloc(spec->order_by_count, sizeof(*input->order_by));
	if (input->relation_key == NULL || input->relation_definition == NULL ||
	    input->columns == NULL || (r->index_count != 0 && input->indexes == NULL) ||
	    (r->statistics_present && (input->population_basis == NULL ||
	     input->width_basis == NULL || input->confidence_source == NULL)) ||
	    input->predicate == NULL || input->projections == NULL ||
	    (spec->order_by_count != 0 && input->order_by == NULL))
		goto nomem;
	for (size_t i = 0; i < r->column_count; i++) {
		input->columns[i].type = copy_nonempty(r->columns[i].type);
		input->columns[i].collation = copy_nonempty(r->columns[i].collation);
		if (input->columns[i].type == NULL || input->columns[i].collation == NULL)
			goto nomem;
	}
	for (size_t i = 0; i < r->index_count; i++) {
		const struct sql_replay_index_spec *src = &r->indexes[i];
		struct sql_replay_index *dst = &input->indexes[i];
		dst->logical_key = copy_nonempty(src->logical_key);
		dst->canonical_definition = copy_nonempty(src->canonical_definition);
		dst->part_count = src->part_count;
		dst->statistics_present = src->statistics_present;
		dst->tuple_count = src->tuple_count;
		dst->prefix_count = src->prefix_count;
		dst->part_columns = malloc(src->part_count * sizeof(*dst->part_columns));
		if (dst->part_columns == NULL)
			goto nomem;
		memcpy(dst->part_columns, src->part_columns,
		       src->part_count * sizeof(*dst->part_columns));
		if (src->statistics_present) {
			dst->population_basis = copy_nonempty(src->population_basis);
			dst->ndv_basis = copy_nonempty(src->ndv_basis);
			dst->distinct_prefixes = malloc(src->prefix_count *
						       sizeof(*dst->distinct_prefixes));
			if (dst->population_basis == NULL || dst->ndv_basis == NULL ||
			    dst->distinct_prefixes == NULL)
				goto nomem;
			memcpy(dst->distinct_prefixes, src->distinct_prefixes,
			       src->prefix_count * sizeof(*dst->distinct_prefixes));
		}
		if (dst->logical_key == NULL || dst->canonical_definition == NULL ||
		    dst->part_columns == NULL ||
		    (src->statistics_present && (dst->population_basis == NULL ||
		     dst->ndv_basis == NULL || dst->distinct_prefixes == NULL)))
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
	*result = input;
	return SQL_REPLAY_INPUT_OK;
nomem:
	sql_replay_input_delete(input);
	return SQL_REPLAY_INPUT_NOMEM;
}
