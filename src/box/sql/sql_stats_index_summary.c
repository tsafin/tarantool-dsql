#include "sql_stats_index_summary.h"

#include <stdlib.h>

struct sql_stats_index_summary {
	size_t part_count;
	struct sql_stats_hll **prefixes;
	sql_stats_index_value_extract_f *extract;
	void *extract_context;
	uint64_t rows;
	uint64_t bytes;
	bool failed;
};

struct sql_stats_index_summary *
sql_stats_index_summary_new(size_t part_count, uint8_t precision,
			    uint64_t seed, size_t max_bytes,
			    sql_stats_index_value_extract_f *extract,
			    void *extract_context)
{
	if (part_count == 0 || part_count > SIZE_MAX / sizeof(struct sql_stats_hll *) ||
	    extract == NULL || precision < 4 || precision > 18)
		return NULL;
	size_t pointer_bytes = part_count * sizeof(struct sql_stats_hll *);
	size_t sketch_bytes;
	if (!sql_stats_hll_storage_bytes(precision, &sketch_bytes) ||
	    pointer_bytes > max_bytes ||
	    part_count > (max_bytes - pointer_bytes) / sketch_bytes)
		return NULL;
	struct sql_stats_index_summary *summary = calloc(1, sizeof(*summary));
	if (summary == NULL)
		return NULL;
	summary->prefixes = calloc(part_count, sizeof(*summary->prefixes));
	if (summary->prefixes == NULL)
		goto fail;
	summary->part_count = part_count;
	summary->extract = extract;
	summary->extract_context = extract_context;
	for (size_t i = 0; i < part_count; i++) {
		summary->prefixes[i] = sql_stats_hll_new(precision, seed);
		if (summary->prefixes[i] == NULL)
			goto fail;
	}
	return summary;
fail:
	sql_stats_index_summary_delete(summary);
	return NULL;
}

void
sql_stats_index_summary_delete(struct sql_stats_index_summary *summary)
{
	if (summary == NULL)
		return;
	for (size_t i = 0; i < summary->part_count; i++)
		sql_stats_hll_delete(summary->prefixes == NULL ? NULL :
				     summary->prefixes[i]);
	free(summary->prefixes);
	free(summary);
}

int
sql_stats_index_summary_consume(void *context, const char *tuple,
				size_t tuple_size, const uint32_t *field_ids,
				size_t field_count)
{
	struct sql_stats_index_summary *summary = context;
	if (summary == NULL || summary->failed || tuple == NULL ||
	    tuple_size == 0 || (field_count != 0 && field_ids == NULL) ||
	    summary->rows == UINT64_MAX || tuple_size > UINT64_MAX - summary->bytes) {
		if (summary != NULL)
			summary->failed = true;
		return -1;
	}
	struct sql_stats_hll_value *parts = calloc(summary->part_count,
							 sizeof(*parts));
	if (parts == NULL || summary->extract(summary->extract_context, tuple,
		tuple_size, field_ids, field_count, parts, summary->part_count) != 0) {
		free(parts);
		summary->failed = true;
		return -1;
	}
	for (size_t i = 0; i < summary->part_count; i++) {
		if (parts[i].data == NULL && parts[i].size != 0) {
			summary->failed = true;
			free(parts);
			return -1;
		}
		if (sql_stats_hll_add_tuple(summary->prefixes[i], parts, i + 1) != 0) {
			summary->failed = true;
			free(parts);
			return -1;
		}
	}
	free(parts);
	summary->rows++;
	summary->bytes += tuple_size;
	return 0;
}

uint64_t
sql_stats_index_summary_sample_rows(const struct sql_stats_index_summary *summary)
{
	return summary == NULL || summary->failed ? 0 : summary->rows;
}

uint64_t
sql_stats_index_summary_sample_bytes(const struct sql_stats_index_summary *summary)
{
	return summary == NULL || summary->failed ? 0 : summary->bytes;
}

int
sql_stats_index_summary_prefix_ndv(
	const struct sql_stats_index_summary *summary, size_t prefix_count,
	double *estimates, size_t estimate_count)
{
	if (summary == NULL || summary->failed || prefix_count == 0 ||
	    prefix_count > summary->part_count || estimate_count != prefix_count ||
	    estimates == NULL)
		return -1;
	for (size_t i = 0; i < prefix_count; i++)
		estimates[i] = sql_stats_hll_estimate(summary->prefixes[i]);
	return 0;
}
