#include "sql_stats_index_summary.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "field_def.h"
#include "index_def.h"
#include "key_def.h"
#include "qsort_arg.h"
#include "coll/coll.h"
#include "coll/coll_def.h"
#include "msgpuck.h"
#include "tuple.h"
#include "tuple_format.h"
#include "tuple_hash.h"

struct sql_stats_index_summary {
	size_t part_count;
	struct sql_stats_hll **prefixes;
	struct sql_stats_spacesaving **mcv;
	uint64_t *mcv_nonnull_rows;
	unsigned char *mcv_key_scratch;
	size_t max_mcv_value_bytes;
	unsigned char *histogram_storage;
	size_t *histogram_sizes;
	uint32_t histogram_capacity;
	uint32_t histogram_count;
	uint64_t histogram_seen;
	uint64_t histogram_random_state;
	size_t max_histogram_value_bytes;
	sql_stats_index_value_extract_f *extract;
	void *extract_context;
	struct tuple_format *format;
	struct key_def *key_def;
	uint8_t hash_bits;
	uint64_t rows;
	uint64_t bytes;
	bool failed;
};

static uint64_t
summary_next_random(struct sql_stats_index_summary *summary)
{
	uint64_t z = (summary->histogram_random_state +=
			UINT64_C(0x9e3779b97f4a7c15));
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

static uint64_t
summary_random_below(struct sql_stats_index_summary *summary, uint64_t bound)
{
	uint64_t threshold = (uint64_t)(-bound) % bound;
	uint64_t value;
	do {
		value = summary_next_random(summary);
	} while (value < threshold);
	return value % bound;
}

static int
summary_add_histogram_value(struct sql_stats_index_summary *summary,
			    const void *value, size_t value_size)
{
	if (summary->histogram_capacity == 0 || value == NULL || value_size == 0 ||
	    value_size > summary->max_histogram_value_bytes ||
	    summary->histogram_seen == UINT64_MAX)
		return -1;
	uint64_t seen = ++summary->histogram_seen;
	uint32_t slot;
	if (summary->histogram_count < summary->histogram_capacity) {
		slot = summary->histogram_count++;
	} else {
		uint64_t selected = summary_random_below(summary, seen);
		if (selected >= summary->histogram_capacity)
			return 0;
		slot = (uint32_t)selected;
	}
	unsigned char *destination = summary->histogram_storage +
		(size_t)slot * summary->max_histogram_value_bytes;
	memcpy(destination, value, value_size);
	summary->histogram_sizes[slot] = value_size;
	return 0;
}

static bool
summary_mcv_storage_bytes(size_t part_count, uint32_t capacity,
			  size_t max_value_bytes, size_t *mcv_bytes,
			  size_t *scratch_bytes)
{
	if (part_count == 0 || max_value_bytes == SIZE_MAX ||
	    part_count > SIZE_MAX / sizeof(struct sql_stats_spacesaving *) ||
	    part_count > SIZE_MAX / sizeof(uint64_t))
		return false;
	size_t one_sketch;
	if (sql_stats_spacesaving_storage_bytes(capacity, max_value_bytes + 1,
						&one_sketch) != 0)
		return false;
	size_t per_part = sizeof(struct sql_stats_spacesaving *) +
		sizeof(uint64_t);
	if (one_sketch > SIZE_MAX - per_part)
		return false;
	per_part += one_sketch;
	if (part_count > SIZE_MAX / per_part)
		return false;
	*mcv_bytes = part_count * per_part;
	*scratch_bytes = max_value_bytes + 1;
	if (*scratch_bytes > SIZE_MAX - *mcv_bytes)
		return false;
	return true;
}

static int
summary_enable_mcv(struct sql_stats_index_summary *summary,
		   uint32_t capacity, size_t max_value_bytes,
		   size_t scratch_bytes)
{
	summary->mcv = calloc(summary->part_count, sizeof(*summary->mcv));
	summary->mcv_nonnull_rows = calloc(summary->part_count,
						   sizeof(*summary->mcv_nonnull_rows));
	summary->mcv_key_scratch = malloc(scratch_bytes);
	if (summary->mcv == NULL || summary->mcv_nonnull_rows == NULL ||
	    summary->mcv_key_scratch == NULL)
		return -1;
	summary->max_mcv_value_bytes = max_value_bytes;
	for (size_t i = 0; i < summary->part_count; i++) {
		summary->mcv[i] = sql_stats_spacesaving_new(capacity);
		if (summary->mcv[i] == NULL)
			return -1;
	}
	return 0;
}

static int
summary_add_mcv(struct sql_stats_index_summary *summary, size_t part,
		uint8_t type_tag, const void *value, size_t value_size)
{
	if (summary->mcv == NULL || type_tag == 0 ||
	    (value == NULL && value_size != 0) ||
	    value_size > summary->max_mcv_value_bytes || value_size == SIZE_MAX ||
	    summary->mcv_nonnull_rows[part] == UINT64_MAX)
		return -1;
	size_t key_size = value_size + 1;
	summary->mcv_key_scratch[0] = type_tag;
	if (value_size != 0)
		memcpy(summary->mcv_key_scratch + 1, value, value_size);
	if (sql_stats_spacesaving_add(summary->mcv[part],
				       summary->mcv_key_scratch, key_size) != 0)
		return -1;
	summary->mcv_nonnull_rows[part]++;
	return 0;
}

static struct sql_stats_index_summary *
summary_new_base(size_t part_count, uint8_t precision, uint64_t seed,
		 size_t max_bytes, sql_stats_index_value_extract_f *extract,
		 void *extract_context)
{
	if (part_count == 0 || part_count > SIZE_MAX / sizeof(struct sql_stats_hll *) ||
	    precision < 4 || precision > 18)
		return NULL;
	size_t pointer_bytes = part_count * sizeof(struct sql_stats_hll *);
	size_t sketch_bytes;
	if (!sql_stats_hll_storage_bytes(precision, &sketch_bytes) ||
	    pointer_bytes > max_bytes ||
	    part_count > (max_bytes - pointer_bytes) / sketch_bytes)
		return NULL;
	struct sql_stats_index_summary *summary = calloc(1, sizeof(*summary));
	if (summary == NULL)
		return NULL;
	summary->prefixes = calloc(part_count, sizeof(*summary->prefixes));
	if (summary->prefixes == NULL)
		goto fail;
	summary->part_count = part_count;
	summary->extract = extract;
	summary->extract_context = extract_context;
	for (size_t i = 0; i < part_count; i++) {
		summary->prefixes[i] = sql_stats_hll_new(precision, seed);
		if (summary->prefixes[i] == NULL)
			goto fail;
	}
	return summary;
fail:
	sql_stats_index_summary_delete(summary);
	return NULL;
}

struct sql_stats_index_summary *
sql_stats_index_summary_new(size_t part_count, uint8_t precision,
			    uint64_t seed, size_t max_bytes,
			    sql_stats_index_value_extract_f *extract,
			    void *extract_context)
{
	if (extract == NULL)
		return NULL;
	return summary_new_base(part_count, precision, seed, max_bytes, extract,
				extract_context);
}

struct sql_stats_index_summary *
sql_stats_index_summary_new_with_mcv(size_t part_count, uint8_t precision,
				     uint64_t seed, size_t max_bytes,
				     sql_stats_index_value_extract_f *extract,
				     void *extract_context,
				     uint32_t mcv_capacity,
				     size_t max_mcv_value_bytes)
{
	if (extract == NULL)
		return NULL;
	size_t mcv_bytes, scratch_bytes;
	if (!summary_mcv_storage_bytes(part_count, mcv_capacity,
				       max_mcv_value_bytes, &mcv_bytes,
				       &scratch_bytes) || mcv_bytes > max_bytes ||
	    scratch_bytes > max_bytes - mcv_bytes)
		return NULL;
	mcv_bytes += scratch_bytes;
	struct sql_stats_index_summary *summary = summary_new_base(part_count,
		precision, seed, max_bytes - mcv_bytes, extract, extract_context);
	if (summary == NULL)
		return NULL;
	if (summary_enable_mcv(summary, mcv_capacity, max_mcv_value_bytes,
				scratch_bytes) != 0)
		goto fail;
	return summary;
fail:
	sql_stats_index_summary_delete(summary);
	return NULL;
}

struct sql_stats_index_summary *
sql_stats_index_summary_new_for_index(struct tuple_format *format,
				      const struct index_def *index_def,
				      uint8_t precision, uint64_t seed,
				      size_t max_bytes)
{
	if (format == NULL || index_def == NULL ||
	    (index_def->type != HASH && index_def->type != TREE) ||
	    index_def->opts.func_id != 0 ||
	    index_def->key_def == NULL)
		return NULL;
	const struct key_def *key_def = index_def->key_def;
	if (key_def->part_count == 0 ||
	    key_def->part_count > UINT32_MAX || key_def->is_multikey ||
	    key_def->for_func_index)
		return NULL;
	/* Keep the contract narrow: STRING and DOUBLE hash paths normalize
	 * SQL-equal representations; BOOLEAN has a one-to-one MessagePack value
	 * encoding and a value-decoding comparator; UNSIGNED has a value-decoding
	 * hash. INTEGER accepts only MessagePack's canonical signed encoding for
	 * negative values and unsigned encoding for nonnegative values, matching
	 * the value comparator; noncanonical encodings are not produced by tuples. */
	for (uint32_t i = 0; i < key_def->part_count; i++) {
		enum field_type type = key_def->parts[i].type;
		/* tuple_field() below returns the whole top-level field. A JSON-path
		 * key needs its extracted value instead, so keep it NDV-only. */
		if (key_def->parts[i].path != NULL)
			return false;
		if (type != FIELD_TYPE_STRING && type != FIELD_TYPE_DOUBLE &&
		    type != FIELD_TYPE_BOOLEAN && type != FIELD_TYPE_UNSIGNED &&
		    type != FIELD_TYPE_INTEGER)
			return NULL;
		if (type == FIELD_TYPE_STRING && key_def->parts[i].coll != NULL &&
		    key_def->parts[i].coll->hash == NULL)
			return NULL;
	}
	struct sql_stats_index_summary *summary = summary_new_base(
		key_def->part_count, precision, seed, max_bytes, NULL, NULL);
	if (summary == NULL)
		return NULL;
	summary->key_def = key_def_dup(key_def);
	if (summary->key_def == NULL) {
		sql_stats_index_summary_delete(summary);
		return NULL;
	}
	summary->format = format;
	tuple_format_ref(format);
	summary->hash_bits = 32;
	return summary;
}

bool
sql_stats_index_summary_native_mcv_supported(const struct index_def *index_def)
{
	if (index_def == NULL ||
	    (index_def->type != HASH && index_def->type != TREE) ||
	    index_def->key_def == NULL || index_def->opts.func_id != 0)
		return false;
	const struct key_def *key_def = index_def->key_def;
	if (key_def->part_count == 0 || key_def->part_count > UINT32_MAX ||
	    key_def->is_multikey || key_def->for_func_index)
		return false;
	for (uint32_t i = 0; i < key_def->part_count; i++) {
		enum field_type type = key_def->parts[i].type;
		if (type != FIELD_TYPE_STRING && type != FIELD_TYPE_DOUBLE &&
		    type != FIELD_TYPE_BOOLEAN && type != FIELD_TYPE_UNSIGNED &&
		    type != FIELD_TYPE_INTEGER)
			return false;
		if (type == FIELD_TYPE_STRING && key_def->parts[i].coll != NULL &&
		    (key_def->parts[i].coll->type != COLL_TYPE_BINARY ||
		     key_def->parts[i].coll->hash == NULL))
			return false;
	}
	return true;
}

struct sql_stats_index_summary *
sql_stats_index_summary_new_for_index_with_mcv(
	struct tuple_format *format, const struct index_def *index_def,
	uint8_t precision, uint64_t seed, size_t max_bytes,
	uint32_t mcv_capacity, size_t max_mcv_value_bytes)
{
	if (!sql_stats_index_summary_native_mcv_supported(index_def))
		return NULL;
	size_t mcv_bytes, scratch_bytes;
	if (!summary_mcv_storage_bytes(index_def->key_def->part_count,
				       mcv_capacity, max_mcv_value_bytes,
				       &mcv_bytes, &scratch_bytes) ||
	    mcv_bytes > max_bytes || scratch_bytes > max_bytes - mcv_bytes)
		return NULL;
	size_t total_mcv_bytes = mcv_bytes + scratch_bytes;
	struct sql_stats_index_summary *summary =
		sql_stats_index_summary_new_for_index(format, index_def, precision,
						      seed,
						      max_bytes - total_mcv_bytes);
	if (summary == NULL)
		return NULL;
	if (summary_enable_mcv(summary, mcv_capacity, max_mcv_value_bytes,
				scratch_bytes) != 0) {
		sql_stats_index_summary_delete(summary);
		return NULL;
	}
	return summary;
}

struct sql_stats_index_summary *
sql_stats_index_summary_new_for_index_with_mcv_histogram(
	struct tuple_format *format, const struct index_def *index_def,
	uint8_t precision, uint64_t seed, size_t max_bytes,
	uint32_t mcv_capacity, size_t max_mcv_value_bytes,
	uint32_t histogram_capacity, size_t max_histogram_value_bytes)
{
	if (histogram_capacity == 0 || max_histogram_value_bytes == 0 ||
	    histogram_capacity > SIZE_MAX / max_histogram_value_bytes)
		return NULL;
	size_t histogram_bytes = histogram_capacity *
		(sizeof(size_t) + max_histogram_value_bytes);
	if (histogram_bytes > max_bytes)
		return NULL;
	struct sql_stats_index_summary *summary =
		sql_stats_index_summary_new_for_index_with_mcv(
			format, index_def, precision, seed, max_bytes - histogram_bytes,
			mcv_capacity, max_mcv_value_bytes);
	if (summary == NULL)
		return NULL;
	summary->histogram_sizes = calloc(histogram_capacity,
						  sizeof(*summary->histogram_sizes));
	summary->histogram_storage = calloc(histogram_capacity,
						    max_histogram_value_bytes);
	if (summary->histogram_sizes == NULL ||
	    summary->histogram_storage == NULL) {
		sql_stats_index_summary_delete(summary);
		return NULL;
	}
	summary->histogram_capacity = histogram_capacity;
	summary->max_histogram_value_bytes = max_histogram_value_bytes;
	summary->histogram_random_state = seed ^ UINT64_C(0xd1b54a32d192ed03);
	return summary;
}

void
sql_stats_index_summary_delete(struct sql_stats_index_summary *summary)
{
	if (summary == NULL)
		return;
	for (size_t i = 0; i < summary->part_count; i++)
		sql_stats_hll_delete(summary->prefixes == NULL ? NULL :
				     summary->prefixes[i]);
	free(summary->prefixes);
	if (summary->mcv != NULL) {
		for (size_t i = 0; i < summary->part_count; i++)
			sql_stats_spacesaving_delete(summary->mcv[i]);
	}
	free(summary->mcv);
	free(summary->mcv_nonnull_rows);
	free(summary->mcv_key_scratch);
	free(summary->histogram_storage);
	free(summary->histogram_sizes);
	if (summary->key_def != NULL)
		key_def_delete(summary->key_def);
	if (summary->format != NULL)
		tuple_format_unref(summary->format);
	free(summary);
}

int
sql_stats_index_summary_consume(void *context, const char *tuple,
				size_t tuple_size, const uint32_t *field_ids,
				size_t field_count)
{
	struct sql_stats_index_summary *summary = context;
	if (summary == NULL || summary->failed || tuple == NULL ||
	    tuple_size == 0 || (field_count != 0 && field_ids == NULL) ||
	    summary->rows == UINT64_MAX || tuple_size > UINT64_MAX - summary->bytes) {
		if (summary != NULL)
			summary->failed = true;
		return -1;
	}
	if (summary->key_def != NULL) {
		struct tuple *native_tuple = tuple_new(summary->format, tuple,
							 tuple + tuple_size);
		if (native_tuple == NULL) {
			summary->failed = true;
			return -1;
		}
		uint32_t *hashes = malloc(summary->part_count * sizeof(*hashes));
		int rc = hashes == NULL ? -1 : tuple_hash_prefixes(native_tuple,
			summary->key_def, hashes, (uint32_t)summary->part_count);
		if (rc == 0) {
			for (size_t i = 0; i < summary->part_count; i++) {
				if (sql_stats_hll_add_u32(summary->prefixes[i], hashes[i]) != 0) {
					rc = -1;
					break;
				}
				if (summary->mcv != NULL) {
					uint32_t fieldno =
						summary->key_def->parts[i].fieldno;
					const char *field = tuple_field(native_tuple, fieldno);
					if (field == NULL || mp_typeof(*field) == MP_NIL)
						continue;
					const char *end = field;
					mp_next(&end);
					size_t value_size = end - field;
					uint8_t type_tag =
						(uint8_t)summary->key_def->parts[i].type + 1;
					if (summary_add_mcv(summary, i, type_tag, field,
							    value_size) != 0) {
						rc = -1;
						break;
					}
					if (i == 0 && summary->histogram_capacity != 0 &&
					    summary_add_histogram_value(summary, field,
								value_size) != 0) {
						rc = -1;
						break;
					}
				}
			}
		}
		/* tuple_new() returns an unreferenced runtime tuple. */
		tuple_delete(native_tuple);
		free(hashes);
		if (rc != 0) {
			summary->failed = true;
			return -1;
		}
		summary->rows++;
		summary->bytes += tuple_size;
		return 0;
	}
	struct sql_stats_hll_value *parts = calloc(summary->part_count,
							 sizeof(*parts));
	if (parts == NULL || summary->extract(summary->extract_context, tuple,
		tuple_size, field_ids, field_count, parts, summary->part_count) != 0) {
		free(parts);
		summary->failed = true;
		return -1;
	}
	for (size_t i = 0; i < summary->part_count; i++) {
		if (parts[i].data == NULL && parts[i].size != 0) {
			summary->failed = true;
			free(parts);
			return -1;
		}
		if (sql_stats_hll_add_tuple(summary->prefixes[i], parts, i + 1) != 0) {
			summary->failed = true;
			free(parts);
			return -1;
		}
		if (summary->mcv != NULL && parts[i].type_tag != 0) {
			if (summary_add_mcv(summary, i, parts[i].type_tag,
					    parts[i].data, parts[i].size) != 0) {
				summary->failed = true;
				free(parts);
				return -1;
			}
		}
	}
	free(parts);
	summary->rows++;
	summary->bytes += tuple_size;
	return 0;
}

bool
sql_stats_index_summary_has_histogram_sample(
	const struct sql_stats_index_summary *summary)
{
	return summary != NULL && !summary->failed &&
	       summary->histogram_capacity != 0;
}

static int
summary_histogram_compare(const void *lhs, const void *rhs, void *context)
{
	const struct sql_stats_ordered_value *a = lhs;
	const struct sql_stats_ordered_value *b = rhs;
	const struct key_part *part = context;
	return tuple_compare_field(a->data, b->data, (int8_t)part->type,
				   part->coll);
}

static int
summary_histogram_value_compare(const void *lhs, size_t lhs_size,
				const void *rhs, size_t rhs_size, void *context)
{
	(void)lhs_size;
	(void)rhs_size;
	const struct key_part *part = context;
	return tuple_compare_field(lhs, rhs, (int8_t)part->type, part->coll);
}

struct sql_stats_histogram *
sql_stats_index_summary_build_histogram(
	const struct sql_stats_index_summary *summary, size_t part,
	uint32_t max_buckets, size_t max_bytes)
{
	if (!sql_stats_index_summary_has_histogram_sample(summary) || part != 0 ||
	    summary->histogram_count == 0 || max_buckets == 0 || max_bytes == 0)
		return NULL;
	struct sql_stats_ordered_value *values = malloc(
		summary->histogram_count * sizeof(*values));
	if (values == NULL)
		return NULL;
	for (uint32_t i = 0; i < summary->histogram_count; i++) {
		values[i] = (struct sql_stats_ordered_value) {
			.data = summary->histogram_storage +
				(size_t)i * summary->max_histogram_value_bytes,
			.size = summary->histogram_sizes[i],
		};
	}
	struct key_part *key_part = &summary->key_def->parts[0];
	qsort_arg(values, summary->histogram_count, sizeof(*values),
		  summary_histogram_compare, key_part);
	struct sql_stats_histogram *histogram = sql_stats_histogram_new(values,
		summary->histogram_count, max_buckets,
		summary_histogram_value_compare,
		key_part, max_bytes);
	free(values);
	return histogram;
}

uint8_t
sql_stats_index_summary_hash_bits(
	const struct sql_stats_index_summary *summary)
{
	return summary == NULL || summary->failed ? 0 : summary->hash_bits;
}

uint64_t
sql_stats_index_summary_sample_rows(const struct sql_stats_index_summary *summary)
{
	return summary == NULL || summary->failed ? 0 : summary->rows;
}

uint64_t
sql_stats_index_summary_sample_bytes(const struct sql_stats_index_summary *summary)
{
	return summary == NULL || summary->failed ? 0 : summary->bytes;
}

int
sql_stats_index_summary_prefix_ndv(
	const struct sql_stats_index_summary *summary, size_t prefix_count,
	double *estimates, size_t estimate_count)
{
	if (summary == NULL || summary->failed || prefix_count == 0 ||
	    prefix_count > summary->part_count || estimate_count != prefix_count ||
	    estimates == NULL)
		return -1;
	for (size_t i = 0; i < prefix_count; i++)
		estimates[i] = sql_stats_hll_estimate(summary->prefixes[i]);
	return 0;
}

bool
sql_stats_index_summary_has_mcv(
	const struct sql_stats_index_summary *summary)
{
	return summary != NULL && !summary->failed && summary->mcv != NULL;
}

uint32_t
sql_stats_index_summary_mcv_count(
	const struct sql_stats_index_summary *summary, size_t part)
{
	if (summary == NULL || summary->failed || summary->mcv == NULL ||
	    part >= summary->part_count)
		return 0;
	return sql_stats_spacesaving_count(summary->mcv[part]);
}

int
sql_stats_index_summary_mcv_at(
	const struct sql_stats_index_summary *summary, size_t part,
	uint32_t slot, uint8_t *type_tag, const void **value, size_t *value_size,
	struct sql_stats_spacesaving_entry *entry)
{
	if (summary == NULL || summary->failed || summary->mcv == NULL ||
	    part >= summary->part_count || type_tag == NULL || value == NULL ||
	    value_size == NULL || entry == NULL)
		return -1;
	const void *key;
	size_t key_size;
	if (sql_stats_spacesaving_at(summary->mcv[part], slot, &key, &key_size,
				     entry) != 0 || key_size == 0)
		return -1;
	*type_tag = ((const unsigned char *)key)[0];
	*value = (const unsigned char *)key + 1;
	*value_size = key_size - 1;
	return 0;
}

uint64_t
sql_stats_index_summary_mcv_sample_nonnull_rows(
	const struct sql_stats_index_summary *summary, size_t part)
{
	if (summary == NULL || summary->failed || summary->mcv == NULL ||
	    part >= summary->part_count)
		return 0;
	return summary->mcv_nonnull_rows[part];
}

/*
 * Expected observed species for K equally likely values in a population of
 * N after n draws. The no-replacement branch is the probability of missing a
 * group of size N/K in a uniform reservoir sample. Fractional group sizes
 * are a continuous approximation, useful when N is not divisible by K.
 */
static double
expected_sample_ndv(double k, uint64_t population, uint64_t sample_rows,
		    bool with_replacement)
{
	if (sample_rows == 0 || k <= 1)
		return sample_rows == 0 ? 0 : 1;
	if (with_replacement) {
		double log_miss = (double)sample_rows * log1p(-1.0 / k);
		return k * -expm1(log_miss);
	}
	if (sample_rows == population)
		return k;
	double log_miss = 0;
	double group_size = (double)population / k;
	for (uint64_t i = 0; i < sample_rows; i++) {
		double remaining = (double)(population - i);
		if (group_size >= remaining)
			return k;
		log_miss += log1p(-group_size / remaining);
	}
	return k * -expm1(log_miss);
}

int
sql_stats_index_summary_population_prefix_ndv(
	const struct sql_stats_index_summary *summary,
	const struct sql_stats_sample_result *sample, size_t prefix_count,
	uint64_t *estimates, size_t estimate_count, double *confidence,
	size_t max_temp_bytes, uint64_t max_work)
{
	if (summary == NULL || summary->failed || sample == NULL ||
	    !sample->population_known || sample->rows != summary->rows ||
	    prefix_count == 0 || prefix_count > summary->part_count ||
	    estimate_count != prefix_count || estimates == NULL ||
	    confidence == NULL ||
	    (sample->with_replacement && sample->visible_population == 0 &&
	     sample->rows != 0) ||
	    (!sample->with_replacement &&
	     sample->rows > sample->visible_population))
		return -1;
	uint64_t population = sample->visible_population;
	if (population == 0) {
		if (sample->rows != 0)
			return -1;
		for (size_t i = 0; i < prefix_count; i++)
			estimates[i] = 0;
		*confidence = 1;
		return 0;
	}
	if (sample->rows == 0)
		return -1;
	uint64_t work_per_prefix = 64;
	if (!sample->with_replacement) {
		if (sample->rows > UINT64_MAX / work_per_prefix)
			return -1;
		work_per_prefix *= sample->rows;
	}
	if (prefix_count > UINT64_MAX / work_per_prefix ||
	    (uint64_t)prefix_count * work_per_prefix > max_work)
		return -1;
	if (prefix_count > SIZE_MAX / (sizeof(double) + sizeof(uint64_t)) ||
	    prefix_count * (sizeof(double) + sizeof(uint64_t)) > max_temp_bytes)
		return -1;
	double *sample_ndv = malloc(prefix_count * sizeof(*sample_ndv));
	uint64_t *population_ndv = malloc(prefix_count * sizeof(*population_ndv));
	if (sample_ndv == NULL || population_ndv == NULL) {
		free(sample_ndv);
		free(population_ndv);
		return -1;
	}
	if (sql_stats_index_summary_prefix_ndv(summary, prefix_count,
					       sample_ndv, prefix_count) != 0) {
		free(sample_ndv);
		free(population_ndv);
		return -1;
	}
	uint8_t precision = sql_stats_hll_precision(summary->prefixes[0]);
	if (precision == 0) {
		free(sample_ndv);
		free(population_ndv);
		return -1;
	}
	/* HLL RSE with a two-sigma allowance; confidence is a conservative
	 * evidence score combined with sample coverage, not a calibrated
	 * posterior probability. */
	double hll_uncertainty = 2.0 * 1.04 /
		 sqrt((double)(UINT64_C(1) << precision));
	double coverage = sample->with_replacement ?
		-expm1(-(double)sample->rows / (double)population) :
		(double)sample->rows / (double)population;
	double result_confidence = fmax(0, 1.0 - hll_uncertainty) * coverage;
	uint8_t hash_bits = summary->hash_bits;
	for (size_t i = 0; i < prefix_count; i++) {
		double observed = fmin(sample_ndv[i], (double)sample->rows);
		if (!isfinite(observed) || observed <= 0) {
			free(sample_ndv);
			free(population_ndv);
			return -1;
		}
		double lo = fmax(1.0, observed);
		double hi = (double)population;
		if (lo > hi) {
			free(sample_ndv);
			free(population_ndv);
			return -1;
		}
		for (int step = 0; step < 64; step++) {
			double mid = lo + (hi - lo) / 2;
			if (expected_sample_ndv(mid, population, sample->rows,
						sample->with_replacement) < observed)
				lo = mid;
			else
				hi = mid;
		}
		double estimate = lo + (hi - lo) / 2;
		if (estimate >= (double)population) {
			population_ndv[i] = population;
		} else {
			population_ndv[i] = (uint64_t)floor(estimate + 0.5);
			if (population_ndv[i] < 1)
				population_ndv[i] = 1;
		}
		if (hash_bits != 0 && observed > 1) {
			double collision_risk = observed * (observed - 1) /
				(2.0 * exp2(hash_bits));
			result_confidence *= fmax(0, 1.0 - collision_risk);
		}
	}
	memcpy(estimates, population_ndv,
	       prefix_count * sizeof(*population_ndv));
	*confidence = result_confidence;
	free(sample_ndv);
	free(population_ndv);
	return 0;
}
