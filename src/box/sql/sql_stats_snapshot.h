#ifndef TARANTOOL_SQL_STATS_SNAPSHOT_H
#define TARANTOOL_SQL_STATS_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Values describe the meaning of a relation cardinality, not its source. */
enum sql_stats_cardinality_semantics {
	SQL_STATS_CARDINALITY_VISIBLE_ROWS = 1,
	SQL_STATS_CARDINALITY_PHYSICAL_TUPLES = 2,
	SQL_STATS_CARDINALITY_ESTIMATE = 3,
};

/*
 * Caller-canonicalized sampled MCV candidate. type_tag zero is reserved for
 * NULL and is invalid here; values are opaque canonical bytes. estimate and
 * error are a SpaceSaving-style upper estimate and error bound, so the
 * candidate's conservative lower bound is estimate - error.
 */
struct sql_stats_mcv_input {
	uint8_t type_tag;
	const void *value;
	size_t value_size;
	uint64_t estimate;
	uint64_t error;
};

/*
 * One equi-depth histogram boundary over the non-NULL sample. Values use the
 * same caller-canonicalized encoding as MCV entries. Boundaries must share a
 * nonzero type tag and have strictly increasing cumulative counts; the final
 * count must equal sample_nonnull_rows. Ordering of value bytes is validated
 * by the producer because the snapshot intentionally owns no collation.
 */
struct sql_stats_histogram_bucket_input {
	uint8_t type_tag;
	const void *upper_bound;
	size_t upper_bound_size;
	uint64_t cumulative_count;
};

struct sql_stats_index_part_input {
	/* Rows actually delivered to this part's sketch, including NULLs. */
	uint64_t sample_rows;
	/* Of sample_rows, those whose value is not NULL. */
	uint64_t sample_nonnull_rows;
	const struct sql_stats_mcv_input *mcv;
	size_t mcv_count;
	const struct sql_stats_histogram_bucket_input *histogram;
	size_t histogram_count;
};

struct sql_stats_index_input {
	uint32_t index_id;
	uint64_t tuple_count;
	/* Optional caller-defined provenance; NULL/zero preserves legacy callers. */
	enum sql_stats_cardinality_semantics tuple_count_semantics;
	const char *population_basis;
	const char *ndv_basis;
	uint64_t definition_version;
	const uint64_t *distinct_prefixes;
	size_t prefix_count;
	/* Optional volatile S2 payload, one entry per index part. */
	const struct sql_stats_index_part_input *parts;
	size_t part_count;
};

struct sql_stats_relation_input {
	uint32_t space_id;
	double row_count;
	/* Optional caller-defined population semantics/provenance. */
	const char *population_basis;
	double average_row_width;
	/* Optional caller-defined provenance; NULL preserves legacy callers. */
	const char *width_basis;
	uint64_t width_denominator_count;
	double confidence;
	const char *confidence_source;
	enum sql_stats_cardinality_semantics cardinality_semantics;
	/* Opaque units: the API stores but does not interpret these values. */
	uint64_t collected_at;
	uint64_t modification_epoch;
	uint64_t visibility_id;
	const struct sql_stats_index_input *indexes;
	size_t index_count;
};

struct sql_stats_snapshot;
struct sql_stats_relation;
struct sql_stats_index;

enum sql_stats_lookup_status {
	SQL_STATS_LOOKUP_AVAILABLE,
	SQL_STATS_LOOKUP_MISSING,
	SQL_STATS_LOOKUP_STALE,
};

/**
 * Construct a deep-copied immutable snapshot. `max_bytes` bounds all owned
 * allocation bytes, including the snapshot and nested index/prefix arrays;
 * zero is invalid. Duplicate IDs, invalid numeric values, malformed prefix
 * counts, or budget overflow reject the whole snapshot and return NULL.
 *
 * Snapshot API version is currently 5 and intentionally distinct from the
 * persistence payload/catalog/schema versions. `schema_version` mismatch at
 * lookup time reports STALE instead of making ordinary prepare fail.
 */
struct sql_stats_snapshot *
sql_stats_snapshot_new(uint64_t catalog_version, uint64_t schema_version,
		       const struct sql_stats_relation_input *relations,
		       size_t relation_count, size_t max_bytes);

#ifdef SQL_STATS_SNAPSHOT_TESTING
/* Unit-target-only one-shot failure after N successful allocations. */
void sql_stats_snapshot_test_fail_allocation_after(long successful_allocations);
#endif

void
sql_stats_snapshot_retain(struct sql_stats_snapshot *snapshot);

void
sql_stats_snapshot_release(struct sql_stats_snapshot *snapshot);

uint32_t
sql_stats_snapshot_api_version(const struct sql_stats_snapshot *snapshot);

uint64_t
sql_stats_snapshot_catalog_version(const struct sql_stats_snapshot *snapshot);

uint64_t
sql_stats_snapshot_schema_version(const struct sql_stats_snapshot *snapshot);

size_t
sql_stats_snapshot_bytes(const struct sql_stats_snapshot *snapshot);

size_t
sql_stats_snapshot_relation_count(const struct sql_stats_snapshot *snapshot);

/*
 * Read-only ordered enumeration for snapshot merge/replace operations. The
 * returned relation/index pointers are borrowed and valid while the snapshot
 * is retained by the caller. Out-of-range requests return MISSING and leave
 * the output pointer NULL.
 */
enum sql_stats_lookup_status
sql_stats_snapshot_relation_at(const struct sql_stats_snapshot *snapshot,
			       size_t ordinal,
			       const struct sql_stats_relation **relation);

/*
 * Combine disjoint immutable candidates from the same catalog/schema
 * generation, or replace one relation from that generation. These helpers
 * deep-copy a new detached snapshot; inputs remain unchanged. Temporary and
 * resulting allocations are independently bounded by max_bytes.
 */
struct sql_stats_snapshot *
sql_stats_snapshot_combine(const struct sql_stats_snapshot *const *snapshots,
			   size_t snapshot_count, size_t max_bytes);

struct sql_stats_snapshot *
sql_stats_snapshot_replace_relation(const struct sql_stats_snapshot *base,
				    const struct sql_stats_snapshot *replacement,
				    uint32_t space_id, size_t max_bytes);

size_t
sql_stats_relation_index_count(const struct sql_stats_relation *relation);

enum sql_stats_lookup_status
sql_stats_relation_index_at(const struct sql_stats_relation *relation,
			    size_t ordinal,
			    const struct sql_stats_index **index);

enum sql_stats_lookup_status
sql_stats_snapshot_get_relation(const struct sql_stats_snapshot *snapshot,
				uint64_t current_schema_version,
				uint32_t space_id,
				const struct sql_stats_relation **relation);

enum sql_stats_lookup_status
sql_stats_relation_get_index(const struct sql_stats_relation *relation,
			     uint32_t index_id,
			     const struct sql_stats_index **index);

uint32_t
sql_stats_relation_space_id(const struct sql_stats_relation *relation);

double
sql_stats_relation_row_count(const struct sql_stats_relation *relation);

const char *
sql_stats_relation_population_basis(const struct sql_stats_relation *relation);

double
sql_stats_relation_average_row_width(const struct sql_stats_relation *relation);

const char *
sql_stats_relation_width_basis(const struct sql_stats_relation *relation);

uint64_t
sql_stats_relation_width_denominator_count(const struct sql_stats_relation *relation);

double
sql_stats_relation_confidence(const struct sql_stats_relation *relation);

const char *
sql_stats_relation_confidence_source(const struct sql_stats_relation *relation);

enum sql_stats_cardinality_semantics
sql_stats_relation_cardinality_semantics(
	const struct sql_stats_relation *relation);

uint64_t
sql_stats_relation_collected_at(const struct sql_stats_relation *relation);

uint64_t
sql_stats_relation_modification_epoch(const struct sql_stats_relation *relation);

uint64_t
sql_stats_relation_visibility_id(const struct sql_stats_relation *relation);

uint32_t
sql_stats_index_id(const struct sql_stats_index *index);

uint64_t
sql_stats_index_tuple_count(const struct sql_stats_index *index);

enum sql_stats_cardinality_semantics
sql_stats_index_tuple_count_semantics(const struct sql_stats_index *index);

const char *
sql_stats_index_population_basis(const struct sql_stats_index *index);

const char *
sql_stats_index_ndv_basis(const struct sql_stats_index *index);

uint64_t
sql_stats_index_definition_version(const struct sql_stats_index *index);

size_t
sql_stats_index_prefix_count(const struct sql_stats_index *index);

uint64_t
sql_stats_index_distinct_prefix(const struct sql_stats_index *index,
				size_t prefix_index);

uint64_t
sql_stats_index_part_sample_rows(const struct sql_stats_index *index,
				 size_t part_index);

uint64_t
sql_stats_index_part_sample_nonnull_rows(const struct sql_stats_index *index,
						 size_t part_index);

size_t
sql_stats_index_part_mcv_count(const struct sql_stats_index *index,
				       size_t part_index);

/* Value bytes are borrowed from the immutable snapshot. */
enum sql_stats_lookup_status
sql_stats_index_part_mcv_at(const struct sql_stats_index *index,
			    size_t part_index, size_t ordinal,
			    uint8_t *type_tag, const void **value,
			    size_t *value_size, uint64_t *estimate,
			    uint64_t *error);

size_t
sql_stats_index_part_histogram_count(const struct sql_stats_index *index,
				     size_t part_index);

/* Boundary bytes are borrowed from the immutable snapshot. */
enum sql_stats_lookup_status
sql_stats_index_part_histogram_at(const struct sql_stats_index *index,
				  size_t part_index, size_t ordinal,
				  uint8_t *type_tag, const void **upper_bound,
				  size_t *upper_bound_size,
				  uint64_t *cumulative_count);

/* Look up one tracked typed MCV and scale its SpaceSaving estimate/error
 * interval from the sample domain to the index tuple population. An absent
 * candidate remains MISSING (not zero); stale snapshots remain STALE. */
enum sql_stats_lookup_status
sql_stats_snapshot_estimate_index_part_mcv_rows(
	const struct sql_stats_snapshot *snapshot, uint64_t current_schema_version,
	uint32_t space_id, uint32_t index_id, size_t part_index,
	uint8_t type_tag, const void *value, size_t value_size,
	double *estimated_rows, double *error_rows);

/**
 * Estimate the average row count for a relation/index prefix. A zero
 * prefix_count returns relation cardinality; otherwise prefix_count is the
 * number of leading index parts constrained by equality and the average is
 * based on that index's tuple count divided by its distinct-prefix count.
 * The output is written only when the returned status is AVAILABLE.
 * Missing/stale inputs are returned as lookup statuses so callers can
 * preserve legacy estimates.
 */
enum sql_stats_lookup_status
sql_stats_snapshot_estimate_index_prefix_rows(
	const struct sql_stats_snapshot *snapshot, uint64_t current_schema_version,
	uint32_t space_id, uint32_t index_id, uint32_t prefix_count,
	double *rows);

#endif /* TARANTOOL_SQL_STATS_SNAPSHOT_H */
