#include "sql_stats_collection.h"
#include "sql_stats_index_summary.h"

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
