#include "box/sql/sql_stats_histogram.h"

#include <stdint.h>
#include <string.h>

#include "unit.h"

static int
int_compare(const void *a, size_t a_size, const void *b, size_t b_size,
	    void *context)
{
	(void)context;
	if (a_size != sizeof(int) || b_size != sizeof(int))
		return 0;
	int av, bv;
	memcpy(&av, a, sizeof(av));
	memcpy(&bv, b, sizeof(bv));
	return (av > bv) - (av < bv);
}

static void
test_equi_depth_and_copy(void)
{
	plan(8);
	header();
	int data[] = {1, 1, 2, 3, 4, 5, 6, 7};
	struct sql_stats_ordered_value values[8];
	for (size_t i = 0; i < 8; i++)
		values[i] = (struct sql_stats_ordered_value){&data[i], sizeof(data[i])};
	struct sql_stats_histogram *hist = sql_stats_histogram_new(
		values, 8, 4, int_compare, NULL, 1024);
	ok(hist != NULL, "sorted sample builds histogram");
	ok(sql_stats_histogram_bucket_count(hist) == 4,
	   "requested bucket count retained when values permit");
	struct sql_stats_histogram_bucket bucket;
	fail_if(sql_stats_histogram_get_bucket(hist, 0, &bucket) != 0);
	int upper;
	memcpy(&upper, bucket.upper_bound, sizeof(upper));
	ok(upper == 1 && bucket.cumulative_count == 2,
	   "first quantile boundary follows the duplicate run");
	fail_if(sql_stats_histogram_get_bucket(hist, 1, &bucket) != 0);
	memcpy(&upper, bucket.upper_bound, sizeof(upper));
	ok(upper == 3 && bucket.cumulative_count == 4,
	   "second boundary carries cumulative sample count");
	fail_if(sql_stats_histogram_get_bucket(hist, 3, &bucket) != 0);
	memcpy(&upper, bucket.upper_bound, sizeof(upper));
	ok(upper == 7 && bucket.cumulative_count == 8,
	   "last bucket covers complete sample");
	ok(sql_stats_histogram_bytes(hist) <= 1024,
	   "reported allocation stays within budget");
	data[2] = 99;
	fail_if(sql_stats_histogram_get_bucket(hist, 0, &bucket) != 0);
	memcpy(&upper, bucket.upper_bound, sizeof(upper));
	ok(upper == 1, "boundary is deep-copied from sample");
	ok(sql_stats_histogram_get_bucket(hist, 4, &bucket) == -1,
	   "out-of-range bucket rejected");
	sql_stats_histogram_delete(hist);
	sql_stats_histogram_delete(NULL);
	footer();
	check_plan();
}

static void
test_duplicates_and_invalid_input(void)
{
	plan(7);
	header();
	int data[] = {1, 1, 1, 1, 2, 2, 2, 3};
	struct sql_stats_ordered_value values[8];
	for (size_t i = 0; i < 8; i++)
		values[i] = (struct sql_stats_ordered_value){&data[i], sizeof(data[i])};
	struct sql_stats_histogram *hist = sql_stats_histogram_new(
		values, 8, 8, int_compare, NULL, 1024);
	ok(hist != NULL, "duplicate-heavy sample accepted");
	ok(sql_stats_histogram_bucket_count(hist) == 3,
	   "equal values are never split, reducing bucket count");
	struct sql_stats_histogram_bucket bucket;
	fail_if(sql_stats_histogram_get_bucket(hist, 0, &bucket) != 0);
	ok(bucket.cumulative_count == 4, "first duplicate group is intact");
	fail_if(sql_stats_histogram_get_bucket(hist, 2, &bucket) != 0);
	ok(bucket.cumulative_count == 8, "final cumulative count equals sample size");
	sql_stats_histogram_delete(hist);
	int unsorted[] = {2, 1};
	struct sql_stats_ordered_value bad[] = {
		{&unsorted[0], sizeof(int)}, {&unsorted[1], sizeof(int)},
	};
	ok(sql_stats_histogram_new(bad, 2, 2, int_compare, NULL, 1024) == NULL,
	   "unsorted values rejected");
	ok(sql_stats_histogram_new(values, 8, 2, int_compare, NULL, 1) == NULL,
	   "budget too small rejected");
	ok(sql_stats_histogram_new(values, 8, 0, int_compare, NULL, 1024) == NULL,
	   "zero bucket limit rejected");
	footer();
	check_plan();
}

int
main(void)
{
	test_equi_depth_and_copy();
	test_duplicates_and_invalid_input();
	return 0;
}
