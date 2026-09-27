#include "sql_stats_collection.h"
#include "sql_stats_index_summary.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool
same_population(const struct sql_stats_sample_result *sample,
		uint64_t population)
{
	struct sql_stats_collected_population facts;
	return sql_stats_collection_population_from_sample(sample, &facts) &&
	       facts.row_count == population;
}

int
sql_stats_collection_index_from_sample(
	const struct sql_stats_expected_index *expected,
	const struct sql_stats_sample_result *sample,
	const struct sql_stats_index_summary *summary, uint64_t visibility_id,
	uint64_t *distinct_prefixes, size_t prefix_capacity,
	struct sql_stats_collected_index *index, double *confidence,
	size_t max_temp_bytes, uint64_t max_work)
{
	if (expected == NULL || expected->definition_version == 0 ||
	    expected->part_count == 0 || sample == NULL || summary == NULL ||
	    visibility_id == 0 || distinct_prefixes == NULL || index == NULL ||
	    confidence == NULL || prefix_capacity < expected->part_count)
		return -1;
	double estimate_confidence;
	if (sql_stats_index_summary_population_prefix_ndv(summary, sample,
			expected->part_count, distinct_prefixes,
			expected->part_count, &estimate_confidence,
			max_temp_bytes, max_work) != 0)
		return -1;
	static const char population_basis[] = "visible-engine-index-count-v1";
	struct sql_stats_collected_index value = {
		.index_id = expected->index_id,
		.definition_version = expected->definition_version,
		.visibility_id = visibility_id,
		.tuple_count = sample->visible_population,
		.tuple_count_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.population_basis = population_basis,
		.ndv_basis = population_basis,
		.distinct_prefixes = distinct_prefixes,
		.prefix_count = expected->part_count,
	};
	*index = value;
	*confidence = estimate_confidence;
	return 0;
}

struct sql_stats_snapshot *
sql_stats_collection_build_sample_candidate(
	const struct sql_stats_collection_generation *generation,
	const struct sql_stats_expected_relation *expected,
	const struct sql_stats_sampled_index *indexes, size_t index_count,
	const struct sql_stats_sample_result *relation_sample,
	double relation_confidence, const char *confidence_source,
	double *index_confidences,
	size_t max_bytes, size_t max_temp_bytes, uint64_t max_work)
{
	if (generation == NULL || expected == NULL || generation->visibility_id == 0 ||
	    expected->index_count != index_count ||
	    (index_count != 0 && (indexes == NULL || expected->indexes == NULL)) ||
	    (index_count != 0 && index_confidences == NULL) ||
	    relation_sample == NULL || !isfinite(relation_confidence) ||
	    relation_confidence < 0 || relation_confidence > 1 ||
	    confidence_source == NULL || confidence_source[0] == '\0')
		return NULL;
	struct sql_stats_collected_population population;
	struct sql_stats_collected_width width;
	if (!sql_stats_collection_population_from_sample(relation_sample,
							 &population) ||
	    !sql_stats_collection_width_from_sample(relation_sample, &width))
		return NULL;
	size_t prefix_count = 0;
	for (size_t i = 0; i < index_count; i++) {
		if (indexes[i].expected == NULL || indexes[i].summary == NULL ||
		    indexes[i].expected->part_count == 0 ||
		    !same_population(indexes[i].sample, population.row_count) ||
		    indexes[i].expected->part_count > SIZE_MAX - prefix_count)
			return NULL;
		prefix_count += indexes[i].expected->part_count;
	}
	if (prefix_count > SIZE_MAX / sizeof(uint64_t) ||
	    index_count > SIZE_MAX / sizeof(struct sql_stats_collected_index) ||
	    index_count > SIZE_MAX / sizeof(double))
		return NULL;
	size_t prefix_bytes = prefix_count * sizeof(uint64_t);
	size_t index_bytes = index_count * sizeof(struct sql_stats_collected_index);
	size_t confidence_bytes = index_count * sizeof(double);
	if (prefix_bytes > max_bytes || index_bytes > max_bytes - prefix_bytes ||
	    confidence_bytes > max_bytes - prefix_bytes - index_bytes)
		return NULL;
	struct sql_stats_collected_index *collected = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*collected));
	double *confidences = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*confidences));
	uint64_t *prefixes = prefix_count == 0 ? NULL :
		calloc(prefix_count, sizeof(*prefixes));
	if ((index_count != 0 && (collected == NULL || confidences == NULL)) ||
	    (prefix_count != 0 && prefixes == NULL)) {
		free(collected);
		free(confidences);
		free(prefixes);
		return NULL;
	}
	size_t offset = 0;
	bool valid = true;
	for (size_t i = 0; i < index_count; i++) {
		size_t parts = indexes[i].expected->part_count;
		double index_confidence;
		if (sql_stats_collection_index_from_sample(indexes[i].expected,
				indexes[i].sample, indexes[i].summary,
				generation->visibility_id, prefixes + offset, parts,
				&collected[i], &index_confidence, max_temp_bytes,
				max_work) != 0) {
			valid = false;
			break;
		}
		confidences[i] = index_confidence;
		collected[i].distinct_prefixes = prefixes + offset;
		offset += parts;
	}
	struct sql_stats_snapshot *candidate = NULL;
	if (valid) {
		static const char population_basis[] =
			"visible-engine-index-count-v1";
		static const char width_basis[] =
			"sampled-serialized-tuple-bytes-v1";
		struct sql_stats_collected_relation relation = {
			.space_id = expected->space_id,
			.catalog_version = generation->catalog_version,
			.schema_version = generation->schema_version,
			.visibility_id = generation->visibility_id,
			.modification_epoch = expected->modification_epoch,
			.row_count = (double)population.row_count,
			.cardinality_semantics = population.semantics,
			.population_basis = population_basis,
			.average_row_width = width.average_bytes,
			.width_basis = width_basis,
			.width_denominator_count = width.denominator_rows,
			.confidence = relation_confidence,
			.confidence_source = confidence_source,
			.indexes = collected,
			.index_count = index_count,
		};
		struct sql_stats_collection_result result = {
			.generation = *generation,
			.relations = &relation,
			.relation_count = 1,
		};
		candidate = sql_stats_collection_build_candidate(generation, expected,
								 1, &result,
								  max_bytes);
	}
	if (candidate != NULL && index_count != 0)
		memcpy(index_confidences, confidences,
		       index_count * sizeof(*index_confidences));
	free(prefixes);
	free(confidences);
	free(collected);
	return candidate;
}
