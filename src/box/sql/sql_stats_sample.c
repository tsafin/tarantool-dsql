#include "sql_stats_sample.h"

static uint32_t
sql_stats_sample_next_seed(uint64_t *state)
{
	uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return (uint32_t)(z ^ (z >> 31));
}

int
sql_stats_sample_run(const struct sql_stats_sample_request *request,
		     struct sql_stats_sample_sink *sink,
		     struct sql_stats_sample_result *result,
		     sql_stats_sample_random_f *random_tuple,
		     void *random_context)
{
	if (result == NULL)
		return -1;
	*result = (struct sql_stats_sample_result){};
	if (request == NULL || sink == NULL || sink->consume == NULL ||
	    random_tuple == NULL || request->max_rows == 0 ||
	    request->max_bytes == 0 ||
	    (request->field_count != 0 && request->field_ids == NULL))
		return -1;
	result->with_replacement = true;
	uint64_t random_state = request->seed;
	while (result->rows < request->max_rows) {
		const char *tuple = NULL;
		size_t tuple_size = 0;
		if (random_tuple(random_context,
				 sql_stats_sample_next_seed(&random_state), &tuple,
				 &tuple_size) != 0)
			return -1;
		if (tuple == NULL)
			break;
		if (tuple_size > request->max_bytes - result->bytes)
			break;
		if (sink->consume(sink->context, tuple, tuple_size,
				  request->field_ids,
				  request->field_count) != 0)
			return -1;
		result->rows++;
		result->bytes += tuple_size;
	}
	return 0;
}
