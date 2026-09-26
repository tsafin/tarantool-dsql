#ifndef TARANTOOL_SQL_REPLAY_INPUT_H
#define TARANTOOL_SQL_REPLAY_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Detached, single-relation replay-input prototype. Strings are copied by
 * create(); relation_key and index_key are logical labels, never storage IDs.
 * predicate is a canonical expression produced by a separately validated
 * normalizer; this API deliberately does not accept Expr/catalog pointers. */
struct sql_replay_input_spec {
	const char *relation_key;
	const char *predicate;
	const char *index_key; /* NULL means a table scan is the only candidate. */
	const char *index_definition; /* canonical logical definition */
	bool statistics_present;
	uint64_t row_count;
	uint64_t average_row_width;
	uint32_t planner_algorithm_version;
	uint32_t planner_config_version;
	uint32_t beam_width;
};

struct sql_replay_input {
	char *relation_key;
	char *predicate;
	char *index_key;
	char *index_definition;
	bool statistics_present;
	uint64_t row_count;
	uint64_t average_row_width;
	uint32_t planner_algorithm_version;
	uint32_t planner_config_version;
	uint32_t beam_width;
};

enum sql_replay_input_status {
	SQL_REPLAY_INPUT_OK = 0,
	SQL_REPLAY_INPUT_INVALID,
	SQL_REPLAY_INPUT_NOMEM,
};

enum sql_replay_input_status
sql_replay_input_create(const struct sql_replay_input_spec *spec,
			struct sql_replay_input **result);
void sql_replay_input_delete(struct sql_replay_input *input);

#endif
