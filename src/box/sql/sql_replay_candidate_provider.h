#ifndef TARANTOOL_SQL_REPLAY_CANDIDATE_PROVIDER_H
#define TARANTOOL_SQL_REPLAY_CANDIDATE_PROVIDER_H

#include "sql_replay_input.h"

/* A planner producer must distinguish a missing provider from an
 * authoritative empty candidate set. INCOMPLETE means the producer started
 * but could not represent every surviving candidate; partial output is never
 * promoted into a replay input.
 */
enum sql_replay_candidate_provider_state {
	SQL_REPLAY_CANDIDATES_UNAVAILABLE = 0,
	SQL_REPLAY_CANDIDATES_COMPLETE = 1,
	SQL_REPLAY_CANDIDATES_INCOMPLETE = 2,
};

struct sql_replay_candidate_provider {
	enum sql_replay_candidate_provider_state state;
	const struct sql_replay_access_candidate_spec *items;
	size_t count;
};

/* Build an owned replay input from provider output. For COMPLETE, candidate
 * specs may borrow statement/planner memory: this call deep-copies them before
 * returning. UNAVAILABLE builds an input with candidate presence unset, so
 * the ordinary replay-readiness check continues to reject it. INCOMPLETE
 * fails closed and returns no input, even if the provider supplied a partial
 * prefix. This bridge does not certify the producer's completeness claim.
 */
enum sql_replay_input_status
sql_replay_input_create_with_candidate_provider(
	const struct sql_replay_input_spec *base,
	const struct sql_replay_candidate_provider *provider,
	struct sql_replay_input **result);

#endif /* TARANTOOL_SQL_REPLAY_CANDIDATE_PROVIDER_H */
