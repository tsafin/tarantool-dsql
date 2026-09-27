#include "sql_stats_collection.h"

#include <stdlib.h>
#include <string.h>

#include "index.h"
#include "read_view.h"
#include "schema.h"
#include "space.h"

struct sql_stats_collection_context {
	struct read_view view;
	struct sql_stats_collection_target *targets;
	size_t target_count;
	uint64_t schema_version;
	bool view_open;
};

static bool
context_has_space(const struct sql_stats_collection_context *context,
		  uint32_t space_id)
{
	for (size_t i = 0; i < context->target_count; i++) {
		if (context->targets[i].space_id == space_id)
			return true;
	}
	return false;
}

static bool
context_has_index(const struct sql_stats_collection_context *context,
		  uint32_t space_id, uint32_t index_id)
{
	for (size_t i = 0; i < context->target_count; i++) {
		if (context->targets[i].space_id == space_id &&
		    context->targets[i].index_id == index_id)
			return true;
	}
	return false;
}

static bool
context_filter_space(struct space *space, void *arg)
{
	const struct sql_stats_collection_context *context = arg;
	return context_has_space(context, space_id(space));
}

static bool
context_filter_index(struct space *space, struct index *index, void *arg)
{
	const struct sql_stats_collection_context *context = arg;
	return context_has_index(context, space_id(space), index->def->iid);
}

static struct index_read_view *
context_get_index(struct sql_stats_collection_context *context,
		  const struct sql_stats_collection_target *target)
{
	struct space_read_view *space_view;
	read_view_foreach_space(space_view, &context->view) {
		if (space_view->id != target->space_id)
			continue;
		struct index_read_view *index_view =
			space_read_view_index(space_view, target->index_id);
		if (index_view == NULL || index_view->def == NULL ||
		    index_view->def->space_id != target->space_id ||
		    index_view->def->iid != target->index_id)
			return NULL;
		return index_view;
	}
	return NULL;
}

struct sql_stats_collection_context *
sql_stats_collection_context_new(
	const struct sql_stats_collection_target *targets, size_t target_count)
{
	if (targets == NULL || target_count == 0 ||
	    target_count > SIZE_MAX / sizeof(*targets))
		return NULL;
	struct sql_stats_collection_context *context = calloc(1, sizeof(*context));
	if (context == NULL)
		return NULL;
	context->targets = malloc(target_count * sizeof(*targets));
	if (context->targets == NULL)
		goto fail;
	memcpy(context->targets, targets, target_count * sizeof(*targets));
	context->target_count = target_count;
	for (size_t i = 0; i < target_count; i++) {
		for (size_t j = i + 1; j < target_count; j++) {
			if (targets[i].space_id == targets[j].space_id &&
			    targets[i].index_id == targets[j].index_id)
				goto fail;
		}
	}
	uint64_t schema_before = box_schema_version();
	struct read_view_opts opts;
	read_view_opts_create(&opts);
	opts.name = "sql-stats-collection";
	opts.filter_space = context_filter_space;
	opts.filter_index = context_filter_index;
	opts.filter_arg = context;
	if (read_view_open(&context->view, &opts) != 0)
		goto fail;
	context->view_open = true;
	if (box_schema_version() != schema_before)
		goto fail;
	for (size_t i = 0; i < target_count; i++) {
		if (context_get_index(context, &targets[i]) == NULL)
			goto fail;
	}
	context->schema_version = schema_before;
	return context;
fail:
	sql_stats_collection_context_delete(context);
	return NULL;
}

void
sql_stats_collection_context_delete(
	struct sql_stats_collection_context *context)
{
	if (context == NULL)
		return;
	if (context->view_open)
		read_view_close(&context->view);
	free(context->targets);
	free(context);
}

uint64_t
sql_stats_collection_context_visibility_id(
	const struct sql_stats_collection_context *context)
{
	return context == NULL || !context->view_open ? 0 : context->view.id;
}

uint64_t
sql_stats_collection_context_schema_version(
	const struct sql_stats_collection_context *context)
{
	return context == NULL ? 0 : context->schema_version;
}

int
sql_stats_collection_context_sample_index(
	struct sql_stats_collection_context *context,
	const struct sql_stats_collection_target *target,
	const struct sql_stats_sample_request *request,
	struct sql_stats_sample_sink *sink,
	struct sql_stats_sample_result *result)
{
	if (result == NULL)
		return -1;
	*result = (struct sql_stats_sample_result){};
	if (context == NULL || target == NULL || request == NULL || sink == NULL ||
	    sink->consume == NULL || request->max_rows == 0 ||
	    request->max_bytes == 0 || request->max_buffer_bytes == 0 ||
	    request->max_tuples_examined == 0 ||
	    (request->field_count != 0 && request->field_ids == NULL) ||
	    !context_has_index(context, target->space_id, target->index_id))
		return -1;
	if (box_schema_version() != context->schema_version)
		return -1;
	struct index_read_view *index_view = context_get_index(context, target);
	if (index_view == NULL)
		return -1;
	struct sql_stats_sample_reservoir *reservoir =
		sql_stats_sample_reservoir_new(request->max_rows, request->max_bytes,
					       request->max_buffer_bytes,
					       request->seed);
	if (reservoir == NULL)
		return -1;
	struct index_read_view_iterator iterator;
	if (index_read_view_create_iterator(index_view, ITER_ALL, NULL, 0,
					    &iterator) != 0) {
		sql_stats_sample_reservoir_delete(reservoir);
		return -1;
	}
	int rc = -1;
	for (;;) {
		if (sql_stats_sample_reservoir_population(reservoir) >=
		    request->max_tuples_examined)
			goto out;
		struct read_view_tuple tuple;
		if (index_read_view_iterator_next_raw(&iterator, &tuple) != 0)
			goto out;
		if (tuple.data == NULL) {
			rc = 0;
			break;
		}
		if (sql_stats_sample_reservoir_add(reservoir, tuple.data,
						    tuple.size) != 0)
			goto out;
	}
	/* Defer sink delivery until the pinned candidate scan is complete. */
	if (box_schema_version() != context->schema_version) {
		rc = -1;
		goto out;
	}
	if (sql_stats_sample_reservoir_deliver(reservoir, sink,
						request->field_ids,
						request->field_count,
						result) != 0)
		rc = -1;
out:
	index_read_view_iterator_destroy(&iterator);
	sql_stats_sample_reservoir_delete(reservoir);
	if (rc != 0)
		*result = (struct sql_stats_sample_result){};
	return rc;
}
