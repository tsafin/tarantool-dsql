#include "sql_stats_histogram.h"

#include <stdlib.h>
#include <string.h>

struct histogram_bucket {
	unsigned char *upper_bound;
	size_t upper_bound_size;
	uint64_t cumulative_count;
};

struct sql_stats_histogram {
	size_t bytes;
	size_t count;
	struct histogram_bucket buckets[];
};

static int
compare_values(sql_stats_value_compare_f compare, void *context,
	       const struct sql_stats_ordered_value *a,
	       const struct sql_stats_ordered_value *b)
{
	return compare(a->data, a->size, b->data, b->size, context);
}

static uint64_t
quantile_rank(uint64_t count, uint32_t buckets, uint32_t bucket)
{
	/* ceil(count * bucket / buckets), without overflowing the product. */
	uint64_t quotient = count / buckets;
	uint64_t remainder = count % buckets;
	return quotient * bucket + (remainder * bucket + buckets - 1) / buckets;
}

struct sql_stats_histogram *
sql_stats_histogram_new(const struct sql_stats_ordered_value *values,
			uint64_t count, uint32_t max_buckets,
			sql_stats_value_compare_f compare, void *context,
			size_t max_bytes)
{
	if (values == NULL || count == 0 || count > SIZE_MAX || max_buckets == 0 ||
	    compare == NULL || max_bytes < sizeof(struct sql_stats_histogram))
		return NULL;
	for (uint64_t i = 0; i < count; i++) {
		if ((values[i].data == NULL && values[i].size != 0) ||
		    (i != 0 && compare_values(compare, context, &values[i - 1],
					     &values[i]) > 0))
			return NULL;
	}
	uint32_t target = count < max_buckets ? (uint32_t)count : max_buckets;
	/* First pass determines the number of distinct-value-safe boundaries. */
	size_t bucket_count = 0;
	uint64_t previous_rank = 0;
	for (uint32_t i = 1; i <= target; i++) {
		uint64_t rank = quantile_rank(count, target, i);
		while (rank < count && compare_values(compare, context,
					&values[rank - 1], &values[rank]) == 0)
			rank++;
		if (rank > previous_rank) {
			bucket_count++;
			previous_rank = rank;
		}
	}
	if (bucket_count > (SIZE_MAX - sizeof(struct sql_stats_histogram)) /
		    sizeof(struct histogram_bucket))
		return NULL;
	size_t base = sizeof(struct sql_stats_histogram) +
		bucket_count * sizeof(struct histogram_bucket);
	if (base > max_bytes)
		return NULL;
	struct sql_stats_histogram *hist = calloc(1, base);
	if (hist == NULL)
		return NULL;
	hist->bytes = base;
	previous_rank = 0;
	size_t out = 0;
	for (uint32_t i = 1; i <= target; i++) {
		uint64_t rank = quantile_rank(count, target, i);
		while (rank < count && compare_values(compare, context,
					&values[rank - 1], &values[rank]) == 0)
			rank++;
		if (rank == previous_rank)
			continue;
		const struct sql_stats_ordered_value *value = &values[rank - 1];
		size_t allocation_size = value->size == 0 ? 1 : value->size;
		if (allocation_size > max_bytes - hist->bytes) {
			sql_stats_histogram_delete(hist);
			return NULL;
		}
		unsigned char *copy = malloc(allocation_size);
		if (copy == NULL) {
			sql_stats_histogram_delete(hist);
			return NULL;
		}
		if (value->size != 0)
			memcpy(copy, value->data, value->size);
		hist->buckets[out++] = (struct histogram_bucket){copy, value->size,
								  rank};
		hist->bytes += allocation_size;
		previous_rank = rank;
	}
	hist->count = out;
	return hist;
}

void
sql_stats_histogram_delete(struct sql_stats_histogram *hist)
{
	if (hist == NULL)
		return;
	for (size_t i = 0; i < hist->count; i++)
		free(hist->buckets[i].upper_bound);
	free(hist);
}

size_t
sql_stats_histogram_bytes(const struct sql_stats_histogram *hist)
{
	return hist == NULL ? 0 : hist->bytes;
}

size_t
sql_stats_histogram_bucket_count(const struct sql_stats_histogram *hist)
{
	return hist == NULL ? 0 : hist->count;
}

int
sql_stats_histogram_get_bucket(const struct sql_stats_histogram *hist,
			       size_t index,
			       struct sql_stats_histogram_bucket *bucket)
{
	if (hist == NULL || bucket == NULL || index >= hist->count)
		return -1;
	*bucket = (struct sql_stats_histogram_bucket){
		hist->buckets[index].upper_bound,
		hist->buckets[index].upper_bound_size,
		hist->buckets[index].cumulative_count,
	};
	return 0;
}
