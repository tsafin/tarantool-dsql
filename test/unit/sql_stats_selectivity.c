#include "box/sql/sql_stats_selectivity.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "unit.h"

static int
compare_int(const void *a, size_t a_size, const void *b, size_t b_size,
	    void *context)
{
	(void)context;
	if (a_size != sizeof(int) || b_size != sizeof(int))
		return (a_size > b_size) - (a_size < b_size);
	int lhs;
	int rhs;
	memcpy(&lhs, a, sizeof(lhs));
	memcpy(&rhs, b, sizeof(rhs));
	return (lhs > rhs) - (lhs < rhs);
}

static void
test_exact_mcv_and_independence(void)
{
	plan(9);
	header();
	int hot = 2;
	struct sql_stats_mcv_sample mcv[] = {
		{.value = &hot, .value_size = sizeof(hot), .count = 4},
	};
	struct sql_stats_column_summary summary = {
		.row_count = 10,
		.null_fraction = 0.2,
		.distinct_count = 3,
		.confidence = 0.9,
		.sample_nonnull_rows = 8,
		.mcv = mcv,
		.mcv_count = 1,
		.compare = compare_int,
	};
	struct sql_stats_selectivity result;
	ok(sql_stats_estimate_equality(&summary, NULL, 0, true, &result) == 0 &&
	   fabs(result.value - 0.2) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_EXACT,
	   "IS NULL uses exact null fraction");
	ok(sql_stats_estimate_equality(&summary, &hot, sizeof(hot), false,
				       &result) == 0 &&
	   fabs(result.value - 0.4) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_MCV,
	   "MCV frequency is scaled from non-null sample");
	int cold = 3;
	ok(sql_stats_estimate_equality(&summary, &cold, sizeof(cold), false,
				       &result) == 0 &&
	   fabs(result.value - 0.2) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_INDEPENDENCE &&
	   result.confidence < summary.confidence,
	   "untracked value uses low-confidence residual NDV");
	summary.equality_is_unique = true;
	ok(sql_stats_estimate_equality(&summary, &cold, sizeof(cold), false,
				       &result) == 0 &&
	   fabs(result.value - 0.1) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_EXACT &&
	   result.confidence == 1,
	   "known unique equality takes precedence");
	struct sql_stats_selectivity terms[] = {
		{.value = 0.4, .confidence = 0.9,
		 .source = SQL_STATS_SELECTIVITY_MCV},
		{.value = 0.2, .confidence = 0.45,
		 .source = SQL_STATS_SELECTIVITY_INDEPENDENCE},
	};
	ok(sql_stats_selectivity_and(terms, 2, &result) == 0 &&
	   fabs(result.value - 0.08) < 1e-12 && result.confidence == 0.45 &&
	   result.source == SQL_STATS_SELECTIVITY_INDEPENDENCE,
	   "AND uses product fallback with conservative confidence");
	ok(sql_stats_selectivity_and(NULL, 0, &result) == 0 &&
	   result.value == 1 && result.confidence == 1,
	   "empty conjunction is true");
	terms[0].value = NAN;
	ok(sql_stats_selectivity_and(terms, 2, &result) == -1,
	   "invalid selectivity input rejected");
	struct sql_stats_column_summary invalid = summary;
	invalid.null_fraction = 2;
	ok(sql_stats_estimate_equality(&invalid, &cold, sizeof(cold), false,
				       &result) == -1,
	   "invalid column summary rejected");
	struct sql_stats_mcv_sample duplicates[] = {
		{.value = &hot, .value_size = sizeof(hot), .count = 3},
		{.value = &hot, .value_size = sizeof(hot), .count = 1},
	};
	invalid = summary;
	invalid.mcv = duplicates;
	invalid.mcv_count = 2;
	ok(sql_stats_estimate_equality(&invalid, &hot, sizeof(hot), false,
				       &result) == -1,
	   "duplicate MCV entries rejected");
	footer();
	check_plan();
}

static void
test_histogram_ranges(void)
{
	plan(8);
	header();
	int keys[] = {1, 1, 2, 2, 3, 3, 4, 4};
	struct sql_stats_ordered_value values[8];
	for (size_t i = 0; i < 8; i++) {
		values[i].data = &keys[i];
		values[i].size = sizeof(keys[i]);
	}
	struct sql_stats_histogram *hist = sql_stats_histogram_new(values, 8, 4,
		compare_int, NULL, 4096);
	ok(hist != NULL, "histogram fixture builds");
	if (hist == NULL) {
		footer();
		check_plan();
		return;
	}
	struct sql_stats_column_summary summary = {
		.row_count = 10, .null_fraction = 0.2, .distinct_count = 4,
		.confidence = 0.8, .sample_nonnull_rows = 8,
		.histogram = hist, .compare = compare_int,
	};
	int boundary = 2;
	struct sql_stats_selectivity result;
	ok(sql_stats_estimate_range(&summary, &boundary, sizeof(boundary),
		SQL_STATS_RANGE_LT, &result) == 0 &&
	   fabs(result.value - 0.2) < 1e-12,
	   "strict range excludes equal histogram boundary");
	ok(sql_stats_estimate_range(&summary, &boundary, sizeof(boundary),
		SQL_STATS_RANGE_LE, &result) == 0 &&
	   fabs(result.value - 0.4) < 1e-12,
	   "inclusive range includes boundary and excludes NULLs");
	ok(sql_stats_estimate_range(&summary, &boundary, sizeof(boundary),
		SQL_STATS_RANGE_GT, &result) == 0 &&
	   fabs(result.value - 0.4) < 1e-12,
	   "greater-than complements inclusive boundary");
	ok(sql_stats_estimate_range(&summary, &boundary, sizeof(boundary),
		SQL_STATS_RANGE_GE, &result) == 0 &&
	   fabs(result.value - 0.6) < 1e-12,
	   "greater-or-equal includes boundary");
	int between = 3;
	ok(sql_stats_estimate_range(&summary, &between, sizeof(between),
		SQL_STATS_RANGE_LE, &result) == 0 &&
	   fabs(result.value - 0.6) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_HISTOGRAM,
	   "histogram gives cumulative estimate at next boundary");
	ok(sql_stats_estimate_range(&summary, &boundary, sizeof(boundary),
		(enum sql_stats_range_operator)99, &result) == -1,
	   "invalid range operator rejected");
	summary.sample_nonnull_rows = 7;
	ok(sql_stats_estimate_range(&summary, &boundary, sizeof(boundary),
		SQL_STATS_RANGE_LE, &result) == -1,
	   "histogram sample-size mismatch rejected");
	sql_stats_histogram_delete(hist);
	footer();
	check_plan();
}

static double
q_error(double estimate, double actual)
{
	if (estimate <= 0 || actual <= 0)
		return estimate == actual ? 1 : INFINITY;
	return estimate > actual ? estimate / actual : actual / estimate;
}

static void
test_uniform_and_skewed_qerror(void)
{
	plan(7);
	header();
	int uniform_keys[1000];
	struct sql_stats_ordered_value uniform_values[1000];
	for (size_t i = 0; i < 1000; i++) {
		uniform_keys[i] = (int)i;
		uniform_values[i] = (struct sql_stats_ordered_value){
			.data = &uniform_keys[i], .size = sizeof(uniform_keys[i]),
		};
	}
	struct sql_stats_histogram *uniform_hist = sql_stats_histogram_new(
		uniform_values, 1000, 20, compare_int, NULL, 4096);
	ok(uniform_hist != NULL, "uniform histogram builds");
	if (uniform_hist == NULL) {
		footer();
		check_plan();
		return;
	}
	struct sql_stats_column_summary uniform = {
		.row_count = 1000, .null_fraction = 0, .distinct_count = 1000,
		.confidence = 1, .sample_nonnull_rows = 1000,
		.histogram = uniform_hist, .compare = compare_int,
	};
	struct sql_stats_selectivity result;
	int value = 317;
	ok(sql_stats_estimate_equality(&uniform, &value, sizeof(value), false,
				       &result) == 0 &&
	   q_error(result.value, 1.0 / 1000) <= 1.01,
	   "uniform equality q-error is near one");
	value = 499;
	ok(sql_stats_estimate_range(&uniform, &value, sizeof(value),
		SQL_STATS_RANGE_LE, &result) == 0 &&
	   q_error(result.value, 0.5) <= 1.05,
	   "uniform median range q-error is near one");
	sql_stats_histogram_delete(uniform_hist);

	int skewed_keys[1000];
	struct sql_stats_ordered_value skewed_values[1000];
	for (size_t i = 0; i < 900; i++)
		skewed_keys[i] = 0;
	for (size_t i = 900; i < 1000; i++)
		skewed_keys[i] = (int)(i - 899);
	for (size_t i = 0; i < 1000; i++)
		skewed_values[i] = (struct sql_stats_ordered_value){
			.data = &skewed_keys[i], .size = sizeof(skewed_keys[i]),
		};
	struct sql_stats_histogram *skewed_hist = sql_stats_histogram_new(
		skewed_values, 1000, 10, compare_int, NULL, 4096);
	ok(skewed_hist != NULL, "skewed histogram builds");
	if (skewed_hist == NULL) {
		sql_stats_histogram_delete(skewed_hist);
		footer();
		check_plan();
		return;
	}
	int hot = 0;
	struct sql_stats_mcv_sample mcv[] = {
		{.value = &hot, .value_size = sizeof(hot), .count = 900},
	};
	struct sql_stats_column_summary skewed = {
		.row_count = 1000, .null_fraction = 0, .distinct_count = 101,
		.confidence = 1, .sample_nonnull_rows = 1000,
		.mcv = mcv, .mcv_count = 1, .histogram = skewed_hist,
		.compare = compare_int,
	};
	ok(sql_stats_estimate_equality(&skewed, &hot, sizeof(hot), false,
				       &result) == 0 &&
	   q_error(result.value, 0.9) <= 1.01,
	   "skewed MCV equality q-error is near one");
	int cold = 42;
	ok(sql_stats_estimate_equality(&skewed, &cold, sizeof(cold), false,
				       &result) == 0 &&
	   q_error(result.value, 0.001) <= 1.01,
	   "skewed residual equality q-error is near one");
	ok(sql_stats_estimate_range(&skewed, &hot, sizeof(hot),
		SQL_STATS_RANGE_LE, &result) == 0 &&
	   q_error(result.value, 0.9) <= 1.01,
	   "duplicate-heavy histogram preserves skew mass");
	sql_stats_histogram_delete(skewed_hist);
	footer();
	check_plan();
}

int
main(void)
{
	test_exact_mcv_and_independence();
	test_histogram_ranges();
	test_uniform_and_skewed_qerror();
	return 0;
}
