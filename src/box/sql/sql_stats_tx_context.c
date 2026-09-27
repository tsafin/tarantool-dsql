#include "sql_stats_collection.h"
#include "sql_stats_index_summary.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "box.h"
#include "schema.h"
#include "space.h"
#include "space_cache.h"
#include "txn.h"
#include "sql.h"
#include "vclock/vclock.h"

struct sql_stats_tx_target {
	struct sql_stats_collection_target key;
	uint32_t index_unique_id;
	bool sampled;
};

struct sql_stats_tx_context {
	struct sql_stats_tx_target *targets;
	size_t target_count;
	int64_t txn_id;
	uint64_t catalog_version;
	uint64_t schema_version;
	uint64_t visibility_id;
	bool active;
	bool failed;
};

struct sql_stats_tx_tuple {
	char *data;
	size_t size;
};

struct sql_stats_tx_batch {
	struct sql_stats_tx_tuple *tuples;
	size_t count;
	size_t bytes;
	size_t allocated_bytes;
	size_t max_rows;
	size_t max_bytes;
	size_t max_buffer_bytes;
};

static struct sql_stats_tx_target *
find_target(struct sql_stats_tx_context *context,
	    const struct sql_stats_collection_target *key)
{
	for (size_t i = 0; i < context->target_count; i++) {
		if (context->targets[i].key.space_id == key->space_id &&
		    context->targets[i].key.index_id == key->index_id)
			return &context->targets[i];
	}
	return NULL;
}

static struct index *
lookup_target(const struct sql_stats_collection_target *target)
{
	struct space *space = space_by_id_slow(target->space_id);
	if (space == NULL)
		return NULL;
	return space_index(space, target->index_id);
}

static bool
validate_targets(struct sql_stats_tx_context *context, bool compare_unique_id)
{
	for (size_t i = 0; i < context->target_count; i++) {
		struct sql_stats_tx_target *target = &context->targets[i];
		struct index *index = lookup_target(&target->key);
		if (index == NULL || index->def == NULL ||
		    index->def->space_id != target->key.space_id ||
		    index->def->iid != target->key.index_id ||
		    (compare_unique_id && index->unique_id != target->index_unique_id))
			return false;
		if (!compare_unique_id)
			target->index_unique_id = index->unique_id;
	}
	return true;
}

static bool
owns_current_txn(const struct sql_stats_tx_context *context)
{
	return context != NULL && context->active && box_txn() &&
		box_txn_id() == context->txn_id;
}

static bool
has_required_isolation(const struct sql_stats_tx_context *context)
{
	return owns_current_txn(context) &&
		box_txn_isolation() == TXN_ISOLATION_READ_CONFIRMED;
}

static bool
has_same_visibility(const struct sql_stats_tx_context *context)
{
	return box_vclock != NULL && vclock_sum(box_vclock) >= 0 &&
		(uint64_t)vclock_sum(box_vclock) == context->visibility_id;
}

static void
free_context(struct sql_stats_tx_context *context)
{
	if (context == NULL)
		return;
	free(context->targets);
	free(context);
}

static int
rollback_owned(struct sql_stats_tx_context *context)
{
	if (!owns_current_txn(context))
		return -1;
	if (box_txn_rollback() != 0)
		return -1;
	context->active = false;
	return 0;
}

static int
tx_batch_consume(void *arg, const char *tuple, size_t tuple_size,
		 const uint32_t *field_ids, size_t field_count)
{
	(void)field_ids;
	(void)field_count;
	struct sql_stats_tx_batch *batch = arg;
	if (tuple == NULL || tuple_size == 0 || batch->count == batch->max_rows ||
	    batch->bytes > batch->max_bytes ||
	    tuple_size > batch->max_bytes - batch->bytes ||
	    batch->allocated_bytes > batch->max_buffer_bytes ||
	    tuple_size > batch->max_buffer_bytes - batch->allocated_bytes)
		return -1;
	char *copy = malloc(tuple_size);
	if (copy == NULL)
		return -1;
	memcpy(copy, tuple, tuple_size);
	batch->tuples[batch->count++] = (struct sql_stats_tx_tuple){
		.data = copy,
		.size = tuple_size,
	};
	batch->bytes += tuple_size;
	batch->allocated_bytes += tuple_size;
	return 0;
}

static void
tx_batch_destroy(struct sql_stats_tx_batch *batch)
{
	for (size_t i = 0; i < batch->count; i++)
		free(batch->tuples[i].data);
	free(batch->tuples);
}

static bool
tx_context_valid(struct sql_stats_tx_context *context)
{
	return has_required_isolation(context) &&
		has_same_visibility(context) &&
		box_catalog_version() == context->catalog_version &&
		box_schema_version() == context->schema_version &&
		validate_targets(context, true);
}

int
sql_stats_tx_context_begin(
	const struct sql_stats_collection_target *targets, size_t target_count,
	struct sql_stats_tx_context **context_out)
{
	if (context_out == NULL)
		return -1;
	*context_out = NULL;
	if (box_txn() || targets == NULL || target_count == 0 ||
	    target_count > SIZE_MAX / sizeof(*targets))
		return -1;
	struct sql_stats_tx_context *context = calloc(1, sizeof(*context));
	if (context == NULL)
		return -1;
	context->targets = calloc(target_count, sizeof(*context->targets));
	if (context->targets == NULL) {
		free_context(context);
		return -1;
	}
	context->target_count = target_count;
	for (size_t i = 0; i < target_count; i++) {
		context->targets[i].key = targets[i];
		for (size_t j = 0; j < i; j++) {
			if (targets[i].space_id == targets[j].space_id &&
			    targets[i].index_id == targets[j].index_id) {
				free_context(context);
				return -1;
			}
		}
	}
	context->schema_version = box_schema_version();
	context->catalog_version = box_catalog_version();
	if (box_vclock == NULL || vclock_sum(box_vclock) < 0) {
		free_context(context);
		return -1;
	}
	context->visibility_id = (uint64_t)vclock_sum(box_vclock);
	if (!validate_targets(context, false)) {
		free_context(context);
		return -1;
	}
	if (box_txn_begin() != 0) {
		free_context(context);
		return -1;
	}
	context->active = true;
	context->txn_id = box_txn_id();
	if (context->txn_id < 0 ||
	    box_txn_set_isolation(TXN_ISOLATION_READ_CONFIRMED) != 0 ||
	    !has_required_isolation(context) ||
	    box_catalog_version() != context->catalog_version ||
	    box_schema_version() != context->schema_version ||
	    !validate_targets(context, true)) {
		if (owns_current_txn(context))
			(void)rollback_owned(context);
		free_context(context);
		return -1;
	}
	*context_out = context;
	return 0;
}

uint64_t
sql_stats_tx_context_visibility_id(
	const struct sql_stats_tx_context *context)
{
	return context == NULL ? 0 : context->visibility_id;
}

uint64_t
sql_stats_tx_context_catalog_version(
	const struct sql_stats_tx_context *context)
{
	return context == NULL ? 0 : context->catalog_version;
}

uint64_t
sql_stats_tx_context_schema_version(
	const struct sql_stats_tx_context *context)
{
	return context == NULL ? 0 : context->schema_version;
}

int
sql_stats_tx_context_sample_index(
	struct sql_stats_tx_context *context,
	const struct sql_stats_collection_target *target,
	const struct sql_stats_sample_request *request,
	struct sql_stats_sample_sink *sink,
	struct sql_stats_sample_result *result)
{
	if (result == NULL) {
		if (context != NULL)
			context->failed = true;
		return -1;
	}
	*result = (struct sql_stats_sample_result){};
	if (context == NULL || target == NULL || request == NULL || sink == NULL ||
	    sink->consume == NULL || request->max_rows == 0 ||
	    request->max_bytes == 0 || request->max_buffer_bytes == 0 ||
	    request->max_rows > SIZE_MAX / sizeof(struct sql_stats_tx_tuple) ||
	    request->max_bytes > SIZE_MAX ||
	    request->max_buffer_bytes > SIZE_MAX ||
	    (request->field_count != 0 && request->field_ids == NULL)) {
		if (context != NULL)
			context->failed = true;
		return -1;
	}
	struct sql_stats_tx_target *owned_target = find_target(context, target);
	if (owned_target == NULL || owned_target->sampled || context->failed ||
	    !tx_context_valid(context)) {
		context->failed = true;
		return -1;
	}
	size_t metadata_bytes = sizeof(struct sql_stats_tx_tuple) *
		request->max_rows;
	if (metadata_bytes > request->max_buffer_bytes) {
		context->failed = true;
		return -1;
	}
	struct sql_stats_tx_batch batch = {
		.max_rows = request->max_rows,
		.max_bytes = request->max_bytes,
		.max_buffer_bytes = request->max_buffer_bytes,
		.allocated_bytes = metadata_bytes,
	};
	batch.tuples = calloc(request->max_rows, sizeof(*batch.tuples));
	if (batch.tuples == NULL) {
		context->failed = true;
		return -1;
	}
	struct sql_stats_sample_sink staging_sink = {
		.context = &batch,
		.consume = tx_batch_consume,
	};
	struct sql_stats_sample_result staged_result;
	struct space *space = space_by_id_slow(target->space_id);
	int rc = space == NULL ? -1 : engine_sql_stats_sample(space, request,
		&staging_sink, &staged_result);
	if (rc == 0 && (!tx_context_valid(context) ||
		staged_result.rows != batch.count ||
		staged_result.bytes != batch.bytes))
		rc = -1;
	if (rc == 0) {
		for (size_t i = 0; i < batch.count; i++) {
			if (sink->consume(sink->context, batch.tuples[i].data,
					  batch.tuples[i].size, request->field_ids,
					  request->field_count) != 0) {
				rc = -1;
				break;
			}
		}
	}
	if (rc == 0) {
		owned_target->sampled = true;
		*result = staged_result;
	} else {
		context->failed = true;
	}
	tx_batch_destroy(&batch);
	return rc;
}

int
sql_stats_tx_context_finish(struct sql_stats_tx_context **context_ptr)
{
	if (context_ptr == NULL || *context_ptr == NULL)
		return -1;
	struct sql_stats_tx_context *context = *context_ptr;
	if (!owns_current_txn(context))
		return -1;
	bool complete = !context->failed && has_required_isolation(context) &&
		has_same_visibility(context) &&
		box_catalog_version() == context->catalog_version &&
		box_schema_version() == context->schema_version &&
		validate_targets(context, true);
	for (size_t i = 0; i < context->target_count; i++)
		complete = complete && context->targets[i].sampled;
	int rc;
	if (complete) {
		rc = box_txn_commit();
		context->active = false;
		/* Commit may yield; reject if another transaction committed meanwhile. */
		if (rc == 0 && !has_same_visibility(context))
			rc = -1;
	} else {
		rc = rollback_owned(context);
		if (rc == 0)
			rc = -1;
	}
	if (!context->active) {
		free_context(context);
		*context_ptr = NULL;
	}
	return rc;
}

static bool
tx_context_matches_expected(
	const struct sql_stats_tx_context *context,
	const struct sql_stats_expected_relation *expected,
	size_t expected_count)
{
	if (expected == NULL || expected_count == 0 ||
	    context->target_count == 0)
		return false;
	size_t index_count = 0;
	for (size_t i = 0; i < expected_count; i++) {
		if (expected[i].index_count == 0 || expected[i].indexes == NULL ||
		    expected[i].index_count > SIZE_MAX - index_count)
			return false;
		index_count += expected[i].index_count;
		for (size_t j = 0; j < expected[i].index_count; j++) {
			bool found = false;
			for (size_t k = 0; k < context->target_count; k++) {
				const struct sql_stats_tx_target *target =
					&context->targets[k];
				if (target->key.space_id == expected[i].space_id &&
				    target->key.index_id ==
				    expected[i].indexes[j].index_id &&
				    expected[i].indexes[j].definition_version != 0 &&
				    expected[i].indexes[j].definition_version ==
				    target->index_unique_id) {
					found = true;
					break;
				}
			}
			if (!found)
				return false;
		}
	}
	return index_count == context->target_count;
}

int
sql_stats_tx_context_finish_and_publish(
	struct sql_stats_tx_context **context_ptr,
	const struct sql_stats_expected_relation *expected, size_t expected_count,
	const struct sql_stats_collection_result *result, size_t max_bytes)
{
	if (context_ptr == NULL || *context_ptr == NULL)
		return -1;
	struct sql_stats_tx_context *context = *context_ptr;
	struct sql_stats_collection_generation generation = {
		.catalog_version = context->catalog_version,
		.schema_version = context->schema_version,
		.visibility_id = context->visibility_id,
	};
	struct sql_stats_snapshot *candidate = NULL;
	if (owns_current_txn(context) && !context->failed &&
	    tx_context_valid(context) &&
	    tx_context_matches_expected(context, expected, expected_count)) {
		candidate = sql_stats_collection_build_candidate(&generation,
			expected, expected_count, result, max_bytes);
	}
	if (candidate == NULL) {
		(void)sql_stats_tx_context_abort(context_ptr);
		return -1;
	}
	int rc = sql_stats_tx_context_finish(context_ptr);
	if (rc != 0 || *context_ptr != NULL || sql_get() == NULL ||
	    box_schema_version() != generation.schema_version ||
	    box_catalog_version() != generation.catalog_version || box_vclock == NULL ||
	    vclock_sum(box_vclock) < 0 ||
	    (uint64_t)vclock_sum(box_vclock) != generation.visibility_id ||
	    sql_stats_snapshot_catalog_version(candidate) !=
	    generation.catalog_version ||
	    sql_stats_snapshot_schema_version(candidate) != generation.schema_version) {
		sql_stats_snapshot_release(candidate);
		return -1;
	}
	/* No yield is allowed between the final generation check and this swap. */
	sql_set_stats_snapshot(candidate);
	sql_stats_snapshot_release(candidate);
	return 0;
}

int
sql_stats_tx_context_abort(struct sql_stats_tx_context **context_ptr)
{
	if (context_ptr == NULL || *context_ptr == NULL)
		return -1;
	struct sql_stats_tx_context *context = *context_ptr;
	int rc = rollback_owned(context);
	if (rc == 0) {
		free_context(context);
		*context_ptr = NULL;
	}
	return rc;
}

struct sql_stats_tx_staged_index {
	struct sql_stats_index_summary *summary;
	struct sql_stats_sample_result sample;
};

static int
tx_summary_sink_consume(void *arg, const char *tuple, size_t tuple_size,
			const uint32_t *field_ids, size_t field_count)
{
	return sql_stats_index_summary_consume(arg, tuple, tuple_size, field_ids,
						field_count);
}

static void
tx_staged_indexes_destroy(struct sql_stats_tx_staged_index *staged,
			  size_t count)
{
	if (staged == NULL)
		return;
	for (size_t i = 0; i < count; i++)
		sql_stats_index_summary_delete(staged[i].summary);
	free(staged);
}

struct sql_stats_snapshot *
sql_stats_tx_context_build_sample_candidate(
	struct sql_stats_tx_context *context,
	const struct sql_stats_expected_relation *expected,
	const struct sql_stats_tx_index_spec *specs, size_t spec_count,
	uint32_t relation_index_id, double relation_confidence,
	const char *confidence_source, size_t max_candidate_bytes,
	size_t max_staging_bytes, size_t max_temp_bytes, uint64_t max_work)
{
	if (context == NULL)
		return NULL;
	struct sql_stats_tx_staged_index *staged = NULL;
	struct sql_stats_sampled_index *sampled_indexes = NULL;
	double *confidences = NULL;
	struct sql_stats_snapshot *candidate = NULL;
	bool valid = owns_current_txn(context) && !context->failed &&
		expected != NULL && expected->index_count != 0 &&
		expected->indexes != NULL && specs != NULL &&
		spec_count == expected->index_count &&
		spec_count <= SIZE_MAX / (sizeof(*staged) +
			sizeof(*sampled_indexes) + sizeof(*confidences)) &&
		tx_context_matches_expected(context, expected, 1) &&
		tx_context_valid(context) &&
		context->visibility_id != 0 &&
		isfinite(relation_confidence) && relation_confidence >= 0 &&
		relation_confidence <= 1 &&
		confidence_source != NULL && confidence_source[0] != '\0';
	if (!valid)
		goto fail;
	size_t metadata_bytes = spec_count * sizeof(*staged) +
		spec_count * sizeof(*sampled_indexes) +
		spec_count * sizeof(*confidences);
	if (metadata_bytes > max_staging_bytes)
		goto fail;
	size_t total_staging_bytes = metadata_bytes;
	for (size_t i = 0; i < spec_count; i++) {
		for (size_t j = 0; j < i; j++) {
			if (expected->indexes[i].index_id ==
			    expected->indexes[j].index_id)
				goto fail;
		}
		const struct sql_stats_tx_index_spec *spec = &specs[i];
		if (spec->expected == NULL || spec->extract == NULL ||
		    spec->target.space_id != expected->space_id ||
		    spec->target.index_id != spec->expected->index_id ||
		    spec->request.index_id != spec->target.index_id ||
		    spec->expected->part_count == 0 ||
		    spec->expected->definition_version == 0 ||
		    spec->summary_max_bytes == 0 ||
		    spec->summary_max_bytes > max_staging_bytes -
						      total_staging_bytes)
			goto fail;
		total_staging_bytes += spec->summary_max_bytes;
		bool expected_match = false;
		for (size_t j = 0; j < expected->index_count; j++) {
			if (expected->indexes[j].index_id == spec->expected->index_id &&
			    expected->indexes[j].definition_version ==
				    spec->expected->definition_version &&
			    expected->indexes[j].part_count ==
				    spec->expected->part_count)
				expected_match = true;
		}
		if (!expected_match)
			goto fail;
		for (size_t j = 0; j < i; j++) {
			if (specs[j].target.space_id == spec->target.space_id &&
			    specs[j].target.index_id == spec->target.index_id)
				goto fail;
		}
	}
	if (total_staging_bytes > max_staging_bytes)
		goto fail;
	staged = calloc(spec_count, sizeof(*staged));
	sampled_indexes = calloc(spec_count, sizeof(*sampled_indexes));
	confidences = calloc(spec_count, sizeof(*confidences));
	if (staged == NULL || sampled_indexes == NULL || confidences == NULL)
		goto fail;
	size_t relation_sample_index = spec_count;
	for (size_t i = 0; i < spec_count; i++) {
		const struct sql_stats_tx_index_spec *spec = &specs[i];
		staged[i].summary = sql_stats_index_summary_new(
			spec->expected->part_count, spec->hll_precision,
			spec->hll_seed, spec->summary_max_bytes, spec->extract,
			spec->extract_context);
		if (staged[i].summary == NULL)
			goto fail;
		struct sql_stats_sample_sink sink = {
			.context = staged[i].summary,
			.consume = tx_summary_sink_consume,
		};
		struct sql_stats_sample_request request = spec->request;
		if (sql_stats_tx_context_sample_index(context, &spec->target,
				&request, &sink, &staged[i].sample) != 0)
			goto fail;
		sampled_indexes[i] = (struct sql_stats_sampled_index) {
			.expected = spec->expected,
			.sample = &staged[i].sample,
			.summary = staged[i].summary,
		};
		if (spec->target.index_id == relation_index_id)
			relation_sample_index = i;
	}
	if (relation_sample_index == spec_count || !tx_context_valid(context))
		goto fail;
	struct sql_stats_collection_generation generation = {
		.catalog_version = context->catalog_version,
		.schema_version = context->schema_version,
		.visibility_id = context->visibility_id,
	};
	candidate = sql_stats_collection_build_sample_candidate(&generation,
		expected, sampled_indexes, spec_count,
		&staged[relation_sample_index].sample, relation_confidence,
		confidence_source, confidences, max_candidate_bytes,
		max_temp_bytes, max_work);
	if (candidate == NULL)
		goto fail;
	tx_staged_indexes_destroy(staged, spec_count);
	free(sampled_indexes);
	free(confidences);
	return candidate;
fail:
	context->failed = true;
	if (candidate != NULL)
		sql_stats_snapshot_release(candidate);
	tx_staged_indexes_destroy(staged, spec_count);
	free(sampled_indexes);
	free(confidences);
	return NULL;
}
