#include "box/sql/sql_stats_index_summary.h"

#include <math.h>
#include <string.h>

#include "unit.h"

static int
extract_parts(void *context, const char *tuple, size_t tuple_size,
	     const uint32_t *field_ids, size_t field_count,
	     struct sql_stats_hll_value *parts, size_t part_count)
{
	(void)context;
	(void)field_ids;
	(void)field_count;
	if (tuple_size != 3 || tuple[1] != '|' || part_count != 2)
		return -1;
	parts[0] = (struct sql_stats_hll_value){1, tuple, 1};
	parts[1] = (struct sql_stats_hll_value){1, tuple + 2, 1};
	return 0;
}

static int
extract_fail(void *context, const char *tuple, size_t tuple_size,
	    const uint32_t *field_ids, size_t field_count,
	    struct sql_stats_hll_value *parts, size_t part_count)
{
	(void)context;
	(void)tuple;
	(void)tuple_size;
	(void)field_ids;
	(void)field_count;
	(void)parts;
	(void)part_count;
	return -1;
}

static void
test_sample_prefix_summaries(void)
{
	plan(8);
	header();
	size_t hll_bytes;
	ok(sql_stats_hll_storage_bytes(8, &hll_bytes),
	   "sketch reports exact bounded storage");
	struct sql_stats_index_summary *summary =
		sql_stats_index_summary_new(2, 8, 42,
			2 * hll_bytes + 2 * sizeof(void *), extract_parts, NULL);
	ok(summary != NULL, "summary allocates within explicit budget");
	ok(sql_stats_index_summary_new(2, 8, 42,
		2 * hll_bytes + 2 * sizeof(void *) - 1,
		extract_parts, NULL) == NULL,
	   "summary rejects budget one byte below its allocation");
	fail_if(summary == NULL);
	ok(sql_stats_index_summary_consume(summary, "a|x", 3, NULL, 0) == 0 &&
	   sql_stats_index_summary_consume(summary, "a|y", 3, NULL, 0) == 0 &&
	   sql_stats_index_summary_consume(summary, "b|x", 3, NULL, 0) == 0,
	   "sample tuples feed all ordered prefixes");
	double ndv[2];
	ok(sql_stats_index_summary_prefix_ndv(summary, 2, ndv, 2) == 0 &&
	   fabs(ndv[0] - 2.0) < 0.1 && fabs(ndv[1] - 3.0) < 0.1,
	   "prefix estimates reflect sampled single and composite values");
	ok(sql_stats_index_summary_sample_rows(summary) == 3 &&
	   sql_stats_index_summary_sample_bytes(summary) == 9,
	   "summary counts only delivered sample rows and bytes");
	ok(sql_stats_index_summary_prefix_ndv(summary, 3, ndv, 2) == -1,
	   "request beyond configured index parts is rejected");
	sql_stats_index_summary_delete(summary);
	struct sql_stats_index_summary *failed =
		sql_stats_index_summary_new(1, 8, 42, hll_bytes + sizeof(void *),
					    extract_fail, NULL);
	fail_if(failed == NULL);
	ok(sql_stats_index_summary_consume(failed, "a|x", 3, NULL, 0) == -1 &&
	   sql_stats_index_summary_prefix_ndv(failed, 1, ndv, 1) == -1,
	   "extractor failure poisons summary instead of exposing partial data");
	sql_stats_index_summary_delete(failed);
	footer();
	check_plan();
}

int
main(void)
{
	test_sample_prefix_summaries();
	return 0;
}
