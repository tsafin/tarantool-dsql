#include "box/sql/sql_stats_spacesaving.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "unit.h"

static void
test_bounds_and_ties(void)
{
	plan(7);
	header();
	struct sql_stats_spacesaving *s = sql_stats_spacesaving_new(8);
	struct sql_stats_spacesaving *same = sql_stats_spacesaving_new(2);
	ok(s != NULL && same != NULL, "bounded summaries allocate");
	ok(sql_stats_spacesaving_capacity(s) == 8, "capacity is exposed");
	for (unsigned i = 0; i < 1000; i++)
		fail_if(sql_stats_spacesaving_add(s, "hot", 3) != 0);
	char key[32];
	for (unsigned i = 0; i < 400; i++) {
		int n = snprintf(key, sizeof(key), "cold-%u", i);
		fail_if(n <= 0 || (size_t)n >= sizeof(key));
		fail_if(sql_stats_spacesaving_add(s, key, (size_t)n) != 0);
	}
	struct sql_stats_spacesaving_entry e;
	ok(sql_stats_spacesaving_query(s, "hot", 3, &e) == 1,
	   "heavy hitter remains tracked");
	ok(e.estimate >= 1000 && e.estimate - e.error <= 1000,
	   "heavy hitter frequency is within its reported error interval");
	ok(e.error <= 1400 / 8, "SpaceSaving error is bounded by N / capacity");
	ok(sql_stats_spacesaving_query(s, "not-present", 11, &e) == 0,
	   "query distinguishes an untracked key");
	fail_if(sql_stats_spacesaving_add(same, "b", 1) != 0 ||
		sql_stats_spacesaving_add(same, "a", 1) != 0 ||
		sql_stats_spacesaving_add(same, "c", 1) != 0);
	ok(sql_stats_spacesaving_query(same, "a", 1, &e) == 0 &&
	   sql_stats_spacesaving_query(same, "c", 1, &e) == 1,
	   "equal counters evict lexicographically first key");
	sql_stats_spacesaving_delete(s);
	sql_stats_spacesaving_delete(same);
	footer();
	check_plan();
}

static void
test_eviction_error_bounds(void)
{
	plan(2);
	header();
	struct sql_stats_spacesaving *s = sql_stats_spacesaving_new(4);
	ok(s != NULL, "small summary allocates for repeated evictions");
	if (s != NULL) {
		char key[8];
		for (unsigned i = 0; i < 100; i++) {
			int n = snprintf(key, sizeof(key), "v%02u", i % 63);
			fail_if(n <= 0 || (size_t)n >= sizeof(key));
			fail_if(sql_stats_spacesaving_add(s, key, (size_t)n) != 0);
		}
		int bounded = sql_stats_spacesaving_count(s) == 4;
		for (uint32_t i = 0; bounded && i < 4; i++) {
			const void *key;
			size_t key_size;
			struct sql_stats_spacesaving_entry entry;
			bounded = sql_stats_spacesaving_at(s, i, &key, &key_size,
						   &entry) == 0 &&
				entry.error <= entry.estimate &&
				entry.error <= 100 / 4 && key != NULL && key_size != 0;
		}
		ok(bounded, "every candidate has a valid N/capacity error interval");
	} else {
		ok(false, "every candidate has a valid N/capacity error interval");
	}
	sql_stats_spacesaving_delete(s);
	footer();
	check_plan();
}

static void
test_merge(void)
{
	plan(5);
	header();
	struct sql_stats_spacesaving *left = sql_stats_spacesaving_new(16);
	struct sql_stats_spacesaving *right = sql_stats_spacesaving_new(16);
	fail_if(left == NULL || right == NULL);
	for (unsigned i = 0; i < 700; i++) {
		struct sql_stats_spacesaving *s = i % 2 == 0 ? left : right;
		fail_if(sql_stats_spacesaving_add(s, "shared-hot", 10) != 0);
	}
	char key[32];
	for (unsigned i = 0; i < 200; i++) {
		int n = snprintf(key, sizeof(key), "left-%u", i);
		fail_if(n <= 0 || (size_t)n >= sizeof(key));
		fail_if(sql_stats_spacesaving_add(left, key, (size_t)n) != 0);
		n = snprintf(key, sizeof(key), "right-%u", i);
		fail_if(n <= 0 || (size_t)n >= sizeof(key));
		fail_if(sql_stats_spacesaving_add(right, key, (size_t)n) != 0);
	}
	ok(sql_stats_spacesaving_merge(left, right) == 0,
	   "compatible summaries merge");
	struct sql_stats_spacesaving_entry e;
	ok(sql_stats_spacesaving_query(left, "shared-hot", 10, &e) == 1,
	   "merged heavy hitter is tracked");
	ok(e.estimate >= 700 && e.estimate - e.error <= 700,
	   "merged heavy hitter remains inside conservative error interval");
	ok(sql_stats_spacesaving_merge(left, left) == 0,
	   "self merge is a no-op");
	struct sql_stats_spacesaving *small = sql_stats_spacesaving_new(2);
	ok(sql_stats_spacesaving_merge(left, small) == 0,
	   "different capacities merge into destination capacity");
	sql_stats_spacesaving_delete(small);
	sql_stats_spacesaving_delete(left);
	sql_stats_spacesaving_delete(right);
	footer();
	check_plan();
}

int
main(void)
{
	test_bounds_and_ties();
	test_eviction_error_bounds();
	test_merge();
	return 0;
}
