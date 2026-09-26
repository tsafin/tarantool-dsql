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

struct sql_stats_index_input {
	uint32_t index_id;
	uint64_t tuple_count;
	const uint64_t *distinct_prefixes;
	size_t prefix_count;
};

struct sql_stats_relation_input {
	uint32_t space_id;
	double row_count;
	double average_row_width;
	double confidence;
	enum sql_stats_cardinality_semantics cardinality_semantics;
	/* Opaque units: the API stores but does not interpret these values. */
	uint64_t collected_at;
	uint64_t modification_epoch;
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
 * Snapshot API version is currently 1 and intentionally distinct from the
 * persistence payload/catalog/schema versions. `schema_version` mismatch at
 * lookup time reports STALE instead of making ordinary prepare fail.
 */
struct sql_stats_snapshot *
sql_stats_snapshot_new(uint64_t catalog_version, uint64_t schema_version,
		       const struct sql_stats_relation_input *relations,
		       size_t relation_count, size_t max_bytes);

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

double
sql_stats_relation_average_row_width(const struct sql_stats_relation *relation);

double
sql_stats_relation_confidence(const struct sql_stats_relation *relation);

enum sql_stats_cardinality_semantics
sql_stats_relation_cardinality_semantics(
	const struct sql_stats_relation *relation);

uint64_t
sql_stats_relation_collected_at(const struct sql_stats_relation *relation);

uint64_t
sql_stats_relation_modification_epoch(const struct sql_stats_relation *relation);

uint32_t
sql_stats_index_id(const struct sql_stats_index *index);

uint64_t
sql_stats_index_tuple_count(const struct sql_stats_index *index);

size_t
sql_stats_index_prefix_count(const struct sql_stats_index *index);

uint64_t
sql_stats_index_distinct_prefix(const struct sql_stats_index *index,
				size_t prefix_index);

/**
 * Estimate the average row count for a relation/index prefix. A zero
 * prefix_count returns relation cardinality; otherwise prefix_count is the
 * number of leading index parts constrained by equality and the average is
 * based on that index's tuple count divided by its distinct-prefix count.
 * Missing/stale inputs are returned as lookup statuses so callers can
 * preserve legacy estimates.
 */
enum sql_stats_lookup_status
sql_stats_snapshot_estimate_index_prefix_rows(
	const struct sql_stats_snapshot *snapshot, uint64_t current_schema_version,
	uint32_t space_id, uint32_t index_id, uint32_t prefix_count,
	double *rows);

#endif /* TARANTOOL_SQL_STATS_SNAPSHOT_H */
