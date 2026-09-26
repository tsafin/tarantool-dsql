#include "box/sql/sql_stats_hll.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "unit.h"

static void
test_empty_and_validation(void)
{
	plan(6);
	header();
	struct sql_stats_hll *hll = sql_stats_hll_new(12, 42);
	ok(hll != NULL, "valid precision allocates sketch");
	ok(sql_stats_hll_precision(hll) == 12, "precision is preserved");
	ok(sql_stats_hll_estimate(hll) == 0, "empty sketch estimates zero");
	ok(sql_stats_hll_new(3, 42) == NULL, "precision below range rejected");
	ok(sql_stats_hll_new(19, 42) == NULL, "precision above range rejected");
	ok(sql_stats_hll_add(hll, NULL, 1) == -1 &&
	   sql_stats_hll_add(NULL, "x", 1) == -1, "invalid add rejected");
	sql_stats_hll_delete(hll);
	footer();
	check_plan();
}

static void
test_accuracy_and_determinism(void)
{
	plan(3);
	header();
	struct sql_stats_hll *a = sql_stats_hll_new(12, 0x12345678);
	struct sql_stats_hll *b = sql_stats_hll_new(12, 0x12345678);
	fail_if(a == NULL || b == NULL);
	char value[32];
	for (uint64_t i = 0; i < 100000; i++) {
		int size = snprintf(value, sizeof(value), "value-%" PRIu64, i);
		fail_if(size < 0 || (size_t)size >= sizeof(value));
		fail_if(sql_stats_hll_add(a, value, (size_t)size) != 0);
		fail_if(sql_stats_hll_add(b, value, (size_t)size) != 0);
	}
	double estimate_a = sql_stats_hll_estimate(a);
	double estimate_b = sql_stats_hll_estimate(b);
	double relative_error = fabs(estimate_a - 100000.0) / 100000.0;
	ok(estimate_a == estimate_b, "fixed seed gives repeatable estimate");
	ok(relative_error < 0.05,
	   "100k estimate within 5%% (got %.2f, error %.3f%%)",
	   estimate_a, relative_error * 100);
	ok(estimate_a > 0, "non-empty estimate is positive");
	sql_stats_hll_delete(a);
	sql_stats_hll_delete(b);
	footer();
	check_plan();
}

static void
test_merge(void)
{
	plan(4);
	header();
	struct sql_stats_hll *left = sql_stats_hll_new(12, 17);
	struct sql_stats_hll *right = sql_stats_hll_new(12, 17);
	struct sql_stats_hll *whole = sql_stats_hll_new(12, 17);
	struct sql_stats_hll *wrong_seed = sql_stats_hll_new(12, 18);
	struct sql_stats_hll *wrong_precision = sql_stats_hll_new(11, 17);
	fail_if(left == NULL || right == NULL || whole == NULL ||
		wrong_seed == NULL || wrong_precision == NULL);
	char value[32];
	for (uint64_t i = 0; i < 100000; i++) {
		int size = snprintf(value, sizeof(value), "merge-%" PRIu64, i);
		fail_if(size < 0 || (size_t)size >= sizeof(value));
		fail_if(sql_stats_hll_add(i < 50000 ? left : right,
					  value, (size_t)size) != 0);
		fail_if(sql_stats_hll_add(whole, value, (size_t)size) != 0);
	}
	fail_if(sql_stats_hll_merge(left, right) != 0);
	ok(fabs(sql_stats_hll_estimate(left) -
		sql_stats_hll_estimate(whole)) < 1e-9,
	   "merged estimate equals single-pass estimate");
	ok(sql_stats_hll_merge(left, wrong_seed) == -1,
	   "merge rejects different seeds");
	ok(sql_stats_hll_merge(left, wrong_precision) == -1,
	   "merge rejects different precision");
	ok(sql_stats_hll_merge(NULL, right) == -1,
	   "merge rejects NULL sketch");
	sql_stats_hll_delete(left);
	sql_stats_hll_delete(right);
	sql_stats_hll_delete(whole);
	sql_stats_hll_delete(wrong_seed);
	sql_stats_hll_delete(wrong_precision);
	footer();
	check_plan();
}

int
main(void)
{
	test_empty_and_validation();
	test_accuracy_and_determinism();
	test_merge();
	return 0;
}
