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
test_joint_mcv_equality_conjunction(void)
{
	plan(6);
	header();
	int zero = 0;
	int one = 1;
	struct sql_stats_mcv_sample col0_mcv = {
		.value = &zero, .value_size = sizeof(zero), .count = 500,
	};
	struct sql_stats_mcv_sample col1_mcv = col0_mcv;
	struct sql_stats_column_summary summaries[] = {
		{.row_count = 1000, .distinct_count = 2, .confidence = 0.9,
		 .sample_nonnull_rows = 1000, .mcv = &col0_mcv, .mcv_count = 1,
		 .compare = compare_int},
		{.row_count = 1000, .distinct_count = 2, .confidence = 0.8,
		 .sample_nonnull_rows = 1000, .mcv = &col1_mcv, .mcv_count = 1,
		 .compare = compare_int},
	};
	struct sql_stats_mcv_value hot_tuple[] = {
		{.value = &zero, .value_size = sizeof(zero)},
		{.value = &zero, .value_size = sizeof(zero)},
	};
	struct sql_stats_joint_mcv_sample joint[] = {
		{.values = hot_tuple, .value_count = 2, .count = 400},
	};
	struct sql_stats_selectivity result;
	struct sql_stats_selectivity fallback;
	ok(sql_stats_estimate_equality_conjunction(summaries, hot_tuple, 2,
		NULL, 0, 0, &fallback) == 0 &&
	   sql_stats_estimate_equality_conjunction(summaries, hot_tuple, 2,
		joint, 1, 1000, &result) == 0 &&
	   q_error(result.value, 0.4) == 1 &&
	   q_error(result.value, 0.4) < q_error(fallback.value, 0.4) &&
	   result.source == SQL_STATS_SELECTIVITY_MCV,
	   "joint MCV corrects correlated conjunction q-error");
	struct sql_stats_mcv_value cold_tuple[] = {
		{.value = &one, .value_size = sizeof(one)},
		{.value = &one, .value_size = sizeof(one)},
	};
	ok(sql_stats_estimate_equality_conjunction(summaries, cold_tuple, 2,
		joint, 1, 1000, &result) == 0 &&
	   fabs(result.value - 0.25) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_INDEPENDENCE,
	   "missing joint MCV uses independence fallback");
	struct sql_stats_joint_mcv_sample rare_joint[] = {
		{.values = hot_tuple, .value_count = 2, .count = 10},
	};
	ok(sql_stats_estimate_equality_conjunction(summaries, hot_tuple, 2,
		rare_joint, 1, 1000, &result) == 0 &&
	   q_error(result.value, 0.01) == 1 &&
	   q_error(result.value, 0.01) < q_error(fallback.value, 0.01),
	   "joint MCV corrects anti-correlated rare conjunction q-error");
	struct sql_stats_joint_mcv_sample invalid = joint[0];
	invalid.value_count = 1;
	ok(sql_stats_estimate_equality_conjunction(summaries, hot_tuple, 2,
		&invalid, 1, 1000, &result) == -1,
	   "joint MCV arity mismatch rejected");
	struct sql_stats_joint_mcv_sample duplicate[] = {joint[0], joint[0]};
	ok(sql_stats_estimate_equality_conjunction(summaries, hot_tuple, 2,
		duplicate, 2, 1000, &result) == -1,
	   "duplicate joint MCV tuples rejected");
	struct sql_stats_mcv_value malformed = {
		.value = NULL, .value_size = sizeof(int),
	};
	ok(sql_stats_estimate_equality_conjunction(summaries, &malformed, 1,
		NULL, 0, 0, &result) == -1,
	   "malformed equality input rejected");
	footer();
	check_plan();
}

static void
test_joint_mcv_range_conjunction(void)
{
	plan(8);
	header();
	int values0[] = {0, 0, 1, 1};
	int values1[] = {0, 1, 0, 1};
	int histogram_values1[] = {0, 0, 1, 1};
	struct sql_stats_ordered_value ordered0[4];
	struct sql_stats_ordered_value ordered1[4];
	for (size_t i = 0; i < 4; i++) {
		ordered0[i] = (struct sql_stats_ordered_value){
			.data = &values0[i], .size = sizeof(values0[i]),
		};
		ordered1[i] = (struct sql_stats_ordered_value){
			.data = &histogram_values1[i],
			.size = sizeof(histogram_values1[i]),
		};
	}
	struct sql_stats_histogram *hist0 = sql_stats_histogram_new(
		ordered0, 4, 2, compare_int, NULL, 1024);
	struct sql_stats_histogram *hist1 = sql_stats_histogram_new(
		ordered1, 4, 2, compare_int, NULL, 1024);
	ok(hist0 != NULL && hist1 != NULL,
	   "joint range histograms build");
	if (hist0 == NULL || hist1 == NULL) {
		sql_stats_histogram_delete(hist0);
		sql_stats_histogram_delete(hist1);
		footer();
		check_plan();
		return;
	}
	struct sql_stats_column_summary summaries[] = {
		{.row_count = 4, .distinct_count = 2, .confidence = 0.9,
		 .sample_nonnull_rows = 4, .histogram = hist0,
		 .compare = compare_int},
		{.row_count = 4, .distinct_count = 2, .confidence = 0.8,
		 .sample_nonnull_rows = 4, .histogram = hist1,
		 .compare = compare_int},
	};
	struct sql_stats_mcv_value tuple_values[4][2];
	struct sql_stats_joint_mcv_sample joint[4];
	for (size_t i = 0; i < 4; i++) {
		tuple_values[i][0] = (struct sql_stats_mcv_value){
			.value = &values0[i], .value_size = sizeof(values0[i]),
		};
		tuple_values[i][1] = (struct sql_stats_mcv_value){
			.value = &values1[i], .value_size = sizeof(values1[i]),
		};
		joint[i] = (struct sql_stats_joint_mcv_sample){
			.values = tuple_values[i], .value_count = 2, .count = 1,
		};
	}
	int zero = 0;
	int one = 1;
	struct sql_stats_predicate range = {
		.column_index = 0, .kind = SQL_STATS_PREDICATE_RANGE,
		.range_operator = SQL_STATS_RANGE_LE,
		.value = &zero, .value_size = sizeof(zero),
	};
	struct sql_stats_predicate mixed[] = {
		range,
		{.column_index = 1, .kind = SQL_STATS_PREDICATE_EQUALITY,
		 .value = &one, .value_size = sizeof(one)},
	};
	struct sql_stats_selectivity result;
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, &range, 1,
		joint, 4, 4, &result) == 0 &&
	   fabs(result.value - 0.5) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_MCV,
	   "complete joint sample estimates an inclusive range exactly");
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, mixed, 2,
		joint, 4, 4, &result) == 0 &&
	   fabs(result.value - 0.25) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_MCV,
	   "complete joint sample estimates mixed range/equality exactly");
	struct sql_stats_predicate strict = range;
	strict.range_operator = SQL_STATS_RANGE_LT;
	strict.value = &one;
	strict.value_size = sizeof(one);
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, &strict, 1,
		joint, 4, 4, &result) == 0 &&
	   fabs(result.value - 0.5) < 1e-12,
	   "complete joint sample preserves strict range boundary");
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, mixed, 2,
		joint, 2, 4, &result) == 0 &&
	   fabs(result.value - 0.25) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_INDEPENDENCE,
	   "incomplete joint sample falls back to independent terms");
	int minus_one = -1;
	struct sql_stats_predicate duplicate[] = {
		range,
		{.column_index = 0, .kind = SQL_STATS_PREDICATE_RANGE,
		 .range_operator = SQL_STATS_RANGE_GT,
		 .value = &minus_one, .value_size = sizeof(minus_one)},
	};
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, duplicate, 2,
		joint, 4, 4, &result) == 0 &&
	   fabs(result.value - 0.25) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_MCV,
	   "complete joint sample applies repeated same-column bounds together");
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, duplicate, 2,
		joint, 2, 4, &result) == -1,
	   "partial joint sample rejects unsupported two-sided range fallback");
	struct sql_stats_joint_mcv_sample malformed = joint[0];
	malformed.value_count = 1;
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, mixed, 2,
		&malformed, 1, 4, &result) == -1,
	   "joint range sample with wrong arity rejected");
	sql_stats_histogram_delete(hist0);
	sql_stats_histogram_delete(hist1);
	footer();
	check_plan();
}

static void
test_same_column_equality_constraints(void)
{
	plan(4);
	header();
	int hot = 2;
	int cold = 3;
	struct sql_stats_mcv_sample mcv = {
		.value = &hot, .value_size = sizeof(hot), .count = 4,
	};
	struct sql_stats_column_summary summary = {
		.row_count = 10, .null_fraction = 0, .distinct_count = 3,
		.confidence = 0.9, .sample_nonnull_rows = 8,
		.mcv = &mcv, .mcv_count = 1, .compare = compare_int,
	};
	struct sql_stats_predicate same[] = {
		{.column_index = 0, .kind = SQL_STATS_PREDICATE_EQUALITY,
		 .value = &hot, .value_size = sizeof(hot)},
		{.column_index = 0, .kind = SQL_STATS_PREDICATE_EQUALITY,
		 .value = &hot, .value_size = sizeof(hot)},
	};
	struct sql_stats_selectivity result;
	ok(sql_stats_estimate_predicate_conjunction(&summary, 1, same, 2,
		NULL, 0, 0, &result) == 0 &&
	   fabs(result.value - 0.5) < 1e-12 &&
	   result.source == SQL_STATS_SELECTIVITY_MCV,
	   "repeated equality on one column is estimated once, not squared");
	same[1].value = &cold;
	same[1].value_size = sizeof(cold);
	ok(sql_stats_estimate_predicate_conjunction(&summary, 1, same, 2,
		NULL, 0, 0, &result) == 0 && result.value == 0 &&
	   result.source == SQL_STATS_SELECTIVITY_EXACT,
	   "different equalities on one column are an exact contradiction");
	struct sql_stats_predicate ranges[] = {
		{.column_index = 0, .kind = SQL_STATS_PREDICATE_RANGE,
		 .range_operator = SQL_STATS_RANGE_GT,
		 .value = &hot, .value_size = sizeof(hot)},
		{.column_index = 0, .kind = SQL_STATS_PREDICATE_RANGE,
		 .range_operator = SQL_STATS_RANGE_GE,
		 .value = &hot, .value_size = sizeof(hot)},
	};
	/* Without a histogram the selected strict bound must fail closed. */
	ok(sql_stats_estimate_predicate_conjunction(&summary, 1, ranges, 2,
		NULL, 0, 0, &result) == -1,
	   "same-column ranges require stats or exhaustive joint sample");
	ranges[1].range_operator = SQL_STATS_RANGE_LT;
	ok(sql_stats_estimate_predicate_conjunction(&summary, 1, ranges, 2,
		NULL, 0, 0, &result) == 0 && result.value == 0 &&
	   result.source == SQL_STATS_SELECTIVITY_EXACT,
	   "strict and inclusive bounds detect empty same-value interval");
	footer();
	check_plan();
}

static void
test_correlated_range_conjunction_qerror(void)
{
	plan(2);
	header();
	int keys[100];
	struct sql_stats_ordered_value ordered[100];
	for (size_t i = 0; i < 100; i++) {
		keys[i] = i < 50 ? 0 : 1;
		ordered[i] = (struct sql_stats_ordered_value){
			.data = &keys[i], .size = sizeof(keys[i]),
		};
	}
	struct sql_stats_histogram *hist0 = sql_stats_histogram_new(
		ordered, 100, 2, compare_int, NULL, 4096);
	struct sql_stats_histogram *hist1 = sql_stats_histogram_new(
		ordered, 100, 2, compare_int, NULL, 4096);
	ok(hist0 != NULL && hist1 != NULL,
	   "correlated range fixture histograms build");
	if (hist0 == NULL || hist1 == NULL) {
		sql_stats_histogram_delete(hist0);
		sql_stats_histogram_delete(hist1);
		footer();
		check_plan();
		return;
	}
	struct sql_stats_column_summary summaries[] = {
		{.row_count = 100, .distinct_count = 2, .confidence = 0.9,
		 .sample_nonnull_rows = 100, .histogram = hist0,
		 .compare = compare_int},
		{.row_count = 100, .distinct_count = 2, .confidence = 0.9,
		 .sample_nonnull_rows = 100, .histogram = hist1,
		 .compare = compare_int},
	};
	int zero = 0;
	int one = 1;
	struct sql_stats_mcv_value tuple0[] = {
		{.value = &zero, .value_size = sizeof(zero)},
		{.value = &zero, .value_size = sizeof(zero)},
	};
	struct sql_stats_mcv_value tuple1[] = {
		{.value = &one, .value_size = sizeof(one)},
		{.value = &one, .value_size = sizeof(one)},
	};
	struct sql_stats_joint_mcv_sample joint[] = {
		{.values = tuple0, .value_count = 2, .count = 50},
		{.values = tuple1, .value_count = 2, .count = 50},
	};
	struct sql_stats_predicate predicates[] = {
		{.column_index = 0, .kind = SQL_STATS_PREDICATE_RANGE,
		 .range_operator = SQL_STATS_RANGE_LE,
		 .value = &zero, .value_size = sizeof(zero)},
		{.column_index = 1, .kind = SQL_STATS_PREDICATE_EQUALITY,
		 .value = &zero, .value_size = sizeof(zero)},
	};
	struct sql_stats_selectivity exact;
	struct sql_stats_selectivity independent;
	ok(sql_stats_estimate_predicate_conjunction(summaries, 2, predicates, 2,
		joint, 2, 100, &exact) == 0 &&
	   sql_stats_estimate_predicate_conjunction(summaries, 2, predicates, 2,
		NULL, 0, 0, &independent) == 0 &&
	   q_error(exact.value, 0.5) == 1 &&
	   q_error(exact.value, 0.5) < q_error(independent.value, 0.5),
	   "joint range/equality sample corrects correlation q-error");
	sql_stats_histogram_delete(hist0);
	sql_stats_histogram_delete(hist1);
	footer();
	check_plan();
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

static void
test_stale_summary_qerror(void)
{
	plan(3);
	header();
	/* The collection sample says value 1 is hot; the current population has
	 * shifted so value 0 is hot. The stale summary has lower confidence. */
	int old_hot = 1;
	int current_hot = 0;
	struct sql_stats_mcv_sample fresh_mcv = {
		.value = &current_hot, .value_size = sizeof(current_hot),
		.count = 900,
	};
	struct sql_stats_mcv_sample stale_mcv = {
		.value = &old_hot, .value_size = sizeof(old_hot), .count = 900,
	};
	struct sql_stats_column_summary fresh = {
		.row_count = 1000, .null_fraction = 0, .distinct_count = 10,
		.confidence = 0.9, .sample_nonnull_rows = 1000,
		.mcv = &fresh_mcv, .mcv_count = 1, .compare = compare_int,
	};
	struct sql_stats_column_summary stale = fresh;
	stale.confidence = 0.25;
	stale.mcv = &stale_mcv;
	struct sql_stats_selectivity fresh_result;
	struct sql_stats_selectivity stale_result;
	ok(sql_stats_estimate_equality(&fresh, &current_hot,
		 sizeof(current_hot), false, &fresh_result) == 0 &&
	   q_error(fresh_result.value, 0.9) == 1,
	   "current MCV summary estimates shifted hot value accurately");
	ok(sql_stats_estimate_equality(&stale, &current_hot,
		 sizeof(current_hot), false, &stale_result) == 0 &&
	   q_error(stale_result.value, 0.9) > q_error(fresh_result.value, 0.9),
	   "stale MCV fixture exposes q-error after distribution shift");
	ok(stale_result.confidence < fresh_result.confidence,
	   "caller-supplied stale confidence remains visible in result");
	footer();
	check_plan();
}

int
main(void)
{
	test_exact_mcv_and_independence();
	test_histogram_ranges();
	test_uniform_and_skewed_qerror();
	test_joint_mcv_equality_conjunction();
	test_joint_mcv_range_conjunction();
	test_same_column_equality_constraints();
	test_correlated_range_conjunction_qerror();
	test_stale_summary_qerror();
	return 0;
}
