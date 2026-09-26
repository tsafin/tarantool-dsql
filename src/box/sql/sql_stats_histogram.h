#ifndef TARANTOOL_SQL_STATS_HISTOGRAM_H
#define TARANTOOL_SQL_STATS_HISTOGRAM_H

#include <stddef.h>
#include <stdint.h>

/* Values are caller-encoded; compare must implement the SQL type's total
 * ordering, including its NULL and collation rules. */
struct sql_stats_ordered_value {
	const void *data;
	size_t size;
};

typedef int (*sql_stats_value_compare_f)(const void *a, size_t a_size,
					 const void *b, size_t b_size,
					 void *context);

struct sql_stats_histogram;

struct sql_stats_histogram_bucket {
	const void *upper_bound;
	size_t upper_bound_size;
	uint64_t cumulative_count;
};

/*
 * Build an equi-depth histogram from values sorted by compare. Equal values
 * are never split across bucket boundaries, so duplicate-heavy input can
 * produce fewer than max_buckets buckets. The final boundary covers count.
 * Every boundary is deep-copied. max_bytes bounds the histogram and all
 * copied boundary values. Returns NULL for invalid/unsorted input, a budget
 * overflow, or allocation failure. This API defines no SQL encoding or
 * persistent format.
 */
struct sql_stats_histogram *
sql_stats_histogram_new(const struct sql_stats_ordered_value *values,
			uint64_t count, uint32_t max_buckets,
			sql_stats_value_compare_f compare, void *context,
			size_t max_bytes);

void
sql_stats_histogram_delete(struct sql_stats_histogram *histogram);

size_t
sql_stats_histogram_bytes(const struct sql_stats_histogram *histogram);

size_t
sql_stats_histogram_bucket_count(const struct sql_stats_histogram *histogram);

int
sql_stats_histogram_get_bucket(const struct sql_stats_histogram *histogram,
			       size_t index,
			       struct sql_stats_histogram_bucket *bucket);

#endif /* TARANTOOL_SQL_STATS_HISTOGRAM_H */
