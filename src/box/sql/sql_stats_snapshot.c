#include "sql_stats_snapshot.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct sql_stats_index {
	uint32_t index_id;
	uint64_t tuple_count;
	enum sql_stats_cardinality_semantics tuple_count_semantics;
	char *population_basis;
	char *ndv_basis;
	uint64_t definition_version;
	size_t prefix_count;
	uint64_t *distinct_prefixes;
};

struct sql_stats_relation {
	uint32_t space_id;
	double row_count;
	char *population_basis;
	double average_row_width;
	char *width_basis;
	uint64_t width_denominator_count;
	double confidence;
	char *confidence_source;
	enum sql_stats_cardinality_semantics cardinality_semantics;
	uint64_t collected_at;
	uint64_t modification_epoch;
	uint64_t visibility_id;
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

#ifdef SQL_STATS_SNAPSHOT_TESTING
/* One-shot allocation fault injection for the snapshot unit target only. */
static long test_allocations_before_failure = -1;

void
sql_stats_snapshot_test_fail_allocation_after(long successful_allocations)
{
	test_allocations_before_failure = successful_allocations;
}

static bool
test_should_fail_allocation(void)
{
	if (test_allocations_before_failure < 0)
		return false;
	if (test_allocations_before_failure == 0) {
		test_allocations_before_failure = -1;
		return true;
	}
	test_allocations_before_failure--;
	return false;
}
#else
#define test_should_fail_allocation() false
#endif

static void *
stats_malloc(size_t size)
{
	return test_should_fail_allocation() ? NULL : malloc(size);
}

static void *
stats_calloc(size_t count, size_t size)
{
	return test_should_fail_allocation() ? NULL : calloc(count, size);
}

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

static char *
copy_tag(const char *tag, size_t *bytes, size_t limit)
{
	if (tag == NULL)
		return NULL;
	size_t len = strlen(tag);
	if (len == SIZE_MAX || !add_bytes(bytes, len + 1, limit))
		return NULL;
	char *copy = stats_malloc(len + 1);
	if (copy != NULL)
		memcpy(copy, tag, len + 1);
	return copy;
}

static void
destroy_relations(struct sql_stats_relation *relations, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		free(relations[i].population_basis);
		free(relations[i].width_basis);
		free(relations[i].confidence_source);
		if (relations[i].indexes != NULL) {
			for (size_t j = 0; j < relations[i].index_count; j++) {
				free(relations[i].indexes[j].population_basis);
				free(relations[i].indexes[j].ndv_basis);
				free(relations[i].indexes[j].distinct_prefixes);
			}
		}
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
	struct sql_stats_snapshot *snapshot = stats_calloc(1, sizeof(*snapshot));
	if (snapshot == NULL)
		return NULL;
	snapshot->relations = relation_count == 0 ? NULL :
		stats_calloc(relation_count, sizeof(*snapshot->relations));
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
		out->population_basis = copy_tag(in->population_basis,
						 &snapshot->bytes, max_bytes);
		if (in->population_basis != NULL && out->population_basis == NULL)
			goto error;
		out->average_row_width = in->average_row_width;
		out->width_denominator_count = in->width_denominator_count;
		out->width_basis = copy_tag(in->width_basis, &snapshot->bytes,
					    max_bytes);
		if (in->width_basis != NULL && out->width_basis == NULL)
			goto error;
		out->confidence = in->confidence;
		out->confidence_source = copy_tag(in->confidence_source,
						  &snapshot->bytes, max_bytes);
		if (in->confidence_source != NULL && out->confidence_source == NULL)
			goto error;
		out->cardinality_semantics = in->cardinality_semantics;
		out->collected_at = in->collected_at;
		out->modification_epoch = in->modification_epoch;
		out->visibility_id = in->visibility_id;
		out->index_count = in->index_count;
		if (!add_bytes(&snapshot->bytes,
			       in->index_count * sizeof(struct sql_stats_index),
			       max_bytes))
			goto error;
		out->indexes = in->index_count == 0 ? NULL :
			stats_calloc(in->index_count, sizeof(*out->indexes));
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
			index->tuple_count_semantics =
				index_in->tuple_count_semantics;
			index->population_basis = copy_tag(index_in->population_basis,
							   &snapshot->bytes, max_bytes);
			if (index_in->population_basis != NULL &&
			    index->population_basis == NULL)
				goto error;
			index->ndv_basis = copy_tag(index_in->ndv_basis,
						     &snapshot->bytes, max_bytes);
			if (index_in->ndv_basis != NULL && index->ndv_basis == NULL)
				goto error;
			index->definition_version = index_in->definition_version;
			index->prefix_count = index_in->prefix_count;
			if (!add_bytes(&snapshot->bytes,
				       index_in->prefix_count * sizeof(uint64_t),
				       max_bytes))
				goto error;
			if (index_in->prefix_count != 0) {
				index->distinct_prefixes = stats_malloc(index_in->prefix_count *
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
				if ((index->tuple_count != 0 && value == 0) ||
				    value > index->tuple_count ||
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
{ return s == NULL ? 0 : 2; }
uint64_t sql_stats_snapshot_catalog_version(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->catalog_version; }
uint64_t sql_stats_snapshot_schema_version(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->schema_version; }
size_t sql_stats_snapshot_bytes(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->bytes; }
size_t sql_stats_snapshot_relation_count(const struct sql_stats_snapshot *s)
{ return s == NULL ? 0 : s->relation_count; }

enum sql_stats_lookup_status
sql_stats_snapshot_relation_at(const struct sql_stats_snapshot *s,
			       size_t ordinal,
			       const struct sql_stats_relation **result)
{
	if (result != NULL)
		*result = NULL;
	if (s == NULL || result == NULL || ordinal >= s->relation_count)
		return SQL_STATS_LOOKUP_MISSING;
	*result = &s->relations[ordinal];
	return SQL_STATS_LOOKUP_AVAILABLE;
}

size_t sql_stats_relation_index_count(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->index_count; }

enum sql_stats_lookup_status
sql_stats_relation_index_at(const struct sql_stats_relation *r, size_t ordinal,
			    const struct sql_stats_index **result)
{
	if (result != NULL)
		*result = NULL;
	if (r == NULL || result == NULL || ordinal >= r->index_count)
		return SQL_STATS_LOOKUP_MISSING;
	*result = &r->indexes[ordinal];
	return SQL_STATS_LOOKUP_AVAILABLE;
}

struct snapshot_copy_storage {
	struct sql_stats_relation_input *relations;
	struct sql_stats_index_input **indexes;
	uint64_t ***prefixes;
	size_t relation_count;
};

static void
snapshot_copy_storage_destroy(struct snapshot_copy_storage *storage)
{
	for (size_t i = 0; i < storage->relation_count; i++) {
		if (storage->prefixes != NULL && storage->prefixes[i] != NULL) {
			for (size_t j = 0; storage->indexes != NULL &&
			     storage->indexes[i] != NULL &&
			     j < storage->relations[i].index_count; j++)
				free(storage->prefixes[i][j]);
		}
		if (storage->prefixes != NULL)
			free(storage->prefixes[i]);
		if (storage->indexes != NULL)
			free(storage->indexes[i]);
	}
	free(storage->prefixes);
	free(storage->indexes);
	free(storage->relations);
	*storage = (struct snapshot_copy_storage){};
}

static bool
snapshot_copy_relation(const struct sql_stats_relation *source,
		       struct sql_stats_relation_input *relation,
		       struct sql_stats_index_input **indexes,
		       uint64_t ***prefixes, size_t *scratch_bytes,
		       size_t max_bytes)
{
	size_t index_count = sql_stats_relation_index_count(source);
	if (index_count > SIZE_MAX / sizeof(**indexes) ||
	    index_count > SIZE_MAX / sizeof(**prefixes))
		return false;
	/* Make partial-allocation cleanup aware of every owned prefix slot. */
	relation->index_count = index_count;
	size_t index_bytes = index_count * sizeof(**indexes);
	size_t prefix_ptr_bytes = index_count * sizeof(**prefixes);
	if (index_bytes > max_bytes - *scratch_bytes ||
	    prefix_ptr_bytes > max_bytes - *scratch_bytes - index_bytes)
		return false;
	*indexes = index_count == 0 ? NULL : calloc(index_count, sizeof(**indexes));
	*prefixes = index_count == 0 ? NULL : calloc(index_count, sizeof(**prefixes));
	if (index_count != 0 && (*indexes == NULL || *prefixes == NULL))
		return false;
	*scratch_bytes += index_bytes + prefix_ptr_bytes;
	for (size_t i = 0; i < index_count; i++) {
		const struct sql_stats_index *source_index = NULL;
		if (sql_stats_relation_index_at(source, i, &source_index) !=
		    SQL_STATS_LOOKUP_AVAILABLE)
			return false;
		size_t prefix_count = sql_stats_index_prefix_count(source_index);
		if (prefix_count > SIZE_MAX / sizeof(uint64_t) ||
		    prefix_count * sizeof(uint64_t) > max_bytes - *scratch_bytes)
			return false;
		uint64_t *prefix = prefix_count == 0 ? NULL :
			malloc(prefix_count * sizeof(*prefix));
		if (prefix_count != 0 && prefix == NULL)
			return false;
		*scratch_bytes += prefix_count * sizeof(*prefix);
		for (size_t j = 0; j < prefix_count; j++)
			prefix[j] = sql_stats_index_distinct_prefix(source_index, j);
		(*prefixes)[i] = prefix;
		(*indexes)[i] = (struct sql_stats_index_input) {
			.index_id = sql_stats_index_id(source_index),
			.tuple_count = sql_stats_index_tuple_count(source_index),
			.tuple_count_semantics =
				sql_stats_index_tuple_count_semantics(source_index),
			.population_basis =
				sql_stats_index_population_basis(source_index),
			.ndv_basis = sql_stats_index_ndv_basis(source_index),
			.definition_version =
				sql_stats_index_definition_version(source_index),
			.distinct_prefixes = prefix,
			.prefix_count = prefix_count,
		};
	}
	*relation = (struct sql_stats_relation_input) {
		.space_id = sql_stats_relation_space_id(source),
		.row_count = sql_stats_relation_row_count(source),
		.population_basis = sql_stats_relation_population_basis(source),
		.average_row_width = sql_stats_relation_average_row_width(source),
		.width_basis = sql_stats_relation_width_basis(source),
		.width_denominator_count =
			sql_stats_relation_width_denominator_count(source),
		.confidence = sql_stats_relation_confidence(source),
		.confidence_source = sql_stats_relation_confidence_source(source),
		.cardinality_semantics =
			sql_stats_relation_cardinality_semantics(source),
		.collected_at = sql_stats_relation_collected_at(source),
		.modification_epoch =
			sql_stats_relation_modification_epoch(source),
		.visibility_id = sql_stats_relation_visibility_id(source),
		.indexes = *indexes,
		.index_count = index_count,
	};
	return true;
}

static struct sql_stats_snapshot *
snapshot_rebuild(const struct sql_stats_snapshot *base,
		const struct sql_stats_snapshot *replacement,
		uint32_t replace_space_id, size_t max_bytes)
{
	if (base == NULL || max_bytes == 0 ||
	    (replacement != NULL &&
	     (sql_stats_snapshot_catalog_version(base) !=
	      sql_stats_snapshot_catalog_version(replacement) ||
	      sql_stats_snapshot_schema_version(base) !=
	      sql_stats_snapshot_schema_version(replacement))))
		return NULL;
	size_t base_count = sql_stats_snapshot_relation_count(base);
	size_t replacement_count = replacement == NULL ? 0 :
		sql_stats_snapshot_relation_count(replacement);
	if (replacement != NULL && replace_space_id != 0 && replacement_count != 1)
		return NULL;
	if (replacement != NULL && replace_space_id != 0) {
		const struct sql_stats_relation *r = NULL;
		if (sql_stats_snapshot_relation_at(replacement, 0, &r) !=
		    SQL_STATS_LOOKUP_AVAILABLE ||
		    sql_stats_relation_space_id(r) != replace_space_id)
			return NULL;
	}
	size_t count = base_count;
	if (replace_space_id != 0) {
		const struct sql_stats_relation *existing = NULL;
		for (size_t i = 0; i < base_count; i++) {
			if (sql_stats_snapshot_relation_at(base, i, &existing) !=
			    SQL_STATS_LOOKUP_AVAILABLE)
				return NULL;
			if (sql_stats_relation_space_id(existing) == replace_space_id) {
				count--;
				break;
			}
		}
	}
	if (replacement_count > SIZE_MAX - count ||
	    count + replacement_count > SIZE_MAX / sizeof(struct sql_stats_relation_input))
		return NULL;
	count += replacement_count;
	size_t relation_bytes = count * sizeof(struct sql_stats_relation_input);
	if (count > SIZE_MAX / sizeof(struct sql_stats_index_input *) ||
	    count > SIZE_MAX / sizeof(uint64_t **))
		return NULL;
	size_t pointer_bytes = count * (sizeof(struct sql_stats_index_input *) +
					 sizeof(uint64_t **));
	if (relation_bytes > max_bytes ||
	    pointer_bytes > max_bytes - relation_bytes)
		return NULL;
	struct snapshot_copy_storage storage = {.relation_count = count};
	storage.relations = count == 0 ? NULL : calloc(count,
								 sizeof(*storage.relations));
	storage.indexes = count == 0 ? NULL : calloc(count,
							       sizeof(*storage.indexes));
	storage.prefixes = count == 0 ? NULL : calloc(count,
							        sizeof(*storage.prefixes));
	if (count != 0 && (storage.relations == NULL || storage.indexes == NULL ||
			   storage.prefixes == NULL))
		goto fail;
	size_t scratch_bytes = relation_bytes + pointer_bytes;
	size_t out = 0;
	const struct sql_stats_snapshot *sources[2] = {base, replacement};
	for (size_t s = 0; s < 2; s++) {
		const struct sql_stats_snapshot *source = sources[s];
		if (source == NULL)
			continue;
		for (size_t i = 0; i < sql_stats_snapshot_relation_count(source); i++) {
			const struct sql_stats_relation *r = NULL;
			if (sql_stats_snapshot_relation_at(source, i, &r) !=
			    SQL_STATS_LOOKUP_AVAILABLE)
				goto fail;
			if (s == 0 && replace_space_id != 0 &&
			    sql_stats_relation_space_id(r) == replace_space_id)
				continue;
			if (out >= count || !snapshot_copy_relation(r,
				&storage.relations[out], &storage.indexes[out],
				&storage.prefixes[out], &scratch_bytes, max_bytes))
				goto fail;
			out++;
		}
	}
	if (out != count)
		goto fail;
	struct sql_stats_snapshot *result = sql_stats_snapshot_new(
		sql_stats_snapshot_catalog_version(base),
		sql_stats_snapshot_schema_version(base), storage.relations, count,
		max_bytes);
	snapshot_copy_storage_destroy(&storage);
	return result;
fail:
	snapshot_copy_storage_destroy(&storage);
	return NULL;
}

struct sql_stats_snapshot *
sql_stats_snapshot_combine(const struct sql_stats_snapshot *const *snapshots,
			   size_t snapshot_count, size_t max_bytes)
{
	if (snapshots == NULL || snapshot_count == 0 || snapshots[0] == NULL)
		return NULL;
	struct sql_stats_snapshot *result = NULL;
	for (size_t i = 0; i < snapshot_count; i++) {
		if (snapshots[i] == NULL)
			goto fail;
		if (result == NULL) {
			result = snapshot_rebuild(snapshots[i], NULL, 0, max_bytes);
		} else {
			struct sql_stats_snapshot *next = snapshot_rebuild(result,
				snapshots[i], 0, max_bytes);
			sql_stats_snapshot_release(result);
			result = next;
		}
		if (result == NULL)
			goto fail;
	}
	return result;
fail:
	sql_stats_snapshot_release(result);
	return NULL;
}

struct sql_stats_snapshot *
sql_stats_snapshot_replace_relation(const struct sql_stats_snapshot *base,
				    const struct sql_stats_snapshot *replacement,
				    uint32_t space_id, size_t max_bytes)
{
	if (space_id == 0 || replacement == NULL)
		return NULL;
	return snapshot_rebuild(base, replacement, space_id, max_bytes);
}

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
const char *sql_stats_relation_population_basis(const struct sql_stats_relation *r)
{ return r == NULL ? NULL : r->population_basis; }
double sql_stats_relation_average_row_width(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->average_row_width; }
const char *sql_stats_relation_width_basis(const struct sql_stats_relation *r)
{ return r == NULL ? NULL : r->width_basis; }
uint64_t sql_stats_relation_width_denominator_count(
	const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->width_denominator_count; }
double sql_stats_relation_confidence(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->confidence; }
const char *sql_stats_relation_confidence_source(const struct sql_stats_relation *r)
{ return r == NULL ? NULL : r->confidence_source; }
enum sql_stats_cardinality_semantics
sql_stats_relation_cardinality_semantics(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->cardinality_semantics; }
uint64_t sql_stats_relation_collected_at(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->collected_at; }
uint64_t sql_stats_relation_modification_epoch(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->modification_epoch; }
uint64_t sql_stats_relation_visibility_id(const struct sql_stats_relation *r)
{ return r == NULL ? 0 : r->visibility_id; }
uint32_t sql_stats_index_id(const struct sql_stats_index *i)
{ return i == NULL ? 0 : i->index_id; }
uint64_t sql_stats_index_tuple_count(const struct sql_stats_index *i)
{ return i == NULL ? 0 : i->tuple_count; }
enum sql_stats_cardinality_semantics
sql_stats_index_tuple_count_semantics(const struct sql_stats_index *i)
{ return i == NULL ? 0 : i->tuple_count_semantics; }
const char *sql_stats_index_population_basis(const struct sql_stats_index *i)
{ return i == NULL ? NULL : i->population_basis; }
const char *sql_stats_index_ndv_basis(const struct sql_stats_index *i)
{ return i == NULL ? NULL : i->ndv_basis; }
uint64_t sql_stats_index_definition_version(const struct sql_stats_index *i)
{ return i == NULL ? 0 : i->definition_version; }
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
	/* Prefix NDVs describe the index population, which may be smaller than
	 * the relation. */
	*rows = (double)sql_stats_index_tuple_count(index) / distinct;
	return SQL_STATS_LOOKUP_AVAILABLE;
}
