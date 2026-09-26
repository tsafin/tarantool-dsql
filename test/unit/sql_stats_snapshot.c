#include "box/sql/sql_stats_snapshot.h"

#include <math.h>

#include "unit.h"

static void
test_deep_copy_lookup_and_lifetime(void)
{
	plan(12);
	header();
	uint64_t prefixes[] = {2, 5};
	struct sql_stats_index_input indexes[] = {
		{.index_id = 8, .tuple_count = 10, .distinct_prefixes = prefixes,
		 .prefix_count = 2},
	};
	struct sql_stats_relation_input relations[] = {
		{.space_id = 42, .row_count = 10, .average_row_width = 24,
		 .confidence = 0.9,
		 .cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		 .collected_at = 11, .modification_epoch = 12,
		 .indexes = indexes, .index_count = 1},
	};
	struct sql_stats_snapshot *snapshot = sql_stats_snapshot_new(4, 7,
		relations, 1, 4096);
	ok(snapshot != NULL, "valid snapshot created");
	if (snapshot == NULL) {
		footer();
		check_plan();
		return;
	}
	ok(sql_stats_snapshot_api_version(snapshot) == 1,
	   "snapshot API version is explicit");
	ok(sql_stats_snapshot_catalog_version(snapshot) == 4 &&
	   sql_stats_snapshot_schema_version(snapshot) == 7,
	   "catalog and schema versions retained");
	ok(sql_stats_snapshot_bytes(snapshot) <= 4096,
	   "snapshot allocation respects byte budget");
	prefixes[0] = 9;
	relations[0].row_count = 999;
	const struct sql_stats_relation *relation = NULL;
	ok(sql_stats_snapshot_get_relation(snapshot, 7, 42, &relation) ==
	   SQL_STATS_LOOKUP_AVAILABLE, "matching schema finds relation");
	ok(sql_stats_relation_row_count(relation) == 10,
	   "relation data is deep-copied");
	ok(sql_stats_relation_average_row_width(relation) == 24 &&
	   sql_stats_relation_confidence(relation) == 0.9 &&
	   sql_stats_relation_collected_at(relation) == 11 &&
	   sql_stats_relation_modification_epoch(relation) == 12,
	   "relation metadata retained");
	const struct sql_stats_index *index = NULL;
	ok(sql_stats_relation_get_index(relation, 8, &index) ==
	   SQL_STATS_LOOKUP_AVAILABLE, "index lookup succeeds");
	ok(sql_stats_index_tuple_count(index) == 10 &&
	   sql_stats_index_prefix_count(index) == 2 &&
	   sql_stats_index_distinct_prefix(index, 0) == 2 &&
	   sql_stats_index_distinct_prefix(index, 1) == 5,
	   "index prefixes are retained independently");
	ok(sql_stats_snapshot_get_relation(snapshot, 8, 42, &relation) ==
	   SQL_STATS_LOOKUP_STALE && relation == NULL,
	   "schema mismatch reports stale without relation");
	ok(sql_stats_snapshot_get_relation(snapshot, 7, 43, &relation) ==
	   SQL_STATS_LOOKUP_MISSING && relation == NULL,
	   "unknown relation reports missing");
	sql_stats_snapshot_retain(snapshot);
	sql_stats_snapshot_release(snapshot);
	ok(sql_stats_snapshot_relation_count(snapshot) == 1,
	   "retained snapshot remains alive");
	sql_stats_snapshot_release(snapshot);
	footer();
	check_plan();
}

static void
test_reject_invalid_inputs(void)
{
	plan(6);
	header();
	struct sql_stats_relation_input relation = {
		.space_id = 1, .row_count = NAN, .average_row_width = 1,
		.confidence = 1,
		.cardinality_semantics = SQL_STATS_CARDINALITY_ESTIMATE,
	};
	ok(sql_stats_snapshot_new(1, 1, &relation, 1, 4096) == NULL,
	   "NaN cardinality rejected");
	relation.row_count = 1;
	relation.confidence = 1.1;
	ok(sql_stats_snapshot_new(1, 1, &relation, 1, 4096) == NULL,
	   "confidence outside range rejected");
	relation.confidence = 1;
	ok(sql_stats_snapshot_new(1, 1, &relation, 1, 1) == NULL,
	   "insufficient byte budget rejected");
	ok(sql_stats_snapshot_new(1, 1, NULL, 1, 4096) == NULL,
	   "missing relation input rejected");
	struct sql_stats_index_input index = {
		.index_id = 1, .tuple_count = 3, .distinct_prefixes = NULL,
		.prefix_count = 1,
	};
	relation.indexes = &index;
	relation.index_count = 1;
	ok(sql_stats_snapshot_new(1, 1, &relation, 1, 4096) == NULL,
	   "missing prefix vector rejected");
	uint64_t bad_prefix = 4;
	index.distinct_prefixes = &bad_prefix;
	ok(sql_stats_snapshot_new(1, 1, &relation, 1, 4096) == NULL,
	   "distinct count exceeding tuple count rejected");
	footer();
	check_plan();
}

int
main(void)
{
	test_deep_copy_lookup_and_lifetime();
	test_reject_invalid_inputs();
	return 0;
}
