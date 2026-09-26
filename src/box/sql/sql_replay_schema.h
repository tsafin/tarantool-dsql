#ifndef TARANTOOL_SQL_REPLAY_SCHEMA_H
#define TARANTOOL_SQL_REPLAY_SCHEMA_H

#include "sql_replay_input.h"

struct space;

/* Detached catalog schema for one relation. storage_index_ids is private
 * association metadata for later in-memory statistics lookup; it is never
 * embedded in sql_replay_input or its canonical MsgPack representation.
 */
struct sql_replay_space_schema {
	struct sql_replay_relation_spec relation;
	uint32_t *storage_index_ids;
};

enum sql_replay_input_status
sql_replay_space_schema_create(const struct space *space,
			       struct sql_replay_space_schema *result);

void
sql_replay_space_schema_destroy(struct sql_replay_space_schema *schema);

#endif /* TARANTOOL_SQL_REPLAY_SCHEMA_H */
