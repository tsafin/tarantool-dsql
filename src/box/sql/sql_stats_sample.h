#ifndef TARANTOOL_SQL_STATS_SAMPLE_H
#define TARANTOOL_SQL_STATS_SAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct space;

struct sql_stats_sample_request {
	/* Storage index ID whose tuple population is sampled. Zero is primary. */
	uint32_t index_id;
	uint64_t max_rows;
	uint64_t max_bytes;
	uint64_t seed;
	/* Vinyl-only bound for reservoir metadata plus retained tuple copies. */
	uint64_t max_buffer_bytes;
	/* Vinyl-only hard bounds. A successful scan must observe EOF before this
	 * many visible tuples are examined; reaching the cap is failure.
	 */
	uint64_t max_tuples_examined;
	uint64_t max_disk_sources;
	uint64_t max_page_reads;
	uint64_t max_iterator_keys;
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
	/* Set when the engine can report visible_population (memtx transaction-
	 * visible index count or Vinyl exhaustive scan).
	 */
	bool population_known;
	uint64_t visible_population;
	/* Memtx currently uses independent draws with replacement. */
	bool with_replacement;
};

struct sql_stats_sample_reservoir;

int
sql_stats_sample_reservoir_metadata_bytes(uint64_t capacity,
					  uint64_t *bytes);
struct sql_stats_sample_reservoir *
sql_stats_sample_reservoir_new(uint64_t capacity, uint64_t max_bytes,
			       uint64_t max_buffer_bytes, uint64_t seed);
int
sql_stats_sample_reservoir_add(struct sql_stats_sample_reservoir *reservoir,
			       const char *tuple, size_t tuple_size);
uint64_t
sql_stats_sample_reservoir_population(
	const struct sql_stats_sample_reservoir *reservoir);
int
sql_stats_sample_reservoir_deliver(
	struct sql_stats_sample_reservoir *reservoir,
	struct sql_stats_sample_sink *sink, const uint32_t *field_ids,
	size_t field_count, struct sql_stats_sample_result *result);
void
sql_stats_sample_reservoir_delete(
	struct sql_stats_sample_reservoir *reservoir);

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
 * Sample tuples through the storage engine. Engine-specific visibility and
 * transaction restrictions are documented in sql_stats_sampling.md.
 */
int
engine_sql_stats_sample(struct space *space,
			const struct sql_stats_sample_request *request,
			struct sql_stats_sample_sink *sink,
			struct sql_stats_sample_result *result);

#ifdef __cplusplus
}
#endif

#endif /* TARANTOOL_SQL_STATS_SAMPLE_H */
