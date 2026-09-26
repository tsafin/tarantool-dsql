#include "sql_replay_input.h"

#include <stdlib.h>
#include <string.h>

static char *
copy_string(const char *s)
{
	if (s == NULL)
		return NULL;
	size_t n = strlen(s) + 1;
	char *copy = malloc(n);
	if (copy != NULL)
		memcpy(copy, s, n);
	return copy;
}

void
sql_replay_input_delete(struct sql_replay_input *input)
{
	if (input == NULL)
		return;
	free(input->relation_key);
	free(input->predicate);
	free(input->index_key);
	free(input->index_definition);
	free(input);
}

enum sql_replay_input_status
sql_replay_input_create(const struct sql_replay_input_spec *spec,
			struct sql_replay_input **result)
{
	if (result == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*result = NULL;
	if (spec == NULL || spec->relation_key == NULL ||
	    spec->relation_key[0] == '\0' || spec->predicate == NULL ||
	    spec->predicate[0] == '\0' || spec->planner_algorithm_version == 0 ||
	    spec->planner_config_version == 0 || spec->beam_width == 0 ||
	    ((spec->index_key == NULL) != (spec->index_definition == NULL)) ||
	    (!spec->statistics_present &&
	     (spec->row_count != 0 || spec->average_row_width != 0)))
		return SQL_REPLAY_INPUT_INVALID;
	struct sql_replay_input *input = calloc(1, sizeof(*input));
	if (input == NULL)
		return SQL_REPLAY_INPUT_NOMEM;
	input->relation_key = copy_string(spec->relation_key);
	input->predicate = copy_string(spec->predicate);
	input->index_key = copy_string(spec->index_key);
	input->index_definition = copy_string(spec->index_definition);
	if (input->relation_key == NULL || input->predicate == NULL ||
	    (spec->index_key != NULL &&
	     (input->index_key == NULL || input->index_definition == NULL))) {
		sql_replay_input_delete(input);
		return SQL_REPLAY_INPUT_NOMEM;
	}
	input->statistics_present = spec->statistics_present;
	input->row_count = spec->row_count;
	input->average_row_width = spec->average_row_width;
	input->planner_algorithm_version = spec->planner_algorithm_version;
	input->planner_config_version = spec->planner_config_version;
	input->beam_width = spec->beam_width;
	*result = input;
	return SQL_REPLAY_INPUT_OK;
}
