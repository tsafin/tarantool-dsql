#include <lua.h>
#include <lauxlib.h>

#include "box/error.h"
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

LUA_API int
luaopen_sql_stats_tx_context_test(lua_State *L)
{
	static const struct luaL_Reg methods[] = {
		{"sample", lbox_sample},
		{NULL, NULL},
	};
	luaL_register(L, "sql_stats_tx_context_test", methods);
	return 1;
}
