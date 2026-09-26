#include "sql_stats_snapshot.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct sql_stats_index {
	uint32_t index_id;
	uint64_t tuple_count;
	size_t prefix_count;
	uint64_t *distinct_prefixes;
};

struct sql_stats_relation {
	uint32_t space_id;
	double row_count;
	double average_row_width;
	double confidence;
	enum sql_stats_cardinality_semantics cardinality_semantics;
	uint64_t collected_at;
	uint64_t modification_epoch;
	size_t index_count;
	struct sql_stats_index *indexes;
};

struct sql_stats_snapshot {
	atomic_size_t ref_count;
	uint64_t catalog_version;
	uint64_t schema_version;
	size_t bytes;
	size_t relation_count;
	struct sql_stats_relation *relations;
};

static int
compare_relation(const void *lhs, const void *rhs)
{
	const struct sql_stats_relation *a = lhs;
	const struct sql_stats_relation *b = rhs;
	return (a->space_id > b->space_id) - (a->space_id < b->space_id);
}

static int
compare_index(const void *lhs, const void *rhs)
{
	const struct sql_stats_index *a = lhs;
	const struct sql_stats_index *b = rhs;
	return (a->index_id > b->index_id) - (a->index_id < b->index_id);
}

static bool
add_bytes(size_t *total, size_t amount, size_t limit)
{
	if (*total > limit || amount > limit - *total)
		return false;
	*total += amount;
	return true;
}

static void
destroy_relations(struct sql_stats_relation *relations, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		for (size_t j = 0; j < relations[i].index_count; j++)
			free(relations[i].indexes[j].distinct_prefixes);
		free(relations[i].indexes);
	}
	free(relations);
}

struct sql_stats_snapshot *
sql_stats_snapshot_new(uint64_t catalog_version, uint64_t schema_version,
		       const struct sql_stats_relation_input *inputs,
		       size_t relation_count, size_t max_bytes)
{
	if (max_bytes == 0 || (relation_count != 0 && inputs == NULL) ||
	    relation_count > SIZE_MAX / sizeof(struct sql_stats_relation))
		return NULL;
	size_t bytes = sizeof(struct sql_stats_snapshot);
	if (!add_bytes(&bytes, relation_count * sizeof(struct sql_stats_relation),
		       max_bytes))
		return NULL;
	struct sql_stats_snapshot *snapshot = calloc(1, sizeof(*snapshot));
	if (snapshot == NULL)
		return NULL;
	snapshot->relations = relation_count == 0 ? NULL :
		calloc(relation_count, sizeof(*snapshot->relations));
	if (relation_count != 0 && snapshot->relations == NULL) {
		free(snapshot);
		return NULL;
	}
	snapshot->catalog_version = catalog_version;
	snapshot->schema_version = schema_version;
	snapshot->relation_count = relation_count;
	snapshot->bytes = bytes;
	atomic_init(&snapshot->ref_count, 1);
	for (size_t i = 0; i < relation_count; i++) {
		const struct sql_stats_relation_input *in = &inputs[i];
		struct sql_stats_relation *out = &snapshot->relations[i];
		if (!isfinite(in->row_count) || in->row_count < 0 ||
		    !isfinite(in->average_row_width) || in->average_row_width < 0 ||
		    !isfinite(in->confidence) || in->confidence < 0 ||
		    in->confidence > 1 || in->cardinality_semantics <
		    SQL_STATS_CARDINALITY_VISIBLE_ROWS || in->cardinality_semantics >
		    SQL_STATS_CARDINALITY_ESTIMATE ||
		    (in->index_count != 0 && in->indexes == NULL) ||
		    in->index_count > SIZE_MAX / sizeof(struct sql_stats_index))
			goto error;
		out->space_id = in->space_id;
		out->row_count = in->row_count;
		out->average_row_width = in->average_row_width;
		out->confidence = in->confidence;
		out->cardinality_semantics = in->cardinality_semantics;
		out->collected_at = in->collected_at;
		out->modification_epoch = in->modification_epoch;
		out->index_count = in->index_count;
		if (!add_bytes(&snapshot->bytes,
			       in->index_count * sizeof(struct sql_stats_index),
			       max_bytes))
			goto error;
		out->indexes = in->index_count == 0 ? NULL :
			calloc(in->index_count, sizeof(*out->indexes));
		if (in->index_count != 0 && out->indexes == NULL)
			goto error;
		for (size_t j = 0; j < in->index_count; j++) {
			const struct sql_stats_index_input *index_in = &in->indexes[j];
			struct sql_stats_index *index = &out->indexes[j];
			if ((index_in->prefix_count != 0 &&
			     index_in->distinct_prefixes == NULL) ||
			    index_in->prefix_count > SIZE_MAX / sizeof(uint64_t))
				goto error;
			index->index_id = index_in->index_id;
			index->tuple_count = index_in->tuple_count;
			index->prefix_count = index_in->prefix_count;
			if (!add_bytes(&snapshot->bytes,
				       index_in->prefix_count * sizeof(uint64_t),
				       max_bytes))
				goto error;
			if (index_in->prefix_count != 0) {
				index->distinct_prefixes = malloc(index_in->prefix_count *
								 sizeof(uint64_t));
				if (index->distinct_prefixes == NULL)
					goto error;
				memcpy(index->distinct_prefixes,
				       index_in->distinct_prefixes,
				       index_in->prefix_count * sizeof(uint64_t));
			}
			uint64_t previous = 0;
			for (size_t k = 0; k < index->prefix_count; k++) {
				uint64_t value = index->distinct_prefixes[k];
				if (value == 0 || value > index->tuple_count ||
				    (k != 0 && value < previous))
					goto error;
				previous = value;
			}
		}
		if (out->index_count > 1)
			qsort(out->indexes, out->index_count, sizeof(*out->indexes),
			      compare_index);
		for (size_t j = 1; j < out->index_count; j++) {
			if (out->indexes[j - 1].index_id == out->indexes[j].index_id)
				goto error;
		}
	}
	if (relation_count > 1)
		qsort(snapshot->relations, relation_count,
		      sizeof(*snapshot->relations), compare_relation);
	for (size_t i = 1; i < relation_count; i++) {
		if (snapshot->relations[i - 1].space_id ==
		    snapshot->relations[i].space_id)
			goto error;
	}
	return snapshot;
error:
	destroy_relations(snapshot->relations, relation_count);
	free(snapshot);
	return NULL;
}

void
sql_stats_snapshot_retain(struct sql_stats_snapshot *snapshot)
{
	if (snapshot != NULL)
		atomic_fetch_add_explicit(&snapshot->ref_count, 1,
					  memory_order_relaxed);
}

void
sql_stats_snapshot_release(struct sql_stats_snapshot *snapshot)
{
	if (snapshot != NULL && atomic_fetch_sub_explicit(&snapshot->ref_count, 1,
			memory_order_acq_rel) == 1) {
		destroy_relations(snapshot->relations, snapshot->relation_count);
		free(snapshot);
	}
}

uint32_t sql_stats_snapshot_api_version(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : 1; }
uint64_t sql_stats_snapshot_catalog_version(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->catalog_version; }
uint64_t sql_stats_snapshot_schema_version(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->schema_version; }
size_t sql_stats_snapshot_bytes(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->bytes; }
size_t sql_stats_snapshot_relation_count(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->relation_count; }

enum sql_stats_lookup_status
sql_stats_snapshot_get_relation(const struct sql_stats_snapshot *s,
				uint64_t schema_version, uint32_t space_id,
				const struct sql_stats_relation **result)
{
	if (result != NULL)
		*result = NULL;
	if (s == NULL || result == NULL)
		return SQL_STATS_LOOKUP_MISSING;
	if (s->schema_version != schema_version)
		return SQL_STATS_LOOKUP_STALE;
	const struct sql_stats_relation key = {.space_id = space_id};
	const struct sql_stats_relation *relation = bsearch(&key, s->relations,
		s->relation_count, sizeof(*s->relations), compare_relation);
	if (relation == NULL)
		return SQL_STATS_LOOKUP_MISSING;
	*result = relation;
	return SQL_STATS_LOOKUP_AVAILABLE;
}

enum sql_stats_lookup_status
sql_stats_relation_get_index(const struct sql_stats_relation *r,
			     uint32_t index_id,
			     const struct sql_stats_index **result)
{
	if (result != NULL)
		*result = NULL;
	if (r == NULL || result == NULL)
		return SQL_STATS_LOOKUP_MISSING;
	const struct sql_stats_index key = {.index_id = index_id};
	const struct sql_stats_index *index = bsearch(&key, r->indexes,
		r->index_count, sizeof(*r->indexes), compare_index);
	if (index == NULL)
		return SQL_STATS_LOOKUP_MISSING;
	*result = index;
	return SQL_STATS_LOOKUP_AVAILABLE;
}

uint32_t sql_stats_relation_space_id(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->space_id; }
double sql_stats_relation_row_count(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->row_count; }
double sql_stats_relation_average_row_width(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->average_row_width; }
double sql_stats_relation_confidence(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->confidence; }
enum sql_stats_cardinality_semantics
sql_stats_relation_cardinality_semantics(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->cardinality_semantics; }
uint64_t sql_stats_relation_collected_at(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->collected_at; }
uint64_t sql_stats_relation_modification_epoch(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->modification_epoch; }
uint32_t sql_stats_index_id(const struct sql_stats_index *i)
{ return i == NULL ? 0 : i->index_id; }
uint64_t sql_stats_index_tuple_count(const struct sql_stats_index *i)
{ return i == NULL ? 0 : i->tuple_count; }
size_t sql_stats_index_prefix_count(const struct sql_stats_index *i)
{ return i == NULL ? 0 : i->prefix_count; }
uint64_t sql_stats_index_distinct_prefix(const struct sql_stats_index *i,
					 size_t n)
{ return i == NULL || n >= i->prefix_count ? 0 : i->distinct_prefixes[n]; }

enum sql_stats_lookup_status
sql_stats_snapshot_estimate_index_prefix_rows(
	const struct sql_stats_snapshot *snapshot, uint64_t current_schema_version,
	uint32_t space_id, uint32_t index_id, uint32_t prefix_count, double *rows)
{
	if (rows == NULL)
		return SQL_STATS_LOOKUP_MISSING;
	const struct sql_stats_relation *relation = NULL;
	enum sql_stats_lookup_status status = sql_stats_snapshot_get_relation(
		snapshot, current_schema_version, space_id, &relation);
	if (status != SQL_STATS_LOOKUP_AVAILABLE)
		return status;
	if (prefix_count == 0) {
		*rows = sql_stats_relation_row_count(relation);
		return SQL_STATS_LOOKUP_AVAILABLE;
	}
	const struct sql_stats_index *index = NULL;
	status = sql_stats_relation_get_index(relation, index_id, &index);
	if (status != SQL_STATS_LOOKUP_AVAILABLE)
		return status;
	if (prefix_count > sql_stats_index_prefix_count(index))
		return SQL_STATS_LOOKUP_MISSING;
	uint64_t distinct = sql_stats_index_distinct_prefix(index,
							  prefix_count - 1);
	if (distinct == 0)
		return SQL_STATS_LOOKUP_MISSING;
	*rows = sql_stats_relation_row_count(relation) / distinct;
	return SQL_STATS_LOOKUP_AVAILABLE;
}
