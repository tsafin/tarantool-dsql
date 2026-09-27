#include "box/sql/sql_stats_snapshot.h"

#include <math.h>

#include "unit.h"

static void
test_deep_copy_lookup_and_lifetime(void)
{
	plan(28);
	header();
	uint64_t prefixes[] = {2, 5};
	uint64_t sparse_prefixes[] = {2, 4};
	struct sql_stats_index_input indexes[] = {
		{.index_id = 8, .tuple_count = 10, .distinct_prefixes = prefixes,
		 .prefix_count = 2},
		{.index_id = 9, .tuple_count = 6,
		 .distinct_prefixes = sparse_prefixes, .prefix_count = 2},
	};
	struct sql_stats_relation_input relations[] = {
		{.space_id = 42, .row_count = 10, .average_row_width = 24,
		 .confidence = 0.9,
		 .cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		 .collected_at = 11, .modification_epoch = 12,
		 .indexes = indexes, .index_count = 2},
	};
	struct sql_stats_snapshot *snapshot = sql_stats_snapshot_new(4, 7,
		relations, 1, 4096);
	ok(snapshot != NULL, "valid snapshot created");
	if (snapshot == NULL) {
		footer();
		check_plan();
		return;
	}
	ok(sql_stats_snapshot_api_version(snapshot) == 2,
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
	const struct sql_stats_relation *ordinal_relation = NULL;
	ok(sql_stats_snapshot_relation_at(snapshot, 0, &ordinal_relation) ==
	   SQL_STATS_LOOKUP_AVAILABLE && ordinal_relation == relation,
	   "snapshot exposes borrowed ordered relation iteration");
	ok(sql_stats_snapshot_relation_at(snapshot, 1, &ordinal_relation) ==
	   SQL_STATS_LOOKUP_MISSING && ordinal_relation == NULL,
	   "out-of-range relation iteration fails closed");
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
	ok(sql_stats_relation_index_count(relation) == 2 &&
	   sql_stats_relation_index_at(relation, 0, &index) ==
		   SQL_STATS_LOOKUP_AVAILABLE &&
	   sql_stats_index_id(index) == 8,
	   "relation exposes borrowed ordered index iteration");
	ok(sql_stats_index_tuple_count(index) == 10 &&
	   sql_stats_index_prefix_count(index) == 2 &&
	   sql_stats_index_distinct_prefix(index, 0) == 2 &&
	   sql_stats_index_distinct_prefix(index, 1) == 5,
	   "index prefixes are retained independently");
	double rows = -1;
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 7, 42, 8,
							 0, &rows) ==
	   SQL_STATS_LOOKUP_AVAILABLE && rows == 10,
	   "zero-length prefix uses relation cardinality");
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 7, 42, 8,
							 1, &rows) ==
	   SQL_STATS_LOOKUP_AVAILABLE && rows == 5,
	   "first key prefix estimates average rows from its NDV");
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 7, 42, 9,
							 1, &rows) ==
	   SQL_STATS_LOOKUP_AVAILABLE && rows == 3,
	   "sparse index prefix uses its own tuple population");
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 7, 42, 9,
							 2, &rows) ==
	   SQL_STATS_LOOKUP_AVAILABLE && rows == 1.5,
	   "sparse full prefix uses index population rather than relation rows");
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 7, 42, 8,
							 2, &rows) ==
	   SQL_STATS_LOOKUP_AVAILABLE && rows == 2,
	   "full key prefix estimates average rows from its NDV");
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 8, 42, 8,
							 1, &rows) ==
	   SQL_STATS_LOOKUP_STALE,
	   "stale schema rejects index-prefix estimate");
	rows = 123;
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 8, 42, 8,
							 1, &rows) ==
	   SQL_STATS_LOOKUP_STALE && rows == 123,
	   "stale lookup leaves caller estimate untouched for fallback");
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 7, 42, 10,
							 1, &rows) ==
	   SQL_STATS_LOOKUP_MISSING && rows == 123,
	   "missing index leaves caller estimate untouched for fallback");
	ok(sql_stats_snapshot_estimate_index_prefix_rows(snapshot, 7, 42, 8,
							 3, &rows) ==
	   SQL_STATS_LOOKUP_MISSING,
	   "prefix beyond captured definition rejects estimate");
	ok(sql_stats_snapshot_get_relation(snapshot, 8, 42, &relation) ==
	   SQL_STATS_LOOKUP_STALE && relation == NULL,
	   "schema mismatch reports stale without relation");
	ok(sql_stats_snapshot_get_relation(snapshot, 7, 43, &relation) ==
	   SQL_STATS_LOOKUP_MISSING && relation == NULL,
	   "unknown relation reports missing");
	struct sql_stats_relation_input second_input = {
		.space_id = 43, .row_count = 4,
		.cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
	};
	struct sql_stats_snapshot *second = sql_stats_snapshot_new(4, 7,
		&second_input, 1, 4096);
	const struct sql_stats_snapshot *parts[] = {snapshot, second};
	struct sql_stats_snapshot *combined = second != NULL ?
		sql_stats_snapshot_combine(parts, 2, 8192) : NULL;
	ok(combined != NULL &&
	   sql_stats_snapshot_relation_count(combined) == 2,
	   "same-generation disjoint snapshots combine atomically");
	struct sql_stats_relation_input replacement_input = {
		.space_id = 42, .row_count = 11,
		.cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
	};
	struct sql_stats_snapshot *replacement = sql_stats_snapshot_new(4, 7,
		&replacement_input, 1, 4096);
	struct sql_stats_snapshot *replaced = combined != NULL && replacement != NULL ?
		sql_stats_snapshot_replace_relation(combined, replacement, 42, 8192) :
		NULL;
	const struct sql_stats_relation *replaced_relation = NULL;
	const struct sql_stats_relation *preserved_relation = NULL;
	ok(replaced != NULL &&
	   sql_stats_snapshot_get_relation(replaced, 7, 42,
					   &replaced_relation) ==
		   SQL_STATS_LOOKUP_AVAILABLE &&
	   sql_stats_relation_row_count(replaced_relation) == 11 &&
	   sql_stats_snapshot_get_relation(replaced, 7, 43,
					   &preserved_relation) ==
		   SQL_STATS_LOOKUP_AVAILABLE &&
	   sql_stats_relation_row_count(preserved_relation) == 4,
	   "same-generation replacement changes one relation and preserves others");
	struct sql_stats_snapshot *wrong_generation = sql_stats_snapshot_new(5, 7,
		&replacement_input, 1, 4096);
	const struct sql_stats_snapshot *wrong_parts[] = {snapshot,
		wrong_generation};
	ok(sql_stats_snapshot_combine(wrong_parts, 2, 8192) == NULL,
	   "combine rejects catalog-generation mismatch");
	const struct sql_stats_snapshot *duplicate_parts[] = {snapshot, snapshot};
	ok(sql_stats_snapshot_combine(duplicate_parts, 2, 8192) == NULL,
	   "combine rejects duplicate relation ownership");
	sql_stats_snapshot_release(wrong_generation);
	sql_stats_snapshot_release(replaced);
	sql_stats_snapshot_release(replacement);
	sql_stats_snapshot_release(combined);
	sql_stats_snapshot_release(second);
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
	plan(8);
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
	uint64_t empty_prefix = 0;
	index.tuple_count = 0;
	index.distinct_prefixes = &empty_prefix;
	relation.row_count = 0;
	struct sql_stats_snapshot *empty_snapshot = sql_stats_snapshot_new(1, 1,
		&relation, 1, 4096);
	ok(empty_snapshot != NULL,
	   "zero prefix NDV is accepted for an empty index population");
	sql_stats_snapshot_release(empty_snapshot);
	index.tuple_count = 3;
	relation.row_count = 3;
	ok(sql_stats_snapshot_new(1, 1, &relation, 1, 4096) == NULL,
	   "zero prefix NDV remains invalid for a nonempty index");
	footer();
	check_plan();
}

static void
test_allocation_failures_roll_back(void)
{
	plan(1);
	header();
	uint64_t prefixes[] = {2, 4};
	struct sql_stats_index_input index = {
		.index_id = 8, .tuple_count = 10,
		.population_basis = "visible@view", .ndv_basis = "visible@view",
		.distinct_prefixes = prefixes, .prefix_count = 2,
	};
	struct sql_stats_relation_input relation = {
		.space_id = 42, .row_count = 10, .population_basis = "visible@view",
		.average_row_width = 24, .width_basis = "payload/rows",
		.width_denominator_count = 10, .confidence = 1,
		.confidence_source = "exact", .cardinality_semantics =
			SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.indexes = &index, .index_count = 1,
	};
	unsigned int failures = 0;
	bool complete = false;
	for (long fail_after = 0; fail_after < 32; fail_after++) {
		sql_stats_snapshot_test_fail_allocation_after(fail_after);
		struct sql_stats_snapshot *snapshot = sql_stats_snapshot_new(1, 1,
			&relation, 1, 4096);
		if (snapshot == NULL) {
			failures++;
			continue;
		}
		sql_stats_snapshot_test_fail_allocation_after(-1);
		sql_stats_snapshot_release(snapshot);
		complete = true;
		break;
	}
	ok(failures >= 9 && complete,
	   "each snapshot allocation failure rolls back before complete success");
	footer();
	check_plan();
}

int
main(void)
{
	test_deep_copy_lookup_and_lifetime();
	test_reject_invalid_inputs();
	test_allocation_failures_roll_back();
	return 0;
}
