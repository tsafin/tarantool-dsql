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
	static const char hash_basis[] =
		"visible-engine-index-hash32-equivalence-classes-v1";
	const char *ndv_basis = sql_stats_index_summary_hash_bits(summary) == 32 ?
		hash_basis : population_basis;
	struct sql_stats_collected_index value = {
		.index_id = expected->index_id,
		.definition_version = expected->definition_version,
		.visibility_id = visibility_id,
		.tuple_count = sample->visible_population,
		.tuple_count_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.population_basis = population_basis,
		.ndv_basis = ndv_basis,
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
								 &population))
		return NULL;
	bool has_width = sql_stats_collection_width_from_sample(relation_sample,
									 &width);
	if ((!has_width && population.row_count != 0) ||
	    (population.row_count != 0 && relation_sample->rows == 0))
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
	size_t scratch_bytes = prefix_bytes + index_bytes + confidence_bytes;
	if (index_count > SIZE_MAX / sizeof(struct sql_stats_index_part_input *) ||
	    index_count > SIZE_MAX / sizeof(struct sql_stats_mcv_input **))
		return NULL;
	struct sql_stats_collected_index *collected = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*collected));
	double *confidences = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*confidences));
	uint64_t *prefixes = prefix_count == 0 ? NULL :
		calloc(prefix_count, sizeof(*prefixes));
	struct sql_stats_index_part_input **part_storage = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*part_storage));
	struct sql_stats_mcv_input ***mcv_storage = index_count == 0 ? NULL :
		calloc(index_count, sizeof(*mcv_storage));
	if ((index_count != 0 && (collected == NULL || confidences == NULL)) ||
	    (prefix_count != 0 && prefixes == NULL) ||
	    (index_count != 0 && (part_storage == NULL || mcv_storage == NULL))) {
		free(collected);
		free(confidences);
		free(prefixes);
		free(part_storage);
		free(mcv_storage);
		return NULL;
	}
	bool valid = true;
	struct sql_stats_snapshot *candidate = NULL;
	if (index_count != 0) {
		if (index_count > (max_bytes - scratch_bytes) /
		    (2 * sizeof(void *))) {
			valid = false;
		} else {
			scratch_bytes += 2 * index_count * sizeof(void *);
		}
	}
	size_t offset = 0;
	for (size_t i = 0; valid && i < index_count; i++) {
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
		if (sql_stats_index_summary_has_mcv(indexes[i].summary)) {
			if (parts > SIZE_MAX / sizeof(*part_storage[i]) ||
			    parts > SIZE_MAX / sizeof(*mcv_storage[i]) ||
			    parts > (max_bytes - scratch_bytes) /
				    (sizeof(struct sql_stats_index_part_input) +
				     sizeof(struct sql_stats_mcv_input *))) {
				valid = false;
				break;
			}
			part_storage[i] = calloc(parts, sizeof(*part_storage[i]));
			mcv_storage[i] = calloc(parts, sizeof(*mcv_storage[i]));
			if (part_storage[i] == NULL || mcv_storage[i] == NULL) {
				valid = false;
				break;
			}
			scratch_bytes += parts *
				(sizeof(struct sql_stats_index_part_input) +
				 sizeof(struct sql_stats_mcv_input *));
			for (size_t p = 0; p < parts; p++) {
				uint32_t count = sql_stats_index_summary_mcv_count(
					indexes[i].summary, p);
				size_t mcv_bytes;
				if (__builtin_mul_overflow((size_t)count,
						sizeof(struct sql_stats_mcv_input),
						&mcv_bytes) ||
				    mcv_bytes > max_bytes - scratch_bytes) {
					valid = false;
					break;
				}
				struct sql_stats_mcv_input *values = count == 0 ? NULL :
					calloc(count, sizeof(*values));
				if (count != 0 && values == NULL) {
					valid = false;
					break;
				}
				mcv_storage[i][p] = values;
				scratch_bytes += mcv_bytes;
				for (uint32_t n = 0; n < count; n++) {
					uint8_t type_tag;
					const void *value;
					size_t value_size;
					struct sql_stats_spacesaving_entry entry;
					if (sql_stats_index_summary_mcv_at(indexes[i].summary,
						p, n, &type_tag, &value, &value_size,
						&entry) != 0) {
						valid = false;
						break;
					}
					values[n] = (struct sql_stats_mcv_input) {
						.type_tag = type_tag,
						.value = value,
						.value_size = value_size,
						.estimate = entry.estimate,
						.error = entry.error,
					};
				}
				if (!valid)
					break;
				part_storage[i][p] =
					(struct sql_stats_index_part_input) {
					.sample_rows = sql_stats_index_summary_sample_rows(
						indexes[i].summary),
					.sample_nonnull_rows =
						sql_stats_index_summary_mcv_sample_nonnull_rows(
							indexes[i].summary, p),
					.mcv = values,
					.mcv_count = count,
				};
			}
			if (!valid)
				break;
			collected[i].parts = part_storage[i];
			collected[i].part_count = parts;
		}
		offset += parts;
	}
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
			.average_row_width = has_width ? width.average_bytes : 0,
			.width_basis = has_width ? width_basis : NULL,
			.width_denominator_count = has_width ? width.denominator_rows : 0,
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
	for (size_t i = 0; i < index_count; i++) {
		if (mcv_storage != NULL && mcv_storage[i] != NULL) {
			for (size_t p = 0; p < indexes[i].expected->part_count; p++)
				free(mcv_storage[i][p]);
		}
		if (mcv_storage != NULL)
			free(mcv_storage[i]);
		if (part_storage != NULL)
			free(part_storage[i]);
	}
	free(mcv_storage);
	free(part_storage);
	return candidate;
}
