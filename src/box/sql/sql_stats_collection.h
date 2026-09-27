#ifndef TARANTOOL_SQL_STATS_COLLECTION_H
#define TARANTOOL_SQL_STATS_COLLECTION_H

#include "sql_stats_snapshot.h"
#include "sql_stats_sample.h"

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
	const char *population_basis;
	const char *ndv_basis;
	const uint64_t *distinct_prefixes;
	size_t prefix_count;
};

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

/*
 * Open a shared engine read view for exactly the requested indexes. The
 * context owns the view until close. Unsupported engine/index read views fail
 * closed. The current core read-view API does not support Vinyl indexes.
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

/* Finish commits only after every requested index sample succeeds. */
int
sql_stats_tx_context_finish(struct sql_stats_tx_context **context);

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

bool
sql_stats_collection_width_from_sample(
	const struct sql_stats_sample_result *sample,
	struct sql_stats_collected_width *width);

/*
 * Validate exact relation/index/prefix completeness and common generation,
 * then deep-copy a candidate through sql_stats_snapshot_new(). No global
 * state is changed. All semantic/provenance strings are caller-defined and
 * copied verbatim; no value is inferred or normalized here.
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
