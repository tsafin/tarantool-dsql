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
	plan(7);
	header();
	struct sql_stats_index_summary *summary =
		sql_stats_index_summary_new_with_mcv(1, 12, 23, 8192,
			extract_index_value, NULL, 4, 16);
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
	struct sql_stats_expected_index expected_indexes[] = {
		expected_index,
		{.index_id = 9, .definition_version = 4, .part_count = 1},
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
		.indexes = expected_indexes, .index_count = 2,
	};
	struct sql_stats_collection_generation generation = {
		.catalog_version = 4, .schema_version = 7, .visibility_id = 9,
	};
	struct sql_stats_index_summary *second_summary = sql_stats_index_summary_new(
		1, 12, 23, 8192, extract_index_value, NULL);
	bool second_consumed = second_summary != NULL;
	for (int i = 0; second_consumed && i < 100; i++) {
		const char *value = values[i < 63 ? i : i - 63];
		second_consumed = sql_stats_index_summary_consume(second_summary,
								 value, strlen(value),
								 NULL, 0) == 0;
	}
	struct sql_stats_sampled_index sampled_indexes[] = {
		{&expected_indexes[0], &sample, summary},
		{&expected_indexes[1], &sample, second_summary},
	};
	double per_index_confidences[] = {-1.0, -1.0};
	struct sql_stats_snapshot *candidate = second_consumed ?
		sql_stats_collection_build_sample_candidate(&generation,
		&expected_relation, sampled_indexes, 2, &sample,
			index_confidence, SQL_STATS_INDEX_NDV_CONFIDENCE_SOURCE,
			per_index_confidences, 4096, 64, 1000000) : NULL;
	const struct sql_stats_relation *saved_relation = NULL;
	const struct sql_stats_index *saved_index = NULL;
	const struct sql_stats_index *saved_index2 = NULL;
	uint8_t saved_mcv_tag = 0;
	const void *saved_mcv_value = NULL;
	size_t saved_mcv_size = 0;
	uint64_t saved_mcv_estimate = 0, saved_mcv_error = 0;
	bool built = candidate != NULL && sql_stats_snapshot_get_relation(
		candidate, 7, 42, &saved_relation) == SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_relation_get_index(saved_relation, 8, &saved_index) ==
		SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_relation_get_index(saved_relation, 9, &saved_index2) ==
		SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_index_distinct_prefix(saved_index, 0) == prefixes[0] &&
		sql_stats_index_distinct_prefix(saved_index2, 0) >= 90 &&
		sql_stats_index_distinct_prefix(saved_index2, 0) <= 110 &&
		sql_stats_relation_confidence(saved_relation) == index_confidence &&
		per_index_confidences[0] == index_confidence &&
		per_index_confidences[1] > 0;
	ok(built,
	   "multiple sampled indexes become one complete detached candidate");
	bool mcv_built = built &&
		sql_stats_index_part_sample_nonnull_rows(saved_index, 0) == 100 &&
		sql_stats_index_part_mcv_count(saved_index, 0) != 0 &&
		sql_stats_index_part_mcv_at(saved_index, 0, 0, &saved_mcv_tag,
			&saved_mcv_value, &saved_mcv_size, &saved_mcv_estimate,
			&saved_mcv_error) == SQL_STATS_LOOKUP_AVAILABLE &&
		saved_mcv_tag == 1 && saved_mcv_size != 0 &&
		saved_mcv_estimate >= saved_mcv_error;
	ok(mcv_built, "sampled MCV bounds reach the immutable candidate snapshot");
	struct sql_stats_sample_result empty_sample = {
		.population_known = true, .visible_population = 0,
		.with_replacement = true,
	};
	struct sql_stats_index_summary *empty_summary1 =
		sql_stats_index_summary_new(1, 12, 23, 8192,
					    extract_index_value, NULL);
	struct sql_stats_index_summary *empty_summary2 =
		sql_stats_index_summary_new(1, 12, 24, 8192,
					    extract_index_value, NULL);
	struct sql_stats_sampled_index empty_indexes[] = {
		{&expected_indexes[0], &empty_sample, empty_summary1},
		{&expected_indexes[1], &empty_sample, empty_summary2},
	};
	double empty_confidences[] = {-1.0, -1.0};
	struct sql_stats_snapshot *empty_candidate = empty_summary1 != NULL &&
		empty_summary2 != NULL ? sql_stats_collection_build_sample_candidate(
			&generation, &expected_relation, empty_indexes, 2,
			&empty_sample, 0.0, "empty-census-v1", empty_confidences,
			4096, 64, 1000000) : NULL;
	const struct sql_stats_relation *empty_relation = NULL;
	const struct sql_stats_index *empty_index1 = NULL;
	const struct sql_stats_index *empty_index2 = NULL;
	bool empty_built = empty_candidate != NULL &&
		sql_stats_snapshot_get_relation(empty_candidate, 7, 42,
						&empty_relation) ==
		SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_relation_row_count(empty_relation) == 0 &&
		sql_stats_relation_average_row_width(empty_relation) == 0 &&
		sql_stats_relation_get_index(empty_relation, 8, &empty_index1) ==
		SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_relation_get_index(empty_relation, 9, &empty_index2) ==
		SQL_STATS_LOOKUP_AVAILABLE &&
		sql_stats_index_distinct_prefix(empty_index1, 0) == 0 &&
		sql_stats_index_distinct_prefix(empty_index2, 0) == 0;
	ok(empty_built,
	   "empty sampled population builds exact zero summaries without width");
	if (empty_candidate != NULL)
		sql_stats_snapshot_release(empty_candidate);
	sql_stats_index_summary_delete(empty_summary1);
	sql_stats_index_summary_delete(empty_summary2);
	struct sql_stats_sample_result inconsistent_sample = sample;
	inconsistent_sample.visible_population++;
	struct sql_stats_sampled_index inconsistent_indexes[] = {
		{&expected_indexes[0], &sample, summary},
		{&expected_indexes[1], &inconsistent_sample, second_summary},
	};
	per_index_confidences[0] = -1.0;
	per_index_confidences[1] = -1.0;
	struct sql_stats_snapshot *failed =
		sql_stats_collection_build_sample_candidate(&generation,
			&expected_relation, inconsistent_indexes, 2, &sample,
			index_confidence, SQL_STATS_INDEX_NDV_CONFIDENCE_SOURCE,
			per_index_confidences, 4096, 64, 1000000);
	ok(failed == NULL && candidate != NULL &&
	   per_index_confidences[0] == -1.0 &&
	   per_index_confidences[1] == -1.0,
	   "population mismatch preserves prior candidate and confidence outputs");
	if (failed != NULL)
		sql_stats_snapshot_release(failed);
	if (candidate != NULL)
		sql_stats_snapshot_release(candidate);
	sql_stats_index_summary_delete(second_summary);
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
