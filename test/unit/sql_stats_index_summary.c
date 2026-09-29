#include "box/sql/sql_stats_index_summary.h"

#include <math.h>
#include <stdio.h>
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

static int
extract_scalar(void *context, const char *tuple, size_t tuple_size,
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

static int
extract_tagged_scalar(void *context, const char *tuple, size_t tuple_size,
		      const uint32_t *field_ids, size_t field_count,
		      struct sql_stats_hll_value *parts, size_t part_count)
{
	(void)context;
	(void)field_ids;
	(void)field_count;
	if (tuple_size == 0 || part_count != 1)
		return -1;
	parts[0] = (struct sql_stats_hll_value){
		.type_tag = tuple[0] == 'N' ? 0 : 1,
		.data = tuple + 1,
		.size = tuple_size - 1,
	};
	return 0;
}

static void
test_mcv_index_summaries(void)
{
	plan(9);
	header();
	size_t hll_bytes = 0, mcv_bytes = 0;
	ok(sql_stats_hll_storage_bytes(8, &hll_bytes) &&
	   sql_stats_spacesaving_storage_bytes(2, 9, &mcv_bytes) == 0,
	   "HLL and MCV report bounded storage");
	size_t budget = hll_bytes + sizeof(void *) + sizeof(void *) +
		sizeof(uint64_t) + mcv_bytes + 9;
	struct sql_stats_index_summary *summary =
		sql_stats_index_summary_new_with_mcv(1, 8, 31, budget,
			extract_tagged_scalar, NULL, 2, 8);
	ok(summary != NULL, "MCV summary allocates within explicit total budget");
	ok(sql_stats_index_summary_new_with_mcv(1, 8, 31, budget - 1,
		extract_tagged_scalar, NULL, 2, 8) == NULL,
	   "MCV summary rejects a budget one byte below its allocation");
	fail_if(summary == NULL);
	bool consumed = true;
	for (int i = 0; i < 4; i++)
		consumed &= sql_stats_index_summary_consume(summary, "Ihot", 4,
							 NULL, 0) == 0;
	consumed &= sql_stats_index_summary_consume(summary, "Itail", 5,
							 NULL, 0) == 0;
	consumed &= sql_stats_index_summary_consume(summary, "N", 1,
							 NULL, 0) == 0;
	ok(consumed && sql_stats_index_summary_mcv_sample_nonnull_rows(summary,
									   0) == 5,
	   "MCV population counts non-NULL sampled values only");
	ok(sql_stats_index_summary_mcv_count(summary, 0) == 2,
	   "bounded SpaceSaving retains at most configured candidate capacity");
	bool found_hot = false;
	for (uint32_t i = 0; i < sql_stats_index_summary_mcv_count(summary, 0); i++) {
		uint8_t type_tag;
		const void *value;
		size_t value_size;
		struct sql_stats_spacesaving_entry entry;
		if (sql_stats_index_summary_mcv_at(summary, 0, i, &type_tag, &value,
							 &value_size, &entry) == 0 &&
		    type_tag == 1 && value_size == 3 &&
		    memcmp(value, "hot", 3) == 0 &&
		    entry.estimate == 4 && entry.error == 0)
			found_hot = true;
	}
	ok(found_hot, "MCV exposes canonical value bytes and SpaceSaving bounds");
	ok(sql_stats_index_summary_mcv_at(summary, 0, 0, NULL, NULL, NULL,
						   NULL) == -1,
	   "MCV accessor rejects missing output pointers");
	sql_stats_index_summary_delete(summary);
	struct sql_stats_index_summary *bounded =
		sql_stats_index_summary_new_with_mcv(1, 8, 31, budget,
			extract_tagged_scalar, NULL, 2, 3);
	fail_if(bounded == NULL);
	ok(sql_stats_index_summary_consume(bounded, "Ioversize", 9, NULL, 0) == -1 &&
	   sql_stats_index_summary_sample_rows(bounded) == 0,
	   "oversize canonical values fail closed without publishing partial summary");
	sql_stats_index_summary_delete(bounded);
	struct sql_stats_index_summary *without_mcv =
		sql_stats_index_summary_new(1, 8, 31, 10000,
			extract_tagged_scalar, NULL);
	fail_if(without_mcv == NULL);
	ok(sql_stats_index_summary_mcv_count(without_mcv, 0) == 0,
	   "existing HLL-only constructor remains MCV-free");
	sql_stats_index_summary_delete(without_mcv);
	footer();
	check_plan();
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

static void
test_population_prefix_estimation(void)
{
	plan(10);
	header();
	struct sql_stats_index_summary *summary =
		sql_stats_index_summary_new(1, 12, 17, 10000, extract_scalar, NULL);
	ok(summary != NULL, "population estimator allocates bounded prefix sketch");
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
	ok(consumed, "sample prefix summary accepts delivered sample values");
	struct sql_stats_sample_result sample = {
		.rows = 100, .population_known = true,
		.visible_population = 1000, .with_replacement = false,
	};
	uint64_t ndv = 0;
	double confidence = 0;
	ok(sql_stats_index_summary_population_prefix_ndv(summary, &sample, 1,
							   &ndv, 1,
							   &confidence, 64, 1000000) == 0 &&
	   ndv >= 90 && ndv <= 110 && confidence > 0 && confidence < 1,
	   "reservoir occupancy inversion estimates population NDV and confidence");
	sample.with_replacement = true;
	ok(sql_stats_index_summary_population_prefix_ndv(summary, &sample, 1,
							   &ndv, 1,
							   &confidence, 64, 1000000) == 0 &&
	   ndv >= 90 && ndv <= 110,
	   "independent-draw occupancy inversion estimates population NDV");
	sample.population_known = false;
	ndv = 777;
	confidence = 0.25;
	ok(sql_stats_index_summary_population_prefix_ndv(summary, &sample, 1,
							   &ndv, 1,
							   &confidence, 64, 1000000) != 0 &&
	   ndv == 777 && confidence == 0.25,
	   "unknown population rejects without exposing partial outputs");
	sample.population_known = true;
	ok(sql_stats_index_summary_population_prefix_ndv(summary, &sample, 1,
							   &ndv, 1,
							   &confidence, 15, 1000000) != 0 &&
	   ndv == 777 && confidence == 0.25,
	   "population estimator enforces temporary-memory budget");
	ok(sql_stats_index_summary_population_prefix_ndv(summary, &sample, 1,
							   &ndv, 1,
							   &confidence, 64, 1) != 0 &&
	   ndv == 777 && confidence == 0.25,
	   "population estimator enforces its work budget");
	sample.with_replacement = false;
	sample.visible_population = sample.rows;
	ok(sql_stats_index_summary_population_prefix_ndv(summary, &sample, 1,
							   &ndv, 1,
							   &confidence, 64, 1000000) == 0 &&
	   ndv >= 60 && ndv <= 66 && confidence > 0,
	   "complete reservoir sample returns observed HLL NDV without extrapolation");
	sql_stats_index_summary_delete(summary);
	struct sql_stats_index_summary *skewed =
		sql_stats_index_summary_new(1, 12, 19, 10000,
					    extract_scalar, NULL);
	ok(skewed != NULL, "skewed sample summary allocates");
	fail_if(skewed == NULL);
	bool skew_consumed = true;
	for (int i = 0; i < 100; i++) {
		const char *value = i < 95 ? "hot" : values[i - 95];
		if (sql_stats_index_summary_consume(skewed, value, strlen(value),
						   NULL, 0) != 0)
			skew_consumed = false;
	}
	sample = (struct sql_stats_sample_result) {
		.rows = 100, .population_known = true,
		.visible_population = 1000, .with_replacement = true,
	};
	ndv = 0;
	confidence = 0;
	ok(skew_consumed &&
	   sql_stats_index_summary_population_prefix_ndv(skewed, &sample, 1,
							   &ndv, 1,
							   &confidence, 64, 1000000) == 0 &&
	   ndv < 20 && confidence > 0 && confidence < 0.1,
	   "low-coverage skew returns a low-confidence occupancy-model estimate");
	sql_stats_index_summary_delete(skewed);
	footer();
	check_plan();
}

int
main(void)
{
	test_sample_prefix_summaries();
	test_mcv_index_summaries();
	test_population_prefix_estimation();
	return 0;
}
