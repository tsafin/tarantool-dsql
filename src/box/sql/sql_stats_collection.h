#ifndef TARANTOOL_SQL_STATS_COLLECTION_H
#define TARANTOOL_SQL_STATS_COLLECTION_H

#include "sql_stats_snapshot.h"
#include "sql_stats_sample.h"
#include "sql_stats_index_summary.h"

/* Provenance for confidence scores produced by the sample NDV bridge. */
#define SQL_STATS_INDEX_NDV_CONFIDENCE_SOURCE "uniform-occupancy-hll-v1"

/* Opaque producer-defined tokens: this API assigns no estimator policy. */
struct sql_stats_collection_generation {
	uint64_t catalog_version;
	uint64_t schema_version;
	uint64_t visibility_id;
};

struct sql_stats_expected_index {
	uint32_t index_id;
	uint64_t definition_version;
	size_t part_count;
};

struct sql_stats_expected_relation {
	uint32_t space_id;
	uint64_t modification_epoch;
	const struct sql_stats_expected_index *indexes;
	size_t index_count;
};

struct sql_stats_collected_index {
	uint32_t index_id;
	uint64_t definition_version;
	uint64_t visibility_id;
	uint64_t tuple_count;
	enum sql_stats_cardinality_semantics tuple_count_semantics;
	/* Must use the relation row-population basis. */
	const char *population_basis;
	/* May independently name the NDV estimator/hash domain. */
	const char *ndv_basis;
	const uint64_t *distinct_prefixes;
	size_t prefix_count;
};

struct sql_stats_index_summary;

/*
 * Convert one complete sampled index summary into a detached collection
 * record. Index identity and visibility are caller-supplied and are copied,
 * not independently verified against the summary. Prefix values are written
 * to caller-owned storage; confidence is the model/evidence score returned by
 * the population NDV estimator.
 */
int
sql_stats_collection_index_from_sample(
	const struct sql_stats_expected_index *expected,
	const struct sql_stats_sample_result *sample,
	const struct sql_stats_index_summary *summary, uint64_t visibility_id,
	uint64_t *distinct_prefixes, size_t prefix_capacity,
	struct sql_stats_collected_index *index, double *confidence,
	size_t max_temp_bytes, uint64_t max_work);

struct sql_stats_collected_relation {
	uint32_t space_id;
	uint64_t catalog_version;
	uint64_t schema_version;
	uint64_t visibility_id;
	uint64_t modification_epoch;
	double row_count;
	enum sql_stats_cardinality_semantics cardinality_semantics;
	const char *population_basis;
	double average_row_width;
	const char *width_basis;
	uint64_t width_denominator_count;
	double confidence;
	const char *confidence_source;
	uint64_t collected_at;
	const struct sql_stats_collected_index *indexes;
	size_t index_count;
};

struct sql_stats_collection_result {
	struct sql_stats_collection_generation generation;
	const struct sql_stats_collected_relation *relations;
	size_t relation_count;
};

/* One data source to pin inside a collector-owned engine read view. */
struct sql_stats_collection_target {
	uint32_t space_id;
	uint32_t index_id;
};

struct sql_stats_collection_context;
struct sql_stats_tx_index_spec;

/* Independent aggregate limits for one shared-view multi-relation build. */
struct sql_stats_collection_build_budget {
	size_t max_index_requests;
	size_t max_staging_bytes;
	size_t max_candidate_bytes;
	size_t max_temp_bytes;
	uint64_t max_work;
};

/* Complete producer input for one relation in a shared-view build. */
struct sql_stats_collection_relation_spec {
	const struct sql_stats_expected_relation *expected;
	const struct sql_stats_tx_index_spec *indexes;
	size_t index_count;
	uint32_t relation_index_id;
	double relation_confidence;
	const char *confidence_source;
};

/*
 * Open a shared engine read view for exactly the requested indexes. The
 * context owns the view until close. Unsupported engine/index read views fail
 * closed. Vinyl is opt-in through the context and supports bounded full scans;
 * point reads and iterator pagination are unsupported.
 */
struct sql_stats_collection_context *
sql_stats_collection_context_new(
	const struct sql_stats_collection_target *targets, size_t target_count);

void
sql_stats_collection_context_delete(
	struct sql_stats_collection_context *context);

uint64_t
sql_stats_collection_context_visibility_id(
	const struct sql_stats_collection_context *context);

uint64_t
sql_stats_collection_context_schema_version(
	const struct sql_stats_collection_context *context);

/*
 * Exhaustively scan one pinned index into a bounded reservoir. Succeeds only
 * if EOF is reached before max_tuples_examined and all tuple/buffer limits
 * hold. Secondary indexes are supported when their engine read view is.
 */
int
sql_stats_collection_context_sample_index(
	struct sql_stats_collection_context *context,
	const struct sql_stats_collection_target *target,
	const struct sql_stats_sample_request *request,
	struct sql_stats_sample_sink *sink,
	struct sql_stats_sample_result *result);

/*
 * An owned transaction sampler context. This pins transaction ownership and
 * READ_CONFIRMED isolation, not a common data snapshot; its transaction ID
 * must never be used as sql_stats_collection_generation.visibility_id.
 */
struct sql_stats_tx_context;

/* One caller-owned canonical extractor configuration for an expected index. */
struct sql_stats_tx_index_spec {
	struct sql_stats_collection_target target;
	const struct sql_stats_expected_index *expected;
	/* Copied by value; field_ids remains borrowed for this synchronous call. */
	struct sql_stats_sample_request request;
	uint8_t hll_precision;
	uint64_t hll_seed;
	size_t summary_max_bytes;
	sql_stats_index_value_extract_f *extract;
	void *extract_context;
	/* Shared-read-view collector only: use the engine's bounded index hash
	 * adapter. Transaction collectors require an explicit canonical extractor. */
	bool use_native_index_hash;
};

/*
 * Build a detached single-relation candidate by scanning every requested
 * index through this context's shared engine read view. The context must
 * contain exactly those indexes. Failure marks it unusable; no partial
 * candidate escapes. The assembler owns summaries/staging, while extractor
 * contexts and request field_ids are borrowed for this synchronous call.
 */
struct sql_stats_snapshot *
sql_stats_collection_context_build_sample_candidate(
	struct sql_stats_collection_context *context,
	const struct sql_stats_expected_relation *expected,
	const struct sql_stats_tx_index_spec *specs, size_t spec_count,
	uint32_t relation_index_id, double relation_confidence,
	const char *confidence_source, size_t max_candidate_bytes,
	size_t max_staging_bytes, size_t max_temp_bytes, uint64_t max_work);

/*
 * Build one detached candidate for a set of relations from the exact target
 * set pinned by context, then publish it with one existing publish call.
 * Relation/index targets must cover the context exactly once. Aggregate
 * staging includes all per-index summaries plus the largest reservoir: scans
 * are sequential and each reservoir/summary is destroyed before the next
 * relation begins. Temporary NDV inversion uses its maximum per-index
 * footprint; max_work bounds the sum of scan and inversion work across all
 * requested indexes. On failure the context is poisoned, no candidate
 * escapes, and the installed snapshot is unchanged. Parts remain private to
 * this call and can never be individually published through context.
 */
struct sql_stats_snapshot *
sql_stats_collection_context_build_sample_candidates(
	struct sql_stats_collection_context *context,
	const struct sql_stats_collection_relation_spec *relations,
	size_t relation_count,
	const struct sql_stats_collection_build_budget *budget);

/*
 * Install only the exact candidate assembled by this context. On success the
 * context is deleted and *context is set to NULL. The caller retains its own
 * candidate reference; on failure the caller must delete the context.
 */
int
sql_stats_collection_context_publish_candidate(
	struct sql_stats_collection_context **context,
	struct sql_stats_snapshot *candidate);

int
sql_stats_tx_context_begin(
	const struct sql_stats_collection_target *targets, size_t target_count,
	struct sql_stats_tx_context **context);

int
sql_stats_tx_context_sample_index(
	struct sql_stats_tx_context *context,
	const struct sql_stats_collection_target *target,
	const struct sql_stats_sample_request *request,
	struct sql_stats_sample_sink *sink,
	struct sql_stats_sample_result *result);

/* Stable volatile visibility token captured from the local commit vclock. */
uint64_t
sql_stats_tx_context_visibility_id(
	const struct sql_stats_tx_context *context);

/* Local space-cache generation captured and checked by the owned context. */
uint64_t
sql_stats_tx_context_catalog_version(
	const struct sql_stats_tx_context *context);

uint64_t
sql_stats_tx_context_schema_version(
	const struct sql_stats_tx_context *context);

/*
 * Sample all specs through this owned context and build one detached
 * single-relation candidate. relation_index_id selects the sample supplying
 * relation population and width. The assembler owns summaries/staging, but
 * extractor contexts and request field_ids are borrowed during the call.
 * Failure returns NULL, marks the context failed, and leaves installed state
 * unchanged. This does not commit or publish.
 */
struct sql_stats_snapshot *
sql_stats_tx_context_build_sample_candidate(
	struct sql_stats_tx_context *context,
	const struct sql_stats_expected_relation *expected,
	const struct sql_stats_tx_index_spec *specs, size_t spec_count,
	uint32_t relation_index_id, double relation_confidence,
	const char *confidence_source, size_t max_candidate_bytes,
	size_t max_staging_bytes, size_t max_temp_bytes, uint64_t max_work);

/*
 * Commit and publish only the exact candidate returned by this context's
 * assembler. The caller retains ownership of its candidate reference; the
 * context holds a separate reference until finish/abort. Failure preserves
 * the installed snapshot and consumes the owned transaction when possible.
 */
int
sql_stats_tx_context_finish_sample_candidate_and_publish(
	struct sql_stats_tx_context **context,
	struct sql_stats_snapshot *candidate);

/* Finish commits only after every requested index sample succeeds. */
int
sql_stats_tx_context_finish(struct sql_stats_tx_context **context);

/*
 * Validate and deep-copy one complete collection candidate, commit the
 * owned sampling transaction, revalidate its captured generations, then
 * publish the immutable snapshot. Any failure leaves the installed snapshot
 * unchanged and consumes/aborts the owned context when possible.
 */
int
sql_stats_tx_context_finish_and_publish(
	struct sql_stats_tx_context **context,
	const struct sql_stats_expected_relation *expected, size_t expected_count,
	const struct sql_stats_collection_result *result, size_t max_bytes);

/* Abort rolls back only the transaction owned by this context. */
int
sql_stats_tx_context_abort(struct sql_stats_tx_context **context);

/* Exact relation population fact produced by an engine sampler. */
struct sql_stats_collected_population {
	uint64_t row_count;
	enum sql_stats_cardinality_semantics semantics;
};

/*
 * Extract the exact visible population from a successful engine sample.
 * This does not establish/assign a visibility token or generation: the
 * caller must capture that boundary around the engine call before combining
 * the fact with any other summaries. Returns false for unknown or internally
 * inconsistent sampler results.
 */
bool
sql_stats_collection_population_from_sample(
	const struct sql_stats_sample_result *sample,
	struct sql_stats_collected_population *population);

/* Sample-average serialized tuple width; unavailable for an empty sample. */
struct sql_stats_collected_width {
	double average_bytes;
	uint64_t denominator_rows;
};

/* One engine-produced index summary. */
struct sql_stats_sampled_index {
	const struct sql_stats_expected_index *expected;
	const struct sql_stats_sample_result *sample;
	const struct sql_stats_index_summary *summary;
};

bool
sql_stats_collection_width_from_sample(
	const struct sql_stats_sample_result *sample,
	struct sql_stats_collected_width *width);

/*
 * Convert all index summaries for one relation into a detached immutable
 * candidate. Every index and the relation sample must report the same exact
 * visible population. The caller must ensure that every summary was produced
 * for the supplied shared generation/visibility; this helper does not verify
 * summary association or create a visibility boundary. Relation confidence
 * and its provenance are caller-supplied policy. Per-index model confidence
 * values are returned in input order only after the full candidate succeeds.
 * Any error returns NULL without modifying the output array or installed state.
 */
struct sql_stats_snapshot *
sql_stats_collection_build_sample_candidate(
	const struct sql_stats_collection_generation *generation,
	const struct sql_stats_expected_relation *expected,
	const struct sql_stats_sampled_index *indexes, size_t index_count,
	const struct sql_stats_sample_result *relation_sample,
	double relation_confidence, const char *confidence_source,
	double *index_confidences,
	size_t max_bytes, size_t max_temp_bytes, uint64_t max_work);

/*
 * Validate exact relation/index/prefix completeness and common generation,
 * then deep-copy a candidate through sql_stats_snapshot_new(). No global
 * state is changed. All semantic/provenance strings are caller-defined and
 * copied verbatim; no value is inferred or normalized here. A zero-row
 * relation may omit width (zero average, NULL basis, zero denominator), since
 * an empty sample provides no measured width.
 */
struct sql_stats_snapshot *
sql_stats_collection_build_candidate(
	const struct sql_stats_collection_generation *expected_generation,
	const struct sql_stats_expected_relation *expected, size_t expected_count,
	const struct sql_stats_collection_result *result, size_t max_bytes);

#ifdef SQL_STATS_COLLECTION_TESTING
/* Unit-target-only one-shot failure after N successful staging allocations. */
void sql_stats_collection_test_fail_allocation_after(long successful_allocations);
#endif

#endif /* TARANTOOL_SQL_STATS_COLLECTION_H */
