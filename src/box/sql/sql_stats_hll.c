#include "sql_stats_hll.h"

#include <math.h>
#include <stdlib.h>

struct sql_stats_hll {
	uint64_t seed;
	size_t register_count;
	uint8_t precision;
	uint8_t registers[];
};

/* Stable, endian-independent byte hash with a SplitMix64 finalizer. */
static uint64_t
sql_stats_hll_hash(const void *data, size_t size, uint64_t seed)
{
	const unsigned char *bytes = data;
	uint64_t hash = seed ^ (uint64_t)size ^ UINT64_C(0x9e3779b97f4a7c15);
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(0x100000001b3);
		hash ^= hash >> 29;
	}
	hash += UINT64_C(0x9e3779b97f4a7c15);
	hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
	return hash ^ (hash >> 31);
}

struct sql_stats_hll *
sql_stats_hll_new(uint8_t precision, uint64_t seed)
{
	if (precision < 4 || precision > 18)
		return NULL;
	size_t count = (size_t)1 << precision;
	struct sql_stats_hll *hll = calloc(1, sizeof(*hll) + count);
	if (hll == NULL)
		return NULL;
	hll->seed = seed;
	hll->register_count = count;
	hll->precision = precision;
	return hll;
}

void
sql_stats_hll_delete(struct sql_stats_hll *hll)
{
	free(hll);
}

int
sql_stats_hll_add(struct sql_stats_hll *hll, const void *data, size_t size)
{
	if (hll == NULL || (data == NULL && size != 0))
		return -1;
	uint64_t hash = sql_stats_hll_hash(data, size, hll->seed);
	uint64_t index = hash >> (64 - hll->precision);
	uint64_t suffix = hash << hll->precision;
	unsigned max_rank = (unsigned)(64 - hll->precision + 1);
	unsigned rank = suffix == 0 ? max_rank :
		(unsigned)__builtin_clzll(suffix) + 1;
	if (rank > max_rank)
		rank = max_rank;
	if (hll->registers[index] < rank)
		hll->registers[index] = rank;
	return 0;
}

int
sql_stats_hll_merge(struct sql_stats_hll *dst,
		    const struct sql_stats_hll *src)
{
	if (dst == NULL || src == NULL || dst->precision != src->precision ||
	    dst->seed != src->seed)
		return -1;
	for (size_t i = 0; i < dst->register_count; i++) {
		if (dst->registers[i] < src->registers[i])
			dst->registers[i] = src->registers[i];
	}
	return 0;
}

double
sql_stats_hll_estimate(const struct sql_stats_hll *hll)
{
	if (hll == NULL)
		return 0;
	double sum = 0;
	size_t zero_count = 0;
	for (size_t i = 0; i < hll->register_count; i++) {
		uint8_t reg = hll->registers[i];
		sum += ldexp(1.0, -(int)reg);
		zero_count += reg == 0;
	}
	double m = (double)hll->register_count;
	double alpha = 0.7213 / (1.0 + 1.079 / m);
	double estimate = alpha * m * m / sum;
	if (estimate <= 2.5 * m && zero_count != 0)
		estimate = m * log(m / (double)zero_count);
	return estimate;
}

uint8_t
sql_stats_hll_precision(const struct sql_stats_hll *hll)
{
	return hll == NULL ? 0 : hll->precision;
}
