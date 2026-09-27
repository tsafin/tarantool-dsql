#include "sql_stats_collection.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "box.h"
#include "index.h"
#include "read_view.h"
#include "schema.h"
#include "space.h"
#include "sql.h"
#include "sql_stats_index_summary.h"
#include "vclock/vclock.h"

struct sql_stats_collection_staged_index {
	struct sql_stats_index_summary *summary;
	struct sql_stats_sample_result sample;
};

struct sql_stats_collection_context {
	struct read_view view;
	struct sql_stats_collection_target *targets;
	uint32_t *index_unique_ids;
	struct sql_stats_snapshot *assembled_candidate;
	size_t target_count;
	uint64_t catalog_version;
	uint64_t schema_version;
	int64_t visibility_vclock_sum;
	bool view_open;
	bool failed;
};

static int
collection_summary_sink_consume(void *arg, const char *tuple,
				size_t tuple_size, const uint32_t *field_ids,
				size_t field_count)
{
	return sql_stats_index_summary_consume(arg, tuple, tuple_size, field_ids,
						field_count);
}

static bool
collection_context_is_valid(const struct sql_stats_collection_context *context)
{
	if (context == NULL || !context->view_open || context->failed ||
	    box_schema_version() != context->schema_version ||
	    box_catalog_version() != context->catalog_version)
		return false;
	for (size_t i = 0; i < context->target_count; i++) {
		struct space *space = space_by_id_slow(context->targets[i].space_id);
		struct index *index = space != NULL ?
			space_index(space, context->targets[i].index_id) : NULL;
		if (index == NULL || index->unique_id != context->index_unique_ids[i])
			return false;
	}
	return true;
}

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
	context->index_unique_ids = calloc(target_count,
						   sizeof(*context->index_unique_ids));
	if (context->index_unique_ids == NULL)
		goto fail;
	for (size_t i = 0; i < target_count; i++) {
		for (size_t j = i + 1; j < target_count; j++) {
			if (targets[i].space_id == targets[j].space_id &&
			    targets[i].index_id == targets[j].index_id)
				goto fail;
		}
	}
	uint64_t schema_before = box_schema_version();
	uint64_t catalog_before = box_catalog_version();
	struct read_view_opts opts;
	read_view_opts_create(&opts);
	opts.name = "sql-stats-collection";
	opts.enable_vinyl = true;
	opts.filter_space = context_filter_space;
	opts.filter_index = context_filter_index;
	opts.filter_arg = context;
	if (read_view_open(&context->view, &opts) != 0)
		goto fail;
	context->view_open = true;
	context->visibility_vclock_sum = vclock_sum(&context->view.vclock);
	if (context->visibility_vclock_sum < 0)
		goto fail;
	if (box_schema_version() != schema_before ||
	    box_catalog_version() != catalog_before)
		goto fail;
	for (size_t i = 0; i < target_count; i++) {
		struct index_read_view *index_view =
			context_get_index(context, &targets[i]);
		struct space *space = space_by_id_slow(targets[i].space_id);
		struct index *index = space != NULL ?
			space_index(space, targets[i].index_id) : NULL;
		if (index_view == NULL || index == NULL || index->def == NULL)
			goto fail;
		context->index_unique_ids[i] = index->unique_id;
	}
	context->catalog_version = catalog_before;
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
	if (context->assembled_candidate != NULL)
		sql_stats_snapshot_release(context->assembled_candidate);
	free(context->index_unique_ids);
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

static size_t
collection_context_target_index(
	const struct sql_stats_collection_context *context,
	const struct sql_stats_collection_target *target)
{
	for (size_t i = 0; i < context->target_count; i++) {
		if (context->targets[i].space_id == target->space_id &&
		    context->targets[i].index_id == target->index_id)
			return i;
	}
	return context->target_count;
}

struct sql_stats_snapshot *
sql_stats_collection_context_build_sample_candidate(
	struct sql_stats_collection_context *context,
	const struct sql_stats_expected_relation *expected,
	const struct sql_stats_tx_index_spec *specs, size_t spec_count,
	uint32_t relation_index_id, double relation_confidence,
	const char *confidence_source, size_t max_candidate_bytes,
	size_t max_staging_bytes, size_t max_temp_bytes, uint64_t max_work)
{
	struct sql_stats_collection_staged_index *staged = NULL;
	struct sql_stats_sampled_index *sampled_indexes = NULL;
	double *confidences = NULL;
	struct sql_stats_snapshot *candidate = NULL;
	bool valid = collection_context_is_valid(context) &&
		context->assembled_candidate == NULL && expected != NULL &&
		expected->index_count != 0 && expected->indexes != NULL &&
		specs != NULL && spec_count == expected->index_count &&
		spec_count == context->target_count &&
		spec_count <= SIZE_MAX / (sizeof(*staged) +
			sizeof(*sampled_indexes) + sizeof(*confidences)) &&
		isfinite(relation_confidence) && relation_confidence >= 0 &&
		relation_confidence <= 1 && confidence_source != NULL &&
		confidence_source[0] != '\0';
	if (!valid)
		goto fail;
	size_t metadata_bytes = spec_count * (sizeof(*staged) +
		sizeof(*sampled_indexes) + sizeof(*confidences));
	if (metadata_bytes > max_staging_bytes)
		goto fail;
	size_t total_staging_bytes = metadata_bytes;
	size_t max_reservoir_bytes = 0;
	size_t relation_sample_index = spec_count;
	for (size_t i = 0; i < spec_count; i++) {
		const struct sql_stats_tx_index_spec *spec = &specs[i];
		if (spec->expected == NULL || spec->extract == NULL ||
		    spec->target.space_id != expected->space_id ||
		    spec->target.index_id != spec->expected->index_id ||
		    spec->request.index_id != spec->target.index_id ||
		    spec->request.max_buffer_bytes == 0 ||
		    spec->expected->definition_version == 0 ||
		    spec->expected->part_count == 0 ||
		    spec->summary_max_bytes == 0 ||
		    total_staging_bytes > max_staging_bytes ||
		    spec->summary_max_bytes > max_staging_bytes -
						      total_staging_bytes)
			goto fail;
		total_staging_bytes += spec->summary_max_bytes;
		bool expected_match = false;
		for (size_t j = 0; j < expected->index_count; j++) {
			if (expected->indexes[j].index_id ==
				    spec->expected->index_id &&
			    expected->indexes[j].definition_version ==
				    spec->expected->definition_version &&
			    expected->indexes[j].part_count ==
				    spec->expected->part_count)
				expected_match = true;
		}
		for (size_t j = 0; j < i; j++) {
			if (specs[j].target.space_id == spec->target.space_id &&
			    specs[j].target.index_id == spec->target.index_id)
				goto fail;
		}
		size_t target_index = collection_context_target_index(context,
									 &spec->target);
		struct index_read_view *index_view =
			context_get_index(context, &spec->target);
		if (!expected_match || target_index == context->target_count ||
		    context->index_unique_ids[target_index] !=
			    spec->expected->definition_version || index_view == NULL ||
		    index_view->def->key_def->part_count !=
			    spec->expected->part_count)
			goto fail;
		if (spec->request.max_buffer_bytes > max_reservoir_bytes)
			max_reservoir_bytes = spec->request.max_buffer_bytes;
		if (spec->target.index_id == relation_index_id)
			relation_sample_index = i;
	}
	if (total_staging_bytes > max_staging_bytes ||
	    max_reservoir_bytes > max_staging_bytes - total_staging_bytes ||
	    relation_sample_index == spec_count)
		goto fail;
	staged = calloc(spec_count, sizeof(*staged));
	sampled_indexes = calloc(spec_count, sizeof(*sampled_indexes));
	confidences = calloc(spec_count, sizeof(*confidences));
	if (staged == NULL || sampled_indexes == NULL || confidences == NULL)
		goto fail;
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
			.consume = collection_summary_sink_consume,
		};
		struct sql_stats_sample_request request = spec->request;
		if (sql_stats_collection_context_sample_index(context,
				&spec->target, &request, &sink,
				&staged[i].sample) != 0)
			goto fail;
		sampled_indexes[i] = (struct sql_stats_sampled_index) {
			.expected = spec->expected,
			.sample = &staged[i].sample,
			.summary = staged[i].summary,
		};
	}
	if (!collection_context_is_valid(context))
		goto fail;
	struct sql_stats_collection_generation generation = {
		.catalog_version = context->catalog_version,
		.schema_version = context->schema_version,
		.visibility_id = context->view.id,
	};
	candidate = sql_stats_collection_build_sample_candidate(&generation,
		expected, sampled_indexes, spec_count,
		&staged[relation_sample_index].sample, relation_confidence,
		confidence_source, confidences, max_candidate_bytes,
		max_temp_bytes, max_work);
	if (candidate == NULL)
		goto fail;
	sql_stats_snapshot_retain(candidate);
	context->assembled_candidate = candidate;
	for (size_t i = 0; i < spec_count; i++)
		sql_stats_index_summary_delete(staged[i].summary);
	free(staged);
	free(sampled_indexes);
	free(confidences);
	return candidate;
fail:
	if (context != NULL)
		context->failed = true;
	if (candidate != NULL)
		sql_stats_snapshot_release(candidate);
	if (staged != NULL) {
		for (size_t i = 0; i < spec_count; i++)
			sql_stats_index_summary_delete(staged[i].summary);
	}
	free(staged);
	free(sampled_indexes);
	free(confidences);
	return NULL;
}

int
sql_stats_collection_context_publish_candidate(
	struct sql_stats_collection_context **context_ptr,
	struct sql_stats_snapshot *candidate)
{
	if (context_ptr == NULL || *context_ptr == NULL)
		return -1;
	struct sql_stats_collection_context *context = *context_ptr;
	if (candidate == NULL || context->assembled_candidate != candidate ||
	    !collection_context_is_valid(context) || sql_get() == NULL ||
	    box_vclock == NULL || vclock_sum(box_vclock) < 0 ||
	    vclock_sum(box_vclock) != context->visibility_vclock_sum ||
	    sql_stats_snapshot_catalog_version(candidate) !=
		    context->catalog_version ||
	    sql_stats_snapshot_schema_version(candidate) !=
		    context->schema_version) {
		context->failed = true;
		return -1;
	}
	/* No yield is allowed between generation revalidation and pointer swap. */
	sql_set_stats_snapshot(candidate);
	sql_stats_collection_context_delete(context);
	*context_ptr = NULL;
	return 0;
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
	    sink->consume == NULL || request->index_id != target->index_id ||
	    request->max_rows == 0 ||
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
