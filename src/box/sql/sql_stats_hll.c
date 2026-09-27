#include "sql_stats_hll.h"

#include <math.h>
#include <stdlib.h>

struct sql_stats_hll {
	uint64_t seed;
	size_t register_count;
	uint8_t precision;
	uint8_t registers[];
};

bool
sql_stats_hll_storage_bytes(uint8_t precision, size_t *bytes)
{
	if (bytes == NULL || precision < 4 || precision > 18)
		return false;
	*bytes = sizeof(struct sql_stats_hll) + ((size_t)1 << precision);
	return true;
}

static uint64_t
sql_stats_hll_hash_init(size_t size, uint64_t seed)
{
	return seed ^ (uint64_t)size ^ UINT64_C(0x9e3779b97f4a7c15);
}

static uint64_t
sql_stats_hll_hash_bytes(uint64_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(0x100000001b3);
		hash ^= hash >> 29;
	}
	return hash;
}

/* Stable, endian-independent byte hash with a SplitMix64 finalizer. */
static uint64_t
sql_stats_hll_hash_finish(uint64_t hash)
{
	hash += UINT64_C(0x9e3779b97f4a7c15);
	hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
	return hash ^ (hash >> 31);
}

static uint64_t
sql_stats_hll_hash(const void *data, size_t size, uint64_t seed)
{
	uint64_t hash = sql_stats_hll_hash_init(size, seed);
	hash = sql_stats_hll_hash_bytes(hash, data, size);
	return sql_stats_hll_hash_finish(hash);
}

static void
sql_stats_hll_add_hash(struct sql_stats_hll *hll, uint64_t hash)
{
	uint64_t index = hash >> (64 - hll->precision);
	uint64_t suffix = hash << hll->precision;
	unsigned max_rank = (unsigned)(64 - hll->precision + 1);
	unsigned rank = suffix == 0 ? max_rank :
		(unsigned)__builtin_clzll(suffix) + 1;
	if (rank > max_rank)
		rank = max_rank;
	if (hll->registers[index] < rank)
		hll->registers[index] = rank;
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
	sql_stats_hll_add_hash(hll,
		sql_stats_hll_hash(data, size, hll->seed));
	return 0;
}

static void
sql_stats_hll_encode_u64(unsigned char out[8], uint64_t value)
{
	for (int i = 0; i < 8; i++) {
		out[i] = (unsigned char)value;
		value >>= 8;
	}
}

int
sql_stats_hll_add_tuple(struct sql_stats_hll *hll,
			const struct sql_stats_hll_value *values,
			size_t value_count)
{
	if (hll == NULL || (value_count != 0 && values == NULL))
		return -1;
	size_t encoded_size = 8;
	for (size_t i = 0; i < value_count; i++) {
		if ((values[i].data == NULL && values[i].size != 0) ||
		    encoded_size > SIZE_MAX - 9 ||
		    values[i].size > SIZE_MAX - encoded_size - 9)
			return -1;
		encoded_size += 9 + values[i].size;
	}
	uint64_t hash = sql_stats_hll_hash_init(encoded_size, hll->seed);
	unsigned char encoded_count[8];
	sql_stats_hll_encode_u64(encoded_count, (uint64_t)value_count);
	hash = sql_stats_hll_hash_bytes(hash, encoded_count,
					 sizeof(encoded_count));
	for (size_t i = 0; i < value_count; i++) {
		unsigned char encoded_size[8];
		sql_stats_hll_encode_u64(encoded_size, values[i].size);
		hash = sql_stats_hll_hash_bytes(hash, &values[i].type_tag, 1);
		hash = sql_stats_hll_hash_bytes(hash, encoded_size,
						 sizeof(encoded_size));
		hash = sql_stats_hll_hash_bytes(hash, values[i].data,
						 values[i].size);
	}
	sql_stats_hll_add_hash(hll, sql_stats_hll_hash_finish(hash));
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
