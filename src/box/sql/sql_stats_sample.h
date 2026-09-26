#ifndef TARANTOOL_SQL_STATS_SAMPLE_H
#define TARANTOOL_SQL_STATS_SAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct space;

struct sql_stats_sample_request {
	uint64_t max_rows;
	uint64_t max_bytes;
	uint64_t seed;
	const uint32_t *field_ids;
	size_t field_count;
};

struct sql_stats_sample_sink {
	void *context;
	int (*consume)(void *context, const char *tuple, size_t tuple_size,
		       const uint32_t *field_ids, size_t field_count);
};

struct sql_stats_sample_result {
	uint64_t rows;
	uint64_t bytes;
	/* Current prototype uses independent draws with replacement. */
	bool with_replacement;
};

typedef int
sql_stats_sample_random_f(void *context, uint32_t seed,
			  const char **tuple, size_t *tuple_size);

/* Shared bounded sampling loop, exposed for engine adapters and unit tests. */
int
sql_stats_sample_run(const struct sql_stats_sample_request *request,
		     struct sql_stats_sample_sink *sink,
		     struct sql_stats_sample_result *result,
		     sql_stats_sample_random_f *random_tuple,
		     void *random_context);

/**
 * Sample visible tuples through the storage engine in the caller's active
 * transaction. The memtx prototype draws from the primary index with
 * replacement; `rows` counts delivered draws, including duplicates.
 */
int
engine_sql_stats_sample(struct space *space,
			const struct sql_stats_sample_request *request,
			struct sql_stats_sample_sink *sink,
			struct sql_stats_sample_result *result);

#endif /* TARANTOOL_SQL_STATS_SAMPLE_H */
