#ifndef TARANTOOL_SQL_REPLAY_EXTRACT_H
#define TARANTOOL_SQL_REPLAY_EXTRACT_H

#include "sql_replay_input.h"

struct Select;
struct sql_stats_snapshot;

/*
 * Extract the expression and simple-bound portion of a resolved, single-
 * relation SELECT. `metadata` supplies the detached relation/statistics and
 * planner configuration and must describe the resolved source; its
 * expression/list/limit fields are ignored. The cursor map must bind the
 * source cursor to logical relation 0 and every other cursor to UINT32_MAX.
 * Unsupported expression/limit semantics fail closed. The returned model
 * owns no SQL AST or catalog pointers.
 */
enum sql_replay_input_status
sql_replay_input_extract_select(const struct Select *select,
				const struct sql_replay_input_spec *metadata,
				const uint32_t *cursor_to_relation,
				size_t cursor_count,
				struct sql_replay_input **result);

/* Capture catalog relation/index definitions from the resolved source space;
 * statistics remain explicitly absent until an immutable stats provider is
 * wired. Planner version/config values are supplied by the caller.
 */
enum sql_replay_input_status
sql_replay_input_extract_select_from_catalog(
	const struct Select *select, const uint32_t *cursor_to_relation,
	size_t cursor_count, uint32_t planner_algorithm_version,
	uint32_t planner_config_version, uint32_t beam_width,
	struct sql_replay_input **result);

/* As above, additionally copy any current-schema statistics present in the
 * supplied immutable provider. Missing or stale relation statistics remain
 * explicitly absent; unrepresentable measured values fail closed.
 */
enum sql_replay_input_status
sql_replay_input_extract_select_from_snapshot(
	const struct Select *select, const uint32_t *cursor_to_relation,
	size_t cursor_count, uint32_t planner_algorithm_version,
	uint32_t planner_config_version, uint32_t beam_width,
	const struct sql_stats_snapshot *snapshot, uint64_t current_schema_version,
	struct sql_replay_input **result);

#endif /* TARANTOOL_SQL_REPLAY_EXTRACT_H */
