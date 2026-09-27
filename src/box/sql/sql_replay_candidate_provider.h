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

/* Bounded staging area for a planner-side enumeration. The caller provides
 * storage valid until the provider is consumed by
 * sql_replay_input_create_with_candidate_provider(). Completion is explicit:
 * an overflow, unsupported candidate, or unfinished enumeration can never be
 * exposed as a complete (possibly empty) list.
 */
struct sql_replay_candidate_capture {
	struct sql_replay_access_candidate_spec *items;
	size_t capacity;
	size_t count;
	bool started;
	bool incomplete;
};

void
sql_replay_candidate_capture_init(struct sql_replay_candidate_capture *capture,
				  struct sql_replay_access_candidate_spec *storage,
				  size_t capacity);

/* Declare that the active planner began enumerating this relation. This is
 * required to distinguish a complete empty set from an unavailable producer.
 */
void
sql_replay_candidate_capture_begin(
	struct sql_replay_candidate_capture *capture);

/* Add a representable candidate. Returns false and permanently marks capture
 * incomplete for a null candidate or when the fixed capacity is exhausted.
 */
bool
sql_replay_candidate_capture_add(
	struct sql_replay_candidate_capture *capture,
	const struct sql_replay_access_candidate_spec *candidate);

/* Mark a candidate or planner shape as unrepresentable. */
void
sql_replay_candidate_capture_reject(
	struct sql_replay_candidate_capture *capture);

/* Publish a provider view only after enumeration has reached its normal end.
 * A premature end yields INCOMPLETE; a normally completed zero-item capture
 * is an authoritative empty list.
 */
void
sql_replay_candidate_capture_finish(
	struct sql_replay_candidate_capture *capture, bool enumeration_complete,
	struct sql_replay_candidate_provider *provider);

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
