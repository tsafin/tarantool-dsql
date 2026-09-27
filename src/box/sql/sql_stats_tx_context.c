#include "sql_stats_collection.h"

#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "box.h"
#include "schema.h"
#include "space.h"
#include "space_cache.h"
#include "txn.h"
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
