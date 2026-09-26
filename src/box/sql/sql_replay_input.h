#ifndef TARANTOOL_SQL_REPLAY_INPUT_H
#define TARANTOOL_SQL_REPLAY_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sql_replay_cardinality_semantics {
	SQL_REPLAY_CARDINALITY_VISIBLE_ROWS = 1,
	SQL_REPLAY_CARDINALITY_ESTIMATE = 2,
};

struct sql_replay_index_spec {
	const char *logical_key;
	const char *canonical_definition;
	const uint32_t *part_columns;
	size_t part_count;
	bool statistics_present;
	uint64_t tuple_count;
	const char *population_basis;
	const char *ndv_basis;
	const uint64_t *distinct_prefixes;
	size_t prefix_count;
};

struct sql_replay_column_spec {
	const char *type;
	const char *collation;
};

struct sql_replay_relation_spec {
	const char *logical_key;
	const char *canonical_definition;
	const struct sql_replay_column_spec *columns;
	size_t column_count;
	const struct sql_replay_index_spec *indexes;
	size_t index_count;
	bool statistics_present;
	uint64_t row_count;
	enum sql_replay_cardinality_semantics cardinality_semantics;
	const char *population_basis;
	uint64_t average_row_width;
	const char *width_basis;
	uint64_t width_denominator_count;
	uint32_t confidence_ppm;
	const char *confidence_source;
	uint64_t collected_at;
	uint64_t modification_epoch;
};

struct sql_replay_order_spec {
	const char *canonical_expression;
	bool descending;
	bool nulls_first;
};

/* Detached normalized single-relation SELECT subset. All strings are copied
 * by create(); identifiers are logical ordinals/labels, never storage IDs.
 * Expression strings are expected to come from the validated M3 normalizer.
 * This API deliberately accepts no Expr/catalog/storage pointers. */
struct sql_replay_input_spec {
	struct sql_replay_relation_spec relation;
	const char *predicate;
	const char *const *projections;
	size_t projection_count;
	const struct sql_replay_order_spec *order_by;
	size_t order_by_count;
	bool limit_present;
	uint64_t limit;
	bool offset_present;
	uint64_t offset;
	uint32_t planner_algorithm_version;
	uint32_t planner_config_version;
	uint32_t beam_width;
};

struct sql_replay_index {
	char *logical_key;
	char *canonical_definition;
	uint32_t *part_columns;
	size_t part_count;
	bool statistics_present;
	uint64_t tuple_count;
	char *population_basis;
	char *ndv_basis;
	uint64_t *distinct_prefixes;
	size_t prefix_count;
};

struct sql_replay_column {
	char *type;
	char *collation;
};

struct sql_replay_order {
	char *canonical_expression;
	bool descending;
	bool nulls_first;
};

struct sql_replay_input {
	char *relation_key;
	char *relation_definition;
	struct sql_replay_column *columns;
	size_t column_count;
	struct sql_replay_index *indexes;
	size_t index_count;
	bool statistics_present;
	uint64_t row_count;
	enum sql_replay_cardinality_semantics cardinality_semantics;
	char *population_basis;
	uint64_t average_row_width;
	char *width_basis;
	uint64_t width_denominator_count;
	uint32_t confidence_ppm;
	char *confidence_source;
	uint64_t collected_at;
	uint64_t modification_epoch;
	char *predicate;
	char **projections;
	size_t projection_count;
	struct sql_replay_order *order_by;
	size_t order_by_count;
	bool limit_present;
	uint64_t limit;
	bool offset_present;
	uint64_t offset;
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

/* Return owned deterministic MsgPack bytes for input format version 1. */
enum sql_replay_input_status
sql_replay_input_serialize(const struct sql_replay_input *input,
			   char **data, size_t *size);

#endif
