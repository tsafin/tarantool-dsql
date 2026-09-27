#include "box/sql/sql_stats_collection.h"
#include "box/sql/sql_stats_index_summary.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unit.h"

static int
extract_index_value(void *context, const char *tuple, size_t tuple_size,
		    const uint32_t *field_ids, size_t field_count,
		    struct sql_stats_hll_value *parts, size_t part_count)
{
	(void)context;
	(void)field_ids;
	(void)field_count;
	if (tuple_size == 0 || part_count != 1)
		return -1;
	parts[0] = (struct sql_stats_hll_value){1, tuple, tuple_size};
	return 0;
}

static void
test_sample_to_candidate(void)
{
	plan(4);
	header();
	struct sql_stats_index_summary *summary = sql_stats_index_summary_new(
		1, 12, 23, 8192, extract_index_value, NULL);
	ok(summary != NULL, "sample-to-candidate index sketch allocates");
	fail_if(summary == NULL);
	char values[63][4];
	for (int i = 0; i < 63; i++)
		snprintf(values[i], sizeof(values[i]), "v%02d", i);
	bool consumed = true;
	for (int i = 0; i < 100; i++) {
		const char *value = values[i < 63 ? i : i - 63];
		if (sql_stats_index_summary_consume(summary, value, strlen(value),
						   NULL, 0) != 0)
			consumed = false;
	}
	struct sql_stats_sample_result sample = {
		.rows = 100, .bytes = 300, .population_known = true,
		.visible_population = 1000, .with_replacement = true,
	};
	struct sql_stats_expected_index expected_index = {
		.index_id = 8, .definition_version = 3, .part_count = 1,
	};
	uint64_t prefixes[1] = {0};
	struct sql_stats_collected_index index = {0};
	double index_confidence = 0;
	bool converted = consumed &&
		sql_stats_collection_index_from_sample(&expected_index, &sample,
			summary, 9, prefixes, 1, &index, &index_confidence,
			64, 1000000) == 0 && index.tuple_count == 1000 &&
		index.visibility_id == 9 && index.definition_version == 3 &&
		index.prefix_count == 1 && prefixes[0] >= 90 && prefixes[0] <= 110 &&
		strcmp(index.population_basis,
		       "visible-engine-index-count-v1") == 0 &&
		strcmp(index.ndv_basis, index.population_basis) == 0 &&
		index_confidence > 0;
	ok(converted,
	   "sample conversion derives index population, prefix NDV, provenance, and confidence");
	struct sql_stats_collected_population population;
	struct sql_stats_collected_width width;
	bool relation_facts = sql_stats_collection_population_from_sample(
		&sample, &population) && sql_stats_collection_width_from_sample(
		&sample, &width);
	ok(relation_facts && population.row_count == 1000 &&
	   width.denominator_rows == 100 && fabs(width.average_bytes - 3.0) < 1e-9,
	   "same sampled primary population derives exact rows and sampled width");
	struct sql_stats_expected_relation expected_relation = {
		.space_id = 42, .modification_epoch = 11,
		.indexes = &expected_index, .index_count = 1,
	};
	struct sql_stats_collection_generation generation = {
		.catalog_version = 4, .schema_version = 7, .visibility_id = 9,
	};
	struct sql_stats_collected_relation relation = {
		.space_id = 42, .catalog_version = 4, .schema_version = 7,
		.visibility_id = 9, .modification_epoch = 11,
		.row_count = (double)population.row_count,
		.cardinality_semantics = population.semantics,
		.population_basis = index.population_basis,
		.average_row_width = width.average_bytes,
		.width_basis = "sampled-serialized-tuple-bytes-v1",
		.width_denominator_count = width.denominator_rows,
		.confidence = index_confidence,
		.confidence_source = SQL_STATS_INDEX_NDV_CONFIDENCE_SOURCE,
		.collected_at = 12, .indexes = &index, .index_count = 1,
	};
	struct sql_stats_collection_result result = {
		.generation = generation, .relations = &relation, .relation_count = 1,
	};
	struct sql_stats_snapshot *candidate =
		sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
						     &result, 4096);
	const struct sql_stats_relation *saved_relation = NULL;
	const struct sql_stats_index *saved_index = NULL;
	bool built = candidate != NULL && sql_stats_snapshot_get_relation(
		candidate, 7, 42, &saved_relation) == SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_relation_get_index(saved_relation, 8, &saved_index) ==
		SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_index_distinct_prefix(saved_index, 0) == prefixes[0] &&
		sql_stats_relation_confidence(saved_relation) == index_confidence;
	ok(built, "sample-derived summaries build one immutable candidate snapshot");
	if (candidate != NULL)
		sql_stats_snapshot_release(candidate);
	sql_stats_index_summary_delete(summary);
	footer();
	check_plan();
}

int
main(void)
{
	test_sample_to_candidate();
	return 0;
}
