#include "sql_replay_candidate_provider.h"

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
