#include "sql_replay_candidate_provider.h"

void
sql_replay_candidate_capture_init(struct sql_replay_candidate_capture *capture,
				  struct sql_replay_access_candidate_spec *storage,
				  size_t capacity)
{
	if (capture == NULL)
		return;
	*capture = (struct sql_replay_candidate_capture) {
		.items = storage,
		.capacity = capacity,
	};
	if (capacity != 0 && storage == NULL)
		capture->incomplete = true;
}

void
sql_replay_candidate_capture_begin(
	struct sql_replay_candidate_capture *capture)
{
	if (capture != NULL)
		capture->started = true;
}

bool
sql_replay_candidate_capture_add(
	struct sql_replay_candidate_capture *capture,
	const struct sql_replay_access_candidate_spec *candidate)
{
	if (capture == NULL)
		return false;
	capture->started = true;
	if (capture->incomplete || candidate == NULL ||
	    capture->count >= capture->capacity || capture->items == NULL) {
		capture->incomplete = true;
		return false;
	}
	capture->items[capture->count++] = *candidate;
	return true;
}

void
sql_replay_candidate_capture_reject(
	struct sql_replay_candidate_capture *capture)
{
	if (capture == NULL)
		return;
	capture->started = true;
	capture->incomplete = true;
}

void
sql_replay_candidate_capture_finish(
	struct sql_replay_candidate_capture *capture, bool enumeration_complete,
	struct sql_replay_candidate_provider *provider)
{
	if (provider == NULL)
		return;
	*provider = (struct sql_replay_candidate_provider) {
		.state = SQL_REPLAY_CANDIDATES_INCOMPLETE,
	};
	if (capture == NULL || capture->incomplete ||
	    (capture->started && !enumeration_complete))
		return;
	if (!capture->started) {
		provider->state = SQL_REPLAY_CANDIDATES_UNAVAILABLE;
		return;
	}
	provider->state = SQL_REPLAY_CANDIDATES_COMPLETE;
	provider->items = capture->items;
	provider->count = capture->count;
}

enum sql_replay_input_status
sql_replay_input_create_with_candidate_provider(
	const struct sql_replay_input_spec *base,
	const struct sql_replay_candidate_provider *provider,
	struct sql_replay_input **result)
{
	if (result == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*result = NULL;
	if (base == NULL || provider == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	if (provider->state == SQL_REPLAY_CANDIDATES_INCOMPLETE)
		return SQL_REPLAY_INPUT_INCOMPLETE;
	struct sql_replay_input_spec spec = *base;
	switch (provider->state) {
	case SQL_REPLAY_CANDIDATES_UNAVAILABLE:
		if (provider->items != NULL || provider->count != 0)
			return SQL_REPLAY_INPUT_INVALID;
		spec.access_candidates = NULL;
		spec.access_candidate_count = 0;
		spec.access_candidates_present = false;
		break;
	case SQL_REPLAY_CANDIDATES_COMPLETE:
		if (provider->count != 0 && provider->items == NULL)
			return SQL_REPLAY_INPUT_INVALID;
		spec.access_candidates = provider->items;
		spec.access_candidate_count = provider->count;
		spec.access_candidates_present = true;
		break;
	default:
		return SQL_REPLAY_INPUT_INVALID;
	}
	return sql_replay_input_create(&spec, result);
}
