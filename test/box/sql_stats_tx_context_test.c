#include <lua.h>
#include <lauxlib.h>
#include <stdio.h>

#include "box/error.h"
#include "box/index.h"
#include "box/space.h"
#include "box/space_cache.h"
#include "box/sql/sql_stats_collection.h"
#include "box/sql/sql_stats_sample.h"
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
};

static int
extract_unsigned(void *arg, const char *tuple, size_t tuple_size,
		 const uint32_t *field_ids, size_t field_count,
		 struct sql_stats_hll_value *parts, size_t part_count)
{
	struct tx_unsigned_extract *state = arg;
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

LUA_API int
luaopen_sql_stats_tx_context_test(lua_State *L)
{
	static const struct luaL_Reg methods[] = {
		{"sample", lbox_sample},
		{"collect_candidate", lbox_collect_candidate},
		{NULL, NULL},
	};
	luaL_register(L, "sql_stats_tx_context_test", methods);
	return 1;
}
