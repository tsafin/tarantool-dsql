#ifndef TARANTOOL_SQL_STATS_INDEX_SUMMARY_H
#define TARANTOOL_SQL_STATS_INDEX_SUMMARY_H

#include "sql_stats_sample.h"
#include "sql_stats_hll.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert one sampled storage tuple to canonical SQL values for the ordered
 * parts of one index. Returned values are borrowed only until the callback
 * returns. The producer must apply SQL type, NULL, and collation semantics;
 * the summary deliberately does not guess them from MessagePack bytes.
 */
typedef int
sql_stats_index_value_extract_f(void *context, const char *tuple,
				 size_t tuple_size, const uint32_t *field_ids,
				 size_t field_count,
				 struct sql_stats_hll_value *parts,
				 size_t part_count);

struct sql_stats_index_summary;

/*
 * Allocate per-prefix HLL sketches for one index. Memory is bounded by
 * max_bytes, including HLL registers and the sketch pointer vector. The value
 * callback's transient storage is producer-owned and outside this budget.
 */
struct sql_stats_index_summary *
sql_stats_index_summary_new(size_t part_count, uint8_t precision,
			    uint64_t seed, size_t max_bytes,
			    sql_stats_index_value_extract_f *extract,
			    void *extract_context);

void
sql_stats_index_summary_delete(struct sql_stats_index_summary *summary);

/* Sink-compatible consumer; input must be a sampled tuple, not a raw scan. */
int
sql_stats_index_summary_consume(void *context, const char *tuple,
				size_t tuple_size, const uint32_t *field_ids,
				size_t field_count);

/* Number and byte sum of tuples actually delivered into this sample summary. */
uint64_t
sql_stats_index_summary_sample_rows(const struct sql_stats_index_summary *summary);
uint64_t
sql_stats_index_summary_sample_bytes(const struct sql_stats_index_summary *summary);

/* HLL estimate over delivered sample tuples for the first N index parts. */
int
sql_stats_index_summary_prefix_ndv(
	const struct sql_stats_index_summary *summary, size_t prefix_count,
	double *estimates, size_t estimate_count);

#ifdef __cplusplus
}
#endif

#endif /* TARANTOOL_SQL_STATS_INDEX_SUMMARY_H */
