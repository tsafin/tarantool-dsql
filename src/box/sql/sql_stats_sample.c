#include "sql_stats_sample.h"

#include <stdlib.h>
#include <string.h>

struct sql_stats_sample_item {
	char *tuple;
	size_t tuple_size;
};

struct sql_stats_sample_reservoir {
	struct sql_stats_sample_item *items;
	size_t capacity;
	size_t count;
	uint64_t max_payload_bytes;
	uint64_t max_buffer_bytes;
	uint64_t payload_bytes;
	uint64_t allocated_bytes;
	uint64_t population;
	uint64_t random_state;
};

int
sql_stats_sample_reservoir_metadata_bytes(uint64_t capacity, uint64_t *bytes)
{
	if (bytes == NULL || capacity == 0 || capacity > SIZE_MAX /
	    sizeof(struct sql_stats_sample_item))
		return -1;
	size_t items_bytes = (size_t)capacity *
		sizeof(struct sql_stats_sample_item);
	if (items_bytes > SIZE_MAX - sizeof(struct sql_stats_sample_reservoir))
		return -1;
	*bytes = sizeof(struct sql_stats_sample_reservoir) + items_bytes;
	return 0;
}

static uint64_t
sql_stats_sample_next_random(struct sql_stats_sample_reservoir *reservoir)
{
	uint64_t z = (reservoir->random_state +=
		      UINT64_C(0x9e3779b97f4a7c15));
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

static uint64_t
sql_stats_sample_random_below(struct sql_stats_sample_reservoir *reservoir,
			      uint64_t bound)
{
	/* Rejection sampling avoids modulo bias in Algorithm R's choice. */
	uint64_t threshold = -bound % bound;
	uint64_t value;
	do {
		value = sql_stats_sample_next_random(reservoir);
	} while (value < threshold);
	return value % bound;
}

struct sql_stats_sample_reservoir *
sql_stats_sample_reservoir_new(uint64_t capacity, uint64_t max_bytes,
			       uint64_t max_buffer_bytes, uint64_t seed)
{
	uint64_t metadata_bytes;
	if (sql_stats_sample_reservoir_metadata_bytes(capacity,
						      &metadata_bytes) != 0 ||
	    max_bytes == 0 || max_buffer_bytes == 0)
		return NULL;
	if (metadata_bytes > max_buffer_bytes)
		return NULL;
	struct sql_stats_sample_reservoir *reservoir =
		calloc(1, sizeof(*reservoir));
	if (reservoir == NULL)
		return NULL;
	reservoir->items = calloc((size_t)capacity, sizeof(*reservoir->items));
	if (reservoir->items == NULL) {
		free(reservoir);
		return NULL;
	}
	reservoir->capacity = (size_t)capacity;
	reservoir->max_payload_bytes = max_bytes;
	reservoir->max_buffer_bytes = max_buffer_bytes;
	reservoir->allocated_bytes = metadata_bytes;
	reservoir->random_state = seed;
	return reservoir;
}

int
sql_stats_sample_reservoir_add(struct sql_stats_sample_reservoir *reservoir,
			       const char *tuple, size_t tuple_size)
{
	if (reservoir == NULL || tuple == NULL || tuple_size == 0 ||
	    reservoir->population == UINT64_MAX)
		return -1;
	size_t slot;
	if (reservoir->count < reservoir->capacity) {
		slot = reservoir->count;
	} else {
		uint64_t choice = sql_stats_sample_random_below(
			reservoir, reservoir->population + 1);
		if (choice >= reservoir->capacity) {
			reservoir->population++;
			return 0;
		}
		slot = (size_t)choice;
	}
	uint64_t old_size = slot < reservoir->count ?
		reservoir->items[slot].tuple_size : 0;
	if (reservoir->allocated_bytes < old_size ||
	    reservoir->payload_bytes < old_size ||
	    reservoir->payload_bytes - old_size >
		reservoir->max_payload_bytes ||
	    reservoir->allocated_bytes - old_size >
		reservoir->max_buffer_bytes ||
	    tuple_size > reservoir->max_payload_bytes -
		 (reservoir->payload_bytes - old_size) ||
	    tuple_size > reservoir->max_buffer_bytes -
		 (reservoir->allocated_bytes - old_size))
		return -1;
	/* Drop the old slot before allocating its replacement, keeping peak
	 * resident reservoir memory within max_buffer_bytes. The caller aborts
	 * the entire sample on allocation failure, so the old entry need not be
	 * restored.
	 */
	free(reservoir->items[slot].tuple);
	reservoir->items[slot].tuple = NULL;
	char *copy = malloc(tuple_size);
	if (copy == NULL)
		return -2;
	memcpy(copy, tuple, tuple_size);
	reservoir->items[slot] = (struct sql_stats_sample_item){
		.tuple = copy,
		.tuple_size = tuple_size,
	};
	reservoir->allocated_bytes = reservoir->allocated_bytes - old_size +
		tuple_size;
	reservoir->payload_bytes = reservoir->payload_bytes - old_size +
		tuple_size;
	if (reservoir->count < reservoir->capacity)
		reservoir->count++;
	reservoir->population++;
	return 0;
}

uint64_t
sql_stats_sample_reservoir_population(
	const struct sql_stats_sample_reservoir *reservoir)
{
	return reservoir != NULL ? reservoir->population : 0;
}

int
sql_stats_sample_reservoir_deliver(
	struct sql_stats_sample_reservoir *reservoir,
	struct sql_stats_sample_sink *sink, const uint32_t *field_ids,
	size_t field_count, struct sql_stats_sample_result *result)
{
	if (reservoir == NULL || sink == NULL || sink->consume == NULL ||
	    result == NULL || (field_count != 0 && field_ids == NULL))
		return -1;
	for (size_t i = 0; i < reservoir->count; ++i) {
		struct sql_stats_sample_item *item = &reservoir->items[i];
		if (sink->consume(sink->context, item->tuple, item->tuple_size,
				  field_ids, field_count) != 0)
			return -1;
		result->rows++;
		result->bytes += item->tuple_size;
	}
	result->population_known = true;
	result->visible_population = reservoir->population;
	result->with_replacement = false;
	return 0;
}

void
sql_stats_sample_reservoir_delete(
	struct sql_stats_sample_reservoir *reservoir)
{
	if (reservoir == NULL)
		return;
	for (size_t i = 0; i < reservoir->count; ++i)
		free(reservoir->items[i].tuple);
	free(reservoir->items);
	free(reservoir);
}

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
