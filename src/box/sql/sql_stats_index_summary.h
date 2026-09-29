#ifndef TARANTOOL_SQL_STATS_INDEX_SUMMARY_H
#define TARANTOOL_SQL_STATS_INDEX_SUMMARY_H

#include "sql_stats_sample.h"
#include "sql_stats_hll.h"
#include "sql_stats_spacesaving.h"

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
struct index_def;
struct tuple_format;

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

/*
 * As above, and retain a bounded SpaceSaving candidate set for each scalar
 * index part. The caller's extractor must return canonical SQL value bytes,
 * and reserves type tag zero to mark NULL (NULL is excluded from MCVs).
 * Values larger than max_mcv_value_bytes fail the whole summary closed. The
 * total max_bytes budget includes HLL storage, SpaceSaving slots, each slot's
 * maximum key copy, and the reusable tagged-key scratch buffer.
 */
struct sql_stats_index_summary *
sql_stats_index_summary_new_with_mcv(size_t part_count, uint8_t precision,
				     uint64_t seed, size_t max_bytes,
				     sql_stats_index_value_extract_f *extract,
				     void *extract_context,
				     uint32_t mcv_capacity,
				     size_t max_mcv_value_bytes);

/*
 * Native tuple/key-definition adapter. It copies the key definition and
 * retains the tuple format, then uses Tarantool's per-part index hash
 * semantics (type and collation aware) to feed the prefix sketches. The
 * caller must pass the matching format and index definition from one captured
 * schema version. Only TREE/HASH indexes with STRING, DOUBLE, BOOLEAN,
 * UNSIGNED, or signed INTEGER parts are accepted. Signed INTEGER values use
 * canonical MessagePack integer encodings so hashing follows numeric equality.
 * Estimates inherit the engine index hash's 32-bit collision ceiling; they
 * are probabilistic and are not exact/canonical SQL values.
 */
struct sql_stats_index_summary *
sql_stats_index_summary_new_for_index(struct tuple_format *format,
				      const struct index_def *index_def,
				      uint8_t precision, uint64_t seed,
				      size_t max_bytes);

/* 0 for callback values, 32 for native index-hash summaries. */
uint8_t
sql_stats_index_summary_hash_bits(
	const struct sql_stats_index_summary *summary);

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

/* Per-part MCV candidate count and borrowed slot access, if enabled. */
uint32_t
sql_stats_index_summary_mcv_count(
	const struct sql_stats_index_summary *summary, size_t part);

int
sql_stats_index_summary_mcv_at(
	const struct sql_stats_index_summary *summary, size_t part,
	uint32_t slot, uint8_t *type_tag, const void **value, size_t *value_size,
	struct sql_stats_spacesaving_entry *entry);

uint64_t
sql_stats_index_summary_mcv_sample_nonnull_rows(
	const struct sql_stats_index_summary *summary, size_t part);

/*
 * Estimate population prefix NDVs by inverting the uniform-occupancy model
 * over a known sampled population. This is a model-based estimate, not an
 * exact count: the caller must retain the returned confidence and provenance.
 * For sampling with replacement the model uses independent uniform draws; for
 * a reservoir sample it uses the finite-population no-observation product.
 * Returns -1 without modifying outputs when sample/population metadata or the
 * summary is incomplete/inconsistent or the temporary arrays would exceed
 * max_temp_bytes or the estimator's bounded work would exceed max_work. The
 * latter counts the inversion's bounded occupancy-product work across all
 * prefixes.
 */
int
sql_stats_index_summary_population_prefix_ndv(
	const struct sql_stats_index_summary *summary,
	const struct sql_stats_sample_result *sample, size_t prefix_count,
	uint64_t *estimates, size_t estimate_count, double *confidence,
	size_t max_temp_bytes, uint64_t max_work);

#ifdef __cplusplus
}
#endif

#endif /* TARANTOOL_SQL_STATS_INDEX_SUMMARY_H */
