#include "box/sql/sql_stats_collection.h"

#include <math.h>
#include <string.h>

#include "unit.h"

static void
test_population_from_engine_sample(void)
{
	plan(9);
	header();
	struct sql_stats_collected_population population;
	struct sql_stats_sample_result sample = {
		.rows = 8, .bytes = 256, .population_known = true,
		.visible_population = 20, .with_replacement = true,
	};
	ok(sql_stats_collection_population_from_sample(&sample, &population) &&
	   population.row_count == 20 &&
	   population.semantics == SQL_STATS_CARDINALITY_VISIBLE_ROWS,
	   "memtx sample draws retain exact visible population, not draw count");
	struct sql_stats_collected_width width;
	ok(sql_stats_collection_width_from_sample(&sample, &width) &&
	   width.average_bytes == 32 && width.denominator_rows == 8,
	   "sample bytes produce a row-count-denominated mean serialized width");
	struct sql_stats_sample_result fractional = {
		.rows = 3, .bytes = 10, .population_known = true,
		.visible_population = 20, .with_replacement = true,
	};
	ok(sql_stats_collection_width_from_sample(&fractional, &width) &&
	   fabs(width.average_bytes - 10.0 / 3.0) < 1e-12 &&
	   width.denominator_rows == 3,
	   "fractional sample-average width is not rounded down");
	sample.bytes = 7;
	ok(!sql_stats_collection_width_from_sample(&sample, &width),
	   "sample byte count cannot be smaller than its tuple count");
	sample.bytes = 256;
	sample.with_replacement = false;
	ok(sql_stats_collection_population_from_sample(&sample, &population),
	   "exhaustive Vinyl population converts when sampled rows fit population");
	sample.rows = 21;
	ok(!sql_stats_collection_population_from_sample(&sample, &population),
	   "without-replacement rows cannot exceed visible population");
	sample.rows = 0;
	sample.visible_population = 0;
	ok(sql_stats_collection_population_from_sample(&sample, &population) &&
	   population.row_count == 0,
	   "empty visible population is a valid exact count");
	ok(!sql_stats_collection_width_from_sample(&sample, &width),
	   "empty sample does not invent a row-width estimate");
	sample.population_known = false;
	ok(!sql_stats_collection_population_from_sample(&sample, &population),
	   "unknown engine population cannot become exact relation count");
	footer();
	check_plan();
}

static void
test_complete_result_and_rejections(void)
{
	plan(24);
	header();
	uint64_t prefixes[] = {2, 4};
	struct sql_stats_expected_index expected_index = {
		.index_id = 8, .definition_version = 3, .part_count = 2,
	};
	struct sql_stats_expected_relation expected_relation = {
		.space_id = 42, .modification_epoch = 11,
		.indexes = &expected_index, .index_count = 1,
	};
	struct sql_stats_collection_generation generation = {
		.catalog_version = 4, .schema_version = 7, .visibility_id = 9,
	};
	struct sql_stats_collected_index index = {
		.index_id = 8, .definition_version = 3, .visibility_id = 9,
		.tuple_count = 10,
		.tuple_count_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.population_basis = "visible_rows@view-9", .ndv_basis = "visible_rows@view-9",
		.distinct_prefixes = prefixes, .prefix_count = 2,
	};
	struct sql_stats_collected_relation relation = {
		.space_id = 42, .catalog_version = 4, .schema_version = 7,
		.visibility_id = 9, .modification_epoch = 11, .row_count = 10,
		.cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.population_basis = "visible_rows@view-9", .average_row_width = 24,
		.width_basis = "sampled_payload_bytes/sample_rows",
		.width_denominator_count = 8,
		.confidence = 0.75, .confidence_source = "caller-calibrated-v1",
		.collected_at = 12, .indexes = &index, .index_count = 1,
	};
	struct sql_stats_collection_result result = {
		.generation = generation, .relations = &relation, .relation_count = 1,
	};
	struct sql_stats_snapshot *snapshot = sql_stats_collection_build_candidate(
		&generation, &expected_relation, 1, &result, 4096);
	ok(snapshot != NULL, "complete matching collection builds candidate");
	if (snapshot != NULL) {
		const struct sql_stats_relation *r = NULL;
		const struct sql_stats_index *i = NULL;
		ok(sql_stats_snapshot_get_relation(snapshot, 7, 42, &r) ==
		   SQL_STATS_LOOKUP_AVAILABLE, "candidate contains relation");
		ok(strcmp(sql_stats_relation_width_basis(r),
			  "sampled_payload_bytes/sample_rows") == 0 &&
		   strcmp(sql_stats_relation_population_basis(r),
			  "visible_rows@view-9") == 0 &&
		   sql_stats_relation_width_denominator_count(r) == 8 &&
		   strcmp(sql_stats_relation_confidence_source(r),
			  "caller-calibrated-v1") == 0 &&
		   sql_stats_relation_visibility_id(r) == 9,
		   "relation provenance and visibility token deep-copied");
		ok(sql_stats_relation_get_index(r, 8, &i) == SQL_STATS_LOOKUP_AVAILABLE &&
		   sql_stats_index_definition_version(i) == 3 &&
		   sql_stats_index_tuple_count_semantics(i) ==
			SQL_STATS_CARDINALITY_VISIBLE_ROWS &&
		   strcmp(sql_stats_index_population_basis(i), "visible_rows@view-9") == 0 &&
		   strcmp(sql_stats_index_ndv_basis(i), "visible_rows@view-9") == 0,
		   "index population and NDV provenance deep-copied");
		prefixes[0] = 99;
		ok(sql_stats_index_distinct_prefix(i, 0) == 2,
		   "candidate owns copied prefix values");
		sql_stats_snapshot_release(snapshot);
	} else {
		ok(false, "candidate contains relation");
		ok(false, "relation provenance and visibility token deep-copied");
		ok(false, "index population and NDV provenance deep-copied");
		ok(false, "candidate owns copied prefix values");
	}
	prefixes[0] = 2;
	unsigned int staging_failures = 0;
	bool complete_candidate = false;
	for (long fail_after = 0; fail_after < 4; fail_after++) {
		sql_stats_collection_test_fail_allocation_after(fail_after);
		struct sql_stats_snapshot *candidate =
			sql_stats_collection_build_candidate(&generation,
				&expected_relation, 1, &result, 4096);
		if (candidate == NULL) {
			staging_failures++;
			continue;
		}
		sql_stats_collection_test_fail_allocation_after(-1);
		sql_stats_snapshot_release(candidate);
		complete_candidate = true;
		break;
	}
	ok(staging_failures == 2 && complete_candidate,
	   "each collection staging allocation fails before candidate succeeds");
	relation.index_count = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing expected index is rejected");
	relation.index_count = 1;
	result.relation_count = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing expected relation is rejected");
	result.relation_count = 1;
	index.prefix_count = 1;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing leading-prefix NDV is rejected");
	index.prefix_count = 2;
	index.definition_version++;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "index-definition mismatch is rejected");
	index.definition_version--;
	index.visibility_id++;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "index visibility mismatch is rejected");
	index.visibility_id--;
	generation.schema_version++;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "expected generation mismatch is rejected");
	generation.schema_version--;
	index.population_basis = "physical-tuples";
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "mixed population bases are rejected");
	index.population_basis = "visible_rows@view-9";
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 1) == NULL, "candidate copy budget failure is rejected");
	bool saw_budget_rejection = false;
	bool reached_complete_candidate = false;
	for (size_t budget = 1; budget <= 4096; budget++) {
		struct sql_stats_snapshot *candidate =
			sql_stats_collection_build_candidate(&generation,
				&expected_relation, 1, &result, budget);
		if (candidate == NULL) {
			saw_budget_rejection = true;
			continue;
		}
		sql_stats_snapshot_release(candidate);
		reached_complete_candidate = true;
		break;
	}
	ok(saw_budget_rejection && reached_complete_candidate,
	   "budget sweep rejects incomplete copies then accepts a full candidate");
	index.visibility_id = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "unknown index visibility token is rejected");
	index.visibility_id = 9;
	index.definition_version = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "unknown index definition version is rejected");
	index.definition_version = 3;
	index.tuple_count_semantics = (enum sql_stats_cardinality_semantics)99;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "invalid index population semantics rejected");
	index.tuple_count_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS;
	relation.cardinality_semantics = (enum sql_stats_cardinality_semantics)99;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "invalid relation cardinality semantics rejected");
	relation.cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS;
	relation.width_denominator_count = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing width denominator count rejected");
	relation.width_denominator_count = 8;
	relation.row_count = NAN;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "non-finite relation count rejected");
	relation.row_count = -1;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "negative relation count rejected");
	relation.row_count = 10;
	struct sql_stats_expected_relation expected_relations[] = {
		expected_relation,
		{ .space_id = 43, .modification_epoch = 11,
		  .indexes = &expected_index, .index_count = 1 },
	};
	struct sql_stats_collected_relation duplicate_relations[] = {
		relation,
		relation,
	};
	struct sql_stats_collection_result duplicate_relation_result = {
		.generation = generation,
		.relations = duplicate_relations,
		.relation_count = 2,
	};
	ok(sql_stats_collection_build_candidate(&generation,
		&expected_relations[0], 2, &duplicate_relation_result, 4096) == NULL,
	   "duplicate collected relation IDs are rejected");
	struct sql_stats_expected_index expected_indexes[] = {
		expected_index,
		{ .index_id = 9, .definition_version = 3, .part_count = 2 },
	};
	struct sql_stats_collected_index duplicate_indexes[] = {
		index,
		index,
	};
	relation.indexes = duplicate_indexes;
	relation.index_count = 2;
	struct sql_stats_expected_relation two_indexes = {
		.space_id = 42,
		.modification_epoch = 11,
		.indexes = expected_indexes,
		.index_count = 2,
	};
	struct sql_stats_collection_result duplicate_index_result = {
		.generation = generation,
		.relations = &relation,
		.relation_count = 1,
	};
	ok(sql_stats_collection_build_candidate(&generation, &two_indexes, 1,
		&duplicate_index_result, 4096) == NULL,
	   "duplicate collected index IDs are rejected");
	footer();
	check_plan();
}

int
main(void)
{
	test_population_from_engine_sample();
	test_complete_result_and_rejections();
	return 0;
}
