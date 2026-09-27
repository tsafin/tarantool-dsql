#include <lua.h>
#include <lauxlib.h>
#include <limits.h>
#include <stdio.h>

#include "box/error.h"
#include "box/index.h"
#include "box/read_view.h"
#include "box/schema.h"
#include "box/space.h"
#include "box/space_cache.h"
#include "box/sql/sql_stats_collection.h"
#include "box/sql/sql_stats_sample.h"
#include "box/sql/sqlInt.h"
#include "box/sql.h"
#include "diag.h"
#include "msgpuck.h"

struct tx_sample_capture {
	lua_State *L;
	int ids;
	uint64_t calls;
	uint64_t bytes;
};

struct tx_unsigned_extract {
	uint32_t field_id;
	uint64_t calls;
	uint64_t errors;
	bool fail;
};

struct live_read_view_filter {
	uint32_t space_ids[2];
	size_t space_count;
};

static struct read_view live_read_view;
static bool live_read_view_open;
static struct live_read_view_filter live_filter;
static struct sql_stats_collection_context *held_candidate_context;
static struct sql_stats_snapshot *held_candidate;

static bool
live_filter_space(struct space *space, void *arg)
{
	struct live_read_view_filter *filter = arg;
	for (size_t i = 0; i < filter->space_count; i++) {
		if (space_id(space) == filter->space_ids[i])
			return true;
	}
	return false;
}

static bool
live_filter_index(struct space *space, struct index *index, void *arg)
{
	struct live_read_view_filter *filter = arg;
	return live_filter_space(space, filter) && index->def->iid < 2;
}

static struct space_read_view *
live_read_view_space(uint32_t space_id)
{
	struct space_read_view *space_view;
	read_view_foreach_space(space_view, &live_read_view) {
		if (space_view->id == space_id)
			return space_view;
	}
	return NULL;
}

static int
extract_unsigned(void *arg, const char *tuple, size_t tuple_size,
		 const uint32_t *field_ids, size_t field_count,
		 struct sql_stats_hll_value *parts, size_t part_count)
{
	struct tx_unsigned_extract *state = arg;
	if (state->fail) {
		state->errors++;
		return -1;
	}
	const char *data = tuple;
	if (tuple_size == 0 || mp_typeof(*data) != MP_ARRAY) {
		state->errors++;
		return -1;
	}
	uint32_t arity = mp_decode_array(&data);
	bool found = false;
	uint64_t value = 0;
	for (uint32_t i = 0; i < arity; i++) {
		if (i == state->field_id) {
			if (mp_typeof(*data) != MP_UINT) {
				state->errors++;
				return -1;
			}
			value = mp_decode_uint(&data);
			found = true;
		} else {
			mp_next(&data);
		}
	}
	if (!found || field_ids == NULL || field_count != 1 ||
	    field_ids[0] != state->field_id || part_count != 1) {
		state->errors++;
		return -1;
	}
	char encoded[9];
	char *end = mp_encode_uint(encoded, value);
	parts[0] = (struct sql_stats_hll_value) {
		.type_tag = MP_UINT, .data = encoded, .size = end - encoded,
	};
	state->calls++;
	return 0;
}

static int
capture_tuple(void *arg, const char *tuple, size_t tuple_size,
	      const uint32_t *field_ids, size_t field_count)
{
	(void)field_ids;
	(void)field_count;
	struct tx_sample_capture *capture = arg;
	const char *data = tuple;
	if (mp_typeof(*data) != MP_ARRAY || mp_decode_array(&data) < 1) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "Unexpected tuple from SQL stats transaction sampler");
		return -1;
	}
	int64_t id;
	if (mp_typeof(*data) == MP_UINT) {
		uint64_t value = mp_decode_uint(&data);
		if (value > INT64_MAX) {
			diag_set(ClientError, ER_ILLEGAL_PARAMS,
				 "Sampled tuple key is outside Lua integer range");
			return -1;
		}
		id = value;
	} else if (mp_typeof(*data) == MP_INT) {
		id = mp_decode_int(&data);
	} else {
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "Unexpected tuple key type from SQL stats sampler");
		return -1;
	}
	lua_pushinteger(capture->L, id);
	lua_rawseti(capture->L, capture->ids, ++capture->calls);
	capture->bytes += tuple_size;
	return 0;
}

static void
set_uint64(lua_State *L, const char *name, uint64_t value)
{
	lua_pushnumber(L, (lua_Number)value);
	lua_setfield(L, -2, name);
}

static int
lbox_sample(lua_State *L)
{
	uint32_t space_id = (uint32_t)luaL_checkinteger(L, 1);
	uint32_t index_id = (uint32_t)luaL_checkinteger(L, 2);
	uint64_t max_rows = luaL_optinteger(L, 3, 4);
	uint32_t field_ids[] = {0, 1};
	struct sql_stats_collection_target target = {
		.space_id = space_id,
		.index_id = index_id,
	};
	struct sql_stats_sample_request request = {
		.index_id = index_id,
		.max_rows = max_rows,
		.max_bytes = 1024 * 1024,
		.seed = 19,
		.max_buffer_bytes = 1024 * 1024,
		.max_tuples_examined = 10000,
		.max_disk_sources = 10000,
		.max_page_reads = 10000,
		.max_iterator_keys = 10000,
		.field_ids = field_ids,
		.field_count = sizeof(field_ids) / sizeof(field_ids[0]),
	};
	struct sql_stats_sample_result sample_result = {};
	struct sql_stats_tx_context *context = NULL;
	struct tx_sample_capture capture = {
		.L = L,
	};
	struct sql_stats_sample_sink sink = {
		.context = &capture,
		.consume = capture_tuple,
	};
	int begin_rc;
	int sample_rc = -1;
	int finish_rc = -1;
	struct error *err;

	lua_newtable(L);
	capture.ids = lua_gettop(L);
	diag_clear(diag_get());
	begin_rc = sql_stats_tx_context_begin(&target, 1, &context);
	if (begin_rc == 0) {
		sample_rc = sql_stats_tx_context_sample_index(context, &target,
			&request, &sink, &sample_result);
		finish_rc = sql_stats_tx_context_finish(&context);
	}
	err = diag_last_error(diag_get());
	lua_newtable(L);
	lua_pushinteger(L, begin_rc);
	lua_setfield(L, -2, "begin_rc");
	lua_pushinteger(L, sample_rc);
	lua_setfield(L, -2, "sample_rc");
	lua_pushinteger(L, finish_rc);
	lua_setfield(L, -2, "finish_rc");
	lua_pushinteger(L, err == NULL ? 0 : box_error_code((box_error_t *)err));
	lua_setfield(L, -2, "error_code");
	set_uint64(L, "rows", sample_result.rows);
	set_uint64(L, "bytes", sample_result.bytes);
	set_uint64(L, "population", sample_result.visible_population);
	set_uint64(L, "delivered", capture.calls);
	set_uint64(L, "delivered_bytes", capture.bytes);
	lua_pushboolean(L, sample_result.population_known);
	lua_setfield(L, -2, "population_known");
	lua_pushvalue(L, capture.ids);
	lua_setfield(L, -2, "ids");
	return 1;
}

static int
lbox_collect_candidate(lua_State *L)
{
	uint32_t space_id = (uint32_t)luaL_checkinteger(L, 1);
	struct space *space = space_by_id_slow(space_id);
	struct sql_stats_collection_target targets[2];
	struct sql_stats_expected_index expected_indexes[2];
	struct tx_unsigned_extract extracts[2] = {};
	struct sql_stats_tx_index_spec specs[2] = {};
	uint32_t field_ids[2] = {0, 1};
	bool setup_ok = space != NULL;
	for (uint32_t i = 0; setup_ok && i < 2; i++) {
		struct index *index = space_index(space, i);
		if (index == NULL || index->def->key_def->part_count != 1) {
			setup_ok = false;
			break;
		}
		targets[i] = (struct sql_stats_collection_target) {
			.space_id = space_id, .index_id = i,
		};
		expected_indexes[i] = (struct sql_stats_expected_index) {
			.index_id = i, .definition_version = index->unique_id,
			.part_count = 1,
		};
		extracts[i].field_id = i;
		specs[i].target = targets[i];
		specs[i].expected = &expected_indexes[i];
		specs[i].request = (struct sql_stats_sample_request) {
			.index_id = i, .max_rows = 4, .max_bytes = 1024 * 1024,
			.seed = 19 + i, .max_buffer_bytes = 1024 * 1024,
			.max_tuples_examined = 10000, .max_disk_sources = 10000,
			.max_page_reads = 10000, .max_iterator_keys = 10000,
			.field_ids = &field_ids[i], .field_count = 1,
		};
		specs[i].hll_precision = 8;
		specs[i].hll_seed = 19 + i;
		specs[i].summary_max_bytes = 1024;
		specs[i].extract = extract_unsigned;
		specs[i].extract_context = &extracts[i];
	}
	struct sql_stats_expected_relation expected = {
		.space_id = space_id, .modification_epoch = 1,
		.indexes = expected_indexes, .index_count = 2,
	};
	struct sql_stats_tx_context *context = NULL;
	int begin_rc = setup_ok ? sql_stats_tx_context_begin(targets, 2,
										 &context) : -1;
	struct sql_stats_snapshot *candidate = NULL;
	if (begin_rc == 0) {
		candidate = sql_stats_tx_context_build_sample_candidate(context,
			&expected, specs, 2, 0, 0.5, "live_test", 4096,
			8192, 1024, 1000000);
	}
	int candidate_built = candidate != NULL;
	double relation_rows = 0;
	uint64_t width_rows = 0, primary_rows = 0, secondary_rows = 0;
	if (candidate != NULL) {
		const struct sql_stats_relation *relation = NULL;
		const struct sql_stats_index *primary = NULL, *secondary = NULL;
		if (sql_stats_snapshot_get_relation(candidate,
				sql_stats_tx_context_schema_version(context), space_id,
				&relation) == SQL_STATS_LOOKUP_AVAILABLE &&
		    sql_stats_relation_get_index(relation, 0, &primary) ==
				SQL_STATS_LOOKUP_AVAILABLE &&
		    sql_stats_relation_get_index(relation, 1, &secondary) ==
				SQL_STATS_LOOKUP_AVAILABLE) {
			relation_rows = sql_stats_relation_row_count(relation);
			width_rows = sql_stats_relation_width_denominator_count(relation);
			primary_rows = sql_stats_index_tuple_count(primary);
			secondary_rows = sql_stats_index_tuple_count(secondary);
		}
	}
	int finish_rc = -1;
	if (context != NULL) {
		if (candidate != NULL) {
			finish_rc = sql_stats_tx_context_finish_sample_candidate_and_publish(
				&context, candidate);
		} else {
			finish_rc = sql_stats_tx_context_finish(&context);
		}
	}
	lua_newtable(L);
	lua_pushinteger(L, begin_rc);
	lua_setfield(L, -2, "begin_rc");
	lua_pushinteger(L, candidate_built);
	lua_setfield(L, -2, "candidate_built");
	lua_pushinteger(L, finish_rc);
	lua_setfield(L, -2, "finish_rc");
	lua_pushnumber(L, relation_rows);
	lua_setfield(L, -2, "relation_rows");
	set_uint64(L, "width_rows", width_rows);
	set_uint64(L, "primary_rows", primary_rows);
	set_uint64(L, "secondary_rows", secondary_rows);
	for (uint32_t i = 0; i < 2; i++) {
		char key[32];
		snprintf(key, sizeof(key), "extract_%u_calls", i);
		set_uint64(L, key, extracts[i].calls);
		snprintf(key, sizeof(key), "extract_%u_errors", i);
		set_uint64(L, key, extracts[i].errors);
	}
	if (candidate != NULL)
		sql_stats_snapshot_release(candidate);
	return 1;
}

static int
lbox_collect_view_candidate(lua_State *L)
{
	bool hold = lua_toboolean(L, 2);
	bool keep_installed = lua_toboolean(L, 3);
	size_t staging_bytes = luaL_optinteger(L, 4, 8192);
	if (hold && (held_candidate_context != NULL || held_candidate != NULL))
		return luaL_error(L, "a held candidate already exists");
	uint32_t space_id = (uint32_t)luaL_checkinteger(L, 1);
	struct space *space = space_by_id_slow(space_id);
	struct sql_stats_collection_target targets[2];
	struct sql_stats_expected_index expected_indexes[2];
	struct tx_unsigned_extract extracts[2] = {};
	struct sql_stats_tx_index_spec specs[2] = {};
	uint32_t field_ids[2] = {0, 1};
	bool setup_ok = space != NULL;
	for (uint32_t i = 0; setup_ok && i < 2; i++) {
		struct index *index = space_index(space, i);
		if (index == NULL || index->def->key_def->part_count != 1) {
			setup_ok = false;
			break;
		}
		targets[i] = (struct sql_stats_collection_target) {
			.space_id = space_id, .index_id = i,
		};
		expected_indexes[i] = (struct sql_stats_expected_index) {
			.index_id = i, .definition_version = index->unique_id,
			.part_count = 1,
		};
		extracts[i].field_id = i;
		specs[i].target = targets[i];
		specs[i].expected = &expected_indexes[i];
		specs[i].request = (struct sql_stats_sample_request) {
			.index_id = i, .max_rows = 4, .max_bytes = 1024 * 1024,
			.seed = 31 + i, .max_buffer_bytes = 4096,
			.max_tuples_examined = 10000, .max_disk_sources = 10000,
			.max_page_reads = 10000, .max_iterator_keys = 10000,
			.field_ids = &field_ids[i], .field_count = 1,
		};
		specs[i].hll_precision = 8;
		specs[i].hll_seed = 31 + i;
		specs[i].summary_max_bytes = 1024;
		specs[i].extract = extract_unsigned;
		specs[i].extract_context = &extracts[i];
	}
	struct sql_stats_expected_relation expected = {
		.space_id = space_id, .modification_epoch = 1,
		.indexes = expected_indexes, .index_count = 2,
	};
	struct sql_stats_collection_context *context = setup_ok ?
		sql_stats_collection_context_new(targets, 2) : NULL;
	struct sql_stats_snapshot *candidate = context != NULL ?
		sql_stats_collection_context_build_sample_candidate(context,
			&expected, specs, 2, 0, 0.5, "live_view_test", 4096,
			staging_bytes, 1024, 1000000) : NULL;
	double relation_rows = 0;
	uint64_t width_rows = 0, primary_rows = 0, secondary_rows = 0;
	if (candidate != NULL) {
		const struct sql_stats_relation *relation = NULL;
		const struct sql_stats_index *primary = NULL, *secondary = NULL;
		if (sql_stats_snapshot_get_relation(candidate, box_schema_version(),
				space_id, &relation) == SQL_STATS_LOOKUP_AVAILABLE &&
		    sql_stats_relation_get_index(relation, 0, &primary) ==
				SQL_STATS_LOOKUP_AVAILABLE &&
		    sql_stats_relation_get_index(relation, 1, &secondary) ==
				SQL_STATS_LOOKUP_AVAILABLE) {
			relation_rows = sql_stats_relation_row_count(relation);
			width_rows =
				sql_stats_relation_width_denominator_count(relation);
			primary_rows = sql_stats_index_tuple_count(primary);
			secondary_rows = sql_stats_index_tuple_count(secondary);
		}
	}
	int candidate_built = candidate != NULL;
	int publish_rc = -1;
	if (hold && candidate != NULL) {
		held_candidate_context = context;
		held_candidate = candidate;
		context = NULL;
		candidate = NULL;
		publish_rc = 1;
	} else if (candidate != NULL) {
		publish_rc = sql_stats_collection_context_publish_candidate(&context,
										 candidate);
	}
	if (context != NULL)
		sql_stats_collection_context_delete(context);
	if (candidate != NULL)
		sql_stats_snapshot_release(candidate);
	if (!hold && !keep_installed)
		/* Do not leak this test candidate into subsequent SQL statements. */
		sql_set_stats_snapshot(NULL);
	lua_newtable(L);
	lua_pushinteger(L, candidate_built);
	lua_setfield(L, -2, "candidate_built");
	lua_pushinteger(L, publish_rc);
	lua_setfield(L, -2, "publish_rc");
	lua_pushnumber(L, relation_rows);
	lua_setfield(L, -2, "relation_rows");
	lua_pushinteger(L, width_rows);
	lua_setfield(L, -2, "width_rows");
	lua_pushinteger(L, primary_rows);
	lua_setfield(L, -2, "primary_rows");
	lua_pushinteger(L, secondary_rows);
	lua_setfield(L, -2, "secondary_rows");
	lua_pushboolean(L, sql_get()->stats_snapshot != NULL);
	lua_setfield(L, -2, "has_installed_snapshot");
	return 1;
}

static int
lbox_publish_held_candidate(lua_State *L)
{
	(void)L;
	if (held_candidate_context == NULL || held_candidate == NULL)
		return luaL_error(L, "no held candidate exists");
	struct sql_stats_snapshot *installed_before = sql_get()->stats_snapshot;
	int rc = sql_stats_collection_context_publish_candidate(
		&held_candidate_context, held_candidate);
	bool preserved = sql_get()->stats_snapshot == installed_before;
	if (held_candidate_context != NULL)
		sql_stats_collection_context_delete(held_candidate_context);
	sql_stats_snapshot_release(held_candidate);
	held_candidate_context = NULL;
	held_candidate = NULL;
	sql_set_stats_snapshot(NULL);
	lua_newtable(L);
	lua_pushinteger(L, rc);
	lua_setfield(L, -2, "rc");
	lua_pushboolean(L, preserved);
	lua_setfield(L, -2, "preserved");
	return 1;
}

static int
lbox_visibility_open(lua_State *L)
{
	if (live_read_view_open)
		return luaL_error(L, "a live SQL stats read view is already open");
	live_filter.space_ids[0] = (uint32_t)luaL_checkinteger(L, 1);
	live_filter.space_count = lua_isnoneornil(L, 2) ? 1 : 2;
	if (live_filter.space_count == 2)
		live_filter.space_ids[1] = (uint32_t)luaL_checkinteger(L, 2);
	struct space *space[2] = {
		space_by_id_slow(live_filter.space_ids[0]), NULL,
	};
	if (live_filter.space_count == 2)
		space[1] = space_by_id_slow(live_filter.space_ids[1]);
	for (size_t i = 0; i < live_filter.space_count; i++) {
		if (space[i] == NULL || space_index(space[i], 0) == NULL ||
		    space_index(space[i], 1) == NULL)
			return luaL_error(L,
				"expected spaces with primary and secondary indexes");
		if (i != 0 && space[i]->engine == space[0]->engine)
			return luaL_error(L, "expected spaces from distinct engines");
	}
	struct read_view_opts opts;
	read_view_opts_create(&opts);
	opts.name = "sql-stats-live-test";
	opts.filter_space = live_filter_space;
	opts.filter_index = live_filter_index;
	opts.filter_arg = &live_filter;
	opts.enable_vinyl = true;
	int rc = read_view_open(&live_read_view, &opts);
	if (rc == 0)
		live_read_view_open = true;
	lua_newtable(L);
	lua_pushinteger(L, rc);
	lua_setfield(L, -2, "rc");
	if (rc == 0) {
		lua_pushnumber(L, live_read_view.id);
		lua_setfield(L, -2, "id");
	}
	return 1;
}

static double
snapshot_relation_rows(const struct sql_stats_snapshot *snapshot,
		       uint32_t space_id)
{
	const struct sql_stats_relation *relation = NULL;
	if (sql_stats_snapshot_get_relation(snapshot, box_schema_version(),
					     space_id, &relation) !=
	    SQL_STATS_LOOKUP_AVAILABLE)
		return -1;
	return sql_stats_relation_row_count(relation);
}

static int
lbox_collect_multirelation(lua_State *L)
{
	uint32_t space_ids[2] = {
		(uint32_t)luaL_checkinteger(L, 1),
		(uint32_t)luaL_checkinteger(L, 2),
	};
	struct sql_stats_collection_target targets[4];
	struct sql_stats_expected_index expected_indexes[2][2];
	struct tx_unsigned_extract extracts[4] = {};
	struct sql_stats_tx_index_spec index_specs[4] = {};
	struct sql_stats_collection_relation_spec relations[2] = {};
	uint32_t field_ids[4] = {0, 1, 0, 1};
	bool setup_ok = space_ids[0] != 0 && space_ids[1] != 0 &&
		space_ids[0] != space_ids[1];
	for (size_t r = 0; setup_ok && r < 2; r++) {
		struct space *space = space_by_id_slow(space_ids[r]);
		if (space == NULL) {
			setup_ok = false;
			break;
		}
		for (uint32_t i = 0; i < 2; i++) {
			struct index *index = space_index(space, i);
			if (index == NULL || index->def->key_def->part_count != 1) {
				setup_ok = false;
				break;
			}
			size_t flat = r * 2 + i;
			targets[flat] = (struct sql_stats_collection_target) {
				.space_id = space_ids[r], .index_id = i,
			};
			expected_indexes[r][i] = (struct sql_stats_expected_index) {
				.index_id = i, .definition_version = index->unique_id,
				.part_count = index->def->key_def->part_count,
			};
			extracts[flat].field_id = i;
			index_specs[flat].target = targets[flat];
			index_specs[flat].expected = &expected_indexes[r][i];
			index_specs[flat].request = (struct sql_stats_sample_request) {
				.index_id = i, .max_rows = 4, .max_bytes = 1024 * 1024,
				.seed = 41 + flat, .max_buffer_bytes = 4096,
				.max_tuples_examined = 10000, .max_disk_sources = 10000,
				.max_page_reads = 10000, .max_iterator_keys = 10000,
				.field_ids = &field_ids[flat], .field_count = 1,
			};
			index_specs[flat].hll_precision = 8;
			index_specs[flat].hll_seed = 41 + flat;
			index_specs[flat].summary_max_bytes = 4096;
			index_specs[flat].extract = extract_unsigned;
			index_specs[flat].extract_context = &extracts[flat];
		}
		relations[r] = (struct sql_stats_collection_relation_spec) {
			.expected = NULL,
			.indexes = &index_specs[r * 2], .index_count = 2,
			.relation_index_id = 0, .relation_confidence = 0.5,
			.confidence_source = "multi_live_test",
		};
	}
	struct sql_stats_expected_relation expected_relations[2] = {};
	for (size_t r = 0; setup_ok && r < 2; r++) {
		expected_relations[r] = (struct sql_stats_expected_relation) {
			.space_id = space_ids[r], .modification_epoch = 1,
			.indexes = expected_indexes[r], .index_count = 2,
		};
		relations[r].expected = &expected_relations[r];
	}
	struct sql_stats_collection_build_budget budget = {
		.max_index_requests = 4, .max_staging_bytes = 65536,
		.max_candidate_bytes = 65536, .max_temp_bytes = 4096,
		.max_work = 1000000,
	};
	struct sql_stats_collection_context *context = setup_ok ?
		sql_stats_collection_context_new(targets, 4) : NULL;
	struct sql_stats_snapshot *candidate = context != NULL ?
		sql_stats_collection_context_build_sample_candidates(context,
			relations, 2, &budget) : NULL;
	int candidate_built = candidate != NULL;
	struct sql_stats_snapshot *old_snapshot = sql_get()->stats_snapshot;
	if (old_snapshot != NULL)
		sql_stats_snapshot_retain(old_snapshot);
	int publish_rc = candidate != NULL ?
		sql_stats_collection_context_publish_candidate(&context, candidate) : -1;
	if (context != NULL)
		sql_stats_collection_context_delete(context);
	double success_rows[2] = {
		snapshot_relation_rows(sql_get()->stats_snapshot, space_ids[0]),
		snapshot_relation_rows(sql_get()->stats_snapshot, space_ids[1]),
	};
	struct sql_stats_snapshot *published_snapshot = sql_get()->stats_snapshot;
	bool published_two_relations = candidate_built && publish_rc == 0 &&
		published_snapshot != NULL &&
		sql_stats_snapshot_relation_count(published_snapshot) == 2 &&
		published_snapshot != old_snapshot;
	if (candidate != NULL)
		sql_stats_snapshot_release(candidate);

	/* Fail in the later (Vinyl) relation after the first relation has already
	 * built a private part. No part may escape or replace the installed view. */
	extracts[2].fail = true;
	context = sql_stats_collection_context_new(targets, 4);
	struct sql_stats_snapshot *failed_candidate = context != NULL ?
		sql_stats_collection_context_build_sample_candidates(context,
			relations, 2, &budget) : NULL;
	bool failed_late = failed_candidate == NULL;
	if (context != NULL)
		sql_stats_collection_context_delete(context);
	if (failed_candidate != NULL)
		sql_stats_snapshot_release(failed_candidate);
	bool preserved = failed_late &&
		sql_get()->stats_snapshot == published_snapshot &&
		snapshot_relation_rows(sql_get()->stats_snapshot, space_ids[0]) ==
			success_rows[0] &&
		snapshot_relation_rows(sql_get()->stats_snapshot, space_ids[1]) ==
			success_rows[1];
	sql_set_stats_snapshot(old_snapshot);
	if (old_snapshot != NULL)
		sql_stats_snapshot_release(old_snapshot);

	lua_newtable(L);
	lua_pushboolean(L, published_two_relations);
	lua_setfield(L, -2, "published_two_relations");
	lua_pushinteger(L, publish_rc);
	lua_setfield(L, -2, "publish_rc");
	lua_pushnumber(L, success_rows[0]);
	lua_setfield(L, -2, "first_relation_rows");
	lua_pushnumber(L, success_rows[1]);
	lua_setfield(L, -2, "second_relation_rows");
	lua_pushboolean(L, failed_late);
	lua_setfield(L, -2, "later_relation_failed_closed");
	lua_pushboolean(L, preserved);
	lua_setfield(L, -2, "installed_snapshot_preserved");
	return 1;
}

static void
lbox_capture_read_view_index(lua_State *L, struct index_read_view *index_view)
{
	lua_newtable(L);
	struct index_read_view_iterator iterator;
	if (index_read_view_create_iterator(index_view, ITER_ALL, NULL, 0,
					    &iterator) != 0)
		luaL_error(L, "failed to create read-view iterator");
	uint32_t row = 0;
	for (;;) {
		struct read_view_tuple tuple;
		if (index_read_view_iterator_next_raw(&iterator, &tuple) != 0) {
			index_read_view_iterator_destroy(&iterator);
			luaL_error(L, "failed to advance read-view iterator");
		}
		if (tuple.data == NULL)
			break;
		const char *data = tuple.data;
		if (tuple.size == 0 || mp_typeof(*data) != MP_ARRAY ||
		    mp_decode_array(&data) == 0 || mp_typeof(*data) != MP_UINT) {
			index_read_view_iterator_destroy(&iterator);
			luaL_error(L, "expected unsigned first tuple field");
		}
		uint64_t id = mp_decode_uint(&data);
		if (id > INT_MAX) {
			index_read_view_iterator_destroy(&iterator);
			luaL_error(L, "tuple id exceeds Lua test integer range");
		}
		lua_pushinteger(L, (lua_Integer)id);
		lua_rawseti(L, -2, ++row);
	}
	index_read_view_iterator_destroy(&iterator);
}

static int
lbox_visibility_scan(lua_State *L)
{
	if (!live_read_view_open)
		return luaL_error(L, "no live SQL stats read view is open");
	uint32_t space_id = (uint32_t)luaL_checkinteger(L, 1);
	struct space_read_view *space_view = live_read_view_space(space_id);
	if (space_view == NULL)
		return luaL_error(L, "target space is absent from read view");
	struct index_read_view *primary = space_read_view_index(space_view, 0);
	struct index_read_view *secondary = space_read_view_index(space_view, 1);
	if (primary == NULL || secondary == NULL)
		return luaL_error(L, "target index is absent from read view");
	lua_newtable(L);
	lbox_capture_read_view_index(L, primary);
	lua_setfield(L, -2, "primary");
	lbox_capture_read_view_index(L, secondary);
	lua_setfield(L, -2, "secondary");
	return 1;
}

static int
lbox_visibility_close(lua_State *L)
{
	(void)L;
	if (live_read_view_open) {
		read_view_close(&live_read_view);
		live_read_view_open = false;
	}
	return 0;
}

LUA_API int
luaopen_sql_stats_tx_context_test(lua_State *L)
{
	static const struct luaL_Reg methods[] = {
		{"sample", lbox_sample},
		{"collect_candidate", lbox_collect_candidate},
		{"collect_view_candidate", lbox_collect_view_candidate},
		{"collect_multirelation", lbox_collect_multirelation},
		{"publish_held_candidate", lbox_publish_held_candidate},
		{"visibility_open", lbox_visibility_open},
		{"visibility_scan", lbox_visibility_scan},
		{"visibility_close", lbox_visibility_close},
		{NULL, NULL},
	};
	luaL_register(L, "sql_stats_tx_context_test", methods);
	return 1;
}
