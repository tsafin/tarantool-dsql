#ifndef TARANTOOL_SQL_REPLAY_EXTRACT_H
#define TARANTOOL_SQL_REPLAY_EXTRACT_H

#include "sql_replay_input.h"

struct Select;

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

#endif /* TARANTOOL_SQL_REPLAY_EXTRACT_H */
