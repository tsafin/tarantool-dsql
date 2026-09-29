#include "sql_stats_collection.h"

#include <stdlib.h>
#include <string.h>

#ifdef SQL_STATS_COLLECTION_TESTING
static long test_allocations_before_failure = -1;

void
sql_stats_collection_test_fail_allocation_after(long successful_allocations)
{
	test_allocations_before_failure = successful_allocations;
}

static bool
test_should_fail_allocation(void)
{
	if (test_allocations_before_failure < 0)
		return false;
	if (test_allocations_before_failure == 0) {
		test_allocations_before_failure = -1;
		return true;
	}
	test_allocations_before_failure--;
	return false;
}
#else
#define test_should_fail_allocation() false
#endif

static void *
collection_calloc(size_t count, size_t size)
{
	return test_should_fail_allocation() ? NULL : calloc(count, size);
}

static bool
valid_tag(const char *tag)
{
	return tag != NULL && tag[0] != '\0';
}

static bool
valid_cardinality_semantics(enum sql_stats_cardinality_semantics semantics)
{
	return semantics >= SQL_STATS_CARDINALITY_VISIBLE_ROWS &&
	       semantics <= SQL_STATS_CARDINALITY_ESTIMATE;
}

bool
sql_stats_collection_population_from_sample(
	const struct sql_stats_sample_result *sample,
	struct sql_stats_collected_population *population)
{
	if (sample == NULL || population == NULL || !sample->population_known ||
	    (sample->visible_population == 0 && sample->rows != 0) ||
	    (!sample->with_replacement &&
	     sample->rows > sample->visible_population))
		return false;
	*population = (struct sql_stats_collected_population) {
		.row_count = sample->visible_population,
		.semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
	};
	return true;
}

bool
sql_stats_collection_width_from_sample(
	const struct sql_stats_sample_result *sample,
	struct sql_stats_collected_width *width)
{
	if (sample == NULL || width == NULL || !sample->population_known ||
	    sample->rows == 0 || sample->bytes < sample->rows ||
	    (sample->visible_population == 0 && sample->rows != 0) ||
	    (!sample->with_replacement &&
	     sample->rows > sample->visible_population))
		return false;
	*width = (struct sql_stats_collected_width) {
		.average_bytes = (double)sample->bytes / sample->rows,
		.denominator_rows = sample->rows,
	};
	return true;
}

static const struct sql_stats_collected_relation *
find_relation(const struct sql_stats_collection_result *result, uint32_t id)
{
	for (size_t i = 0; i < result->relation_count; i++) {
		if (result->relations[i].space_id == id)
			return &result->relations[i];
	}
	return NULL;
}

static const struct sql_stats_collected_index *
find_index(const struct sql_stats_collected_relation *relation, uint32_t id)
{
	for (size_t i = 0; i < relation->index_count; i++) {
		if (relation->indexes[i].index_id == id)
			return &relation->indexes[i];
	}
	return NULL;
}

static bool
collected_is_unique(const struct sql_stats_collection_result *result)
{
	for (size_t i = 0; i < result->relation_count; i++) {
		for (size_t j = i + 1; j < result->relation_count; j++) {
			if (result->relations[i].space_id ==
			    result->relations[j].space_id)
				return false;
		}
		const struct sql_stats_collected_relation *relation =
			&result->relations[i];
		if (relation->index_count != 0 && relation->indexes == NULL)
			return false;
		for (size_t j = 0; j < relation->index_count; j++) {
			for (size_t k = j + 1; k < relation->index_count; k++) {
				if (relation->indexes[j].index_id ==
				    relation->indexes[k].index_id)
					return false;
			}
		}
	}
	return true;
}

static bool
expected_is_unique(const struct sql_stats_expected_relation *expected,
		   size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (expected[i].index_count != 0 && expected[i].indexes == NULL)
			return false;
		for (size_t j = i + 1; j < count; j++) {
			if (expected[i].space_id == expected[j].space_id)
				return false;
		}
		for (size_t j = 0; j < expected[i].index_count; j++) {
			for (size_t k = j + 1; k < expected[i].index_count; k++) {
				if (expected[i].indexes[j].index_id ==
				    expected[i].indexes[k].index_id)
					return false;
			}
		}
	}
	return true;
}

struct sql_stats_snapshot *
sql_stats_collection_build_candidate(
	const struct sql_stats_collection_generation *expected_generation,
	const struct sql_stats_expected_relation *expected, size_t expected_count,
	const struct sql_stats_collection_result *result, size_t max_bytes)
{
	if (result == NULL || expected_generation == NULL ||
	    (expected_count != 0 && expected == NULL) ||
	    (result->relation_count != 0 && result->relations == NULL) ||
	    expected_count != result->relation_count ||
	    result->generation.catalog_version != expected_generation->catalog_version ||
	    result->generation.schema_version != expected_generation->schema_version ||
	    result->generation.visibility_id != expected_generation->visibility_id ||
	    result->generation.visibility_id == 0 ||
	    expected_generation->visibility_id == 0 ||
	    !expected_is_unique(expected, expected_count) ||
	    !collected_is_unique(result))
		return NULL;
	struct sql_stats_relation_input *inputs = expected_count == 0 ? NULL :
		collection_calloc(expected_count, sizeof(*inputs));
	if (expected_count != 0 && inputs == NULL)
		return NULL;
	bool valid = true;
	for (size_t i = 0; i < expected_count && valid; i++) {
		const struct sql_stats_expected_relation *want = &expected[i];
		const struct sql_stats_collected_relation *have =
			find_relation(result, want->space_id);
		bool has_width = have != NULL && have->width_denominator_count != 0 &&
			valid_tag(have->width_basis);
		bool empty_without_width = have != NULL && have->row_count == 0 &&
			have->width_denominator_count == 0 &&
			have->average_row_width == 0 && have->width_basis == NULL;
		if (have == NULL || have->catalog_version !=
		    result->generation.catalog_version || have->schema_version !=
		    result->generation.schema_version || have->visibility_id !=
		    result->generation.visibility_id || have->visibility_id == 0 ||
		    (!has_width && !empty_without_width) || have->modification_epoch !=
		    want->modification_epoch || have->index_count != want->index_count ||
		    (have->index_count != 0 && have->indexes == NULL) ||
		    !valid_tag(have->population_basis) ||
		    !valid_tag(have->confidence_source)) {
			valid = false;
			break;
		}
		struct sql_stats_index_input *index_inputs = want->index_count == 0 ?
			NULL : collection_calloc(want->index_count,
						 sizeof(*index_inputs));
		if (want->index_count != 0 && index_inputs == NULL) {
			valid = false;
			break;
		}
		for (size_t j = 0; j < want->index_count && valid; j++) {
			const struct sql_stats_expected_index *expected_index =
				&want->indexes[j];
			const struct sql_stats_collected_index *collected_index =
				find_index(have, expected_index->index_id);
			if (collected_index == NULL || expected_index->definition_version == 0 ||
			    collected_index->definition_version == 0 ||
			    collected_index->definition_version !=
			    expected_index->definition_version || collected_index->visibility_id !=
			    result->generation.visibility_id || collected_index->visibility_id == 0 ||
			    collected_index->prefix_count !=
			    expected_index->part_count || (collected_index->prefix_count != 0 &&
			    collected_index->distinct_prefixes == NULL) ||
			    (collected_index->part_count != 0 &&
			     (collected_index->parts == NULL ||
			      collected_index->part_count !=
					collected_index->prefix_count)) ||
			    !valid_tag(collected_index->population_basis) ||
			    !valid_tag(collected_index->ndv_basis) ||
			    !valid_cardinality_semantics(
				collected_index->tuple_count_semantics) ||
			    /* The index tuple population must match the relation's
			     * population; NDV provenance is independent and may describe
			     * an estimator/hash domain rather than the population source. */
			    strcmp(collected_index->population_basis,
				   have->population_basis) != 0) {
				valid = false;
				break;
			}
			index_inputs[j] = (struct sql_stats_index_input) {
				.index_id = collected_index->index_id,
				.tuple_count = collected_index->tuple_count,
				.tuple_count_semantics = collected_index->tuple_count_semantics,
				.population_basis = collected_index->population_basis,
				.ndv_basis = collected_index->ndv_basis,
				.definition_version = collected_index->definition_version,
				.distinct_prefixes = collected_index->distinct_prefixes,
				.prefix_count = collected_index->prefix_count,
				.parts = collected_index->parts,
				.part_count = collected_index->part_count,
			};
		}
		if (valid) {
			inputs[i] = (struct sql_stats_relation_input) {
				.space_id = have->space_id,
				.row_count = have->row_count,
				.population_basis = have->population_basis,
				.average_row_width = have->average_row_width,
				.width_basis = have->width_basis,
				.width_denominator_count = have->width_denominator_count,
				.confidence = have->confidence,
				.confidence_source = have->confidence_source,
				.cardinality_semantics = have->cardinality_semantics,
				.collected_at = have->collected_at,
				.modification_epoch = have->modification_epoch,
				.visibility_id = have->visibility_id,
				.indexes = index_inputs,
				.index_count = want->index_count,
			};
		} else {
			free(index_inputs);
		}
	}
	struct sql_stats_snapshot *candidate = NULL;
	if (valid) {
		candidate = sql_stats_snapshot_new(result->generation.catalog_version,
			result->generation.schema_version, inputs, expected_count,
			max_bytes);
	}
	for (size_t i = 0; i < expected_count; i++)
		free((void *)inputs[i].indexes);
	free(inputs);
	return candidate;
}
