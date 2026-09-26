#include <lua.h>
#include <lauxlib.h>

#include "box/engine.h"
#include "box/error.h"
#include "box/space.h"
#include "box/space_cache.h"
#include "box/sql/sql_stats_sample.h"
#include "diag.h"
#include "msgpuck.h"

struct sample_capture {
	lua_State *L;
	int ids;
	uint64_t calls;
	bool fail;
};

static int
capture_tuple(void *context, const char *tuple, size_t tuple_size,
	      const uint32_t *field_ids, size_t field_count)
{
	(void)field_ids;
	(void)field_count;
	struct sample_capture *capture = context;
	if (capture->fail) {
		diag_set(ClientError, ER_SQL_EXECUTE, "Sample sink failure");
		return -1;
	}
	const char *data = tuple;
	if (mp_decode_array(&data) != 2 || mp_typeof(*data) != MP_UINT) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS,
			 "Unexpected Vinyl sample tuple");
		return -1;
	}
	lua_pushinteger(capture->L, mp_decode_uint(&data));
	lua_rawseti(capture->L, capture->ids, ++capture->calls);
	(void)tuple_size;
	return 0;
}

static void
set_integer(lua_State *L, const char *name, uint64_t value)
{
	lua_pushnumber(L, (lua_Number)value);
	lua_setfield(L, -2, name);
}

static int
lbox_sql_stats_sample_vinyl(lua_State *L)
{
	uint32_t space_id = (uint32_t)luaL_checkinteger(L, 1);
	struct space *space = space_by_id(space_id);
	struct sql_stats_sample_request request = {
		.max_rows = luaL_optinteger(L, 2, 4),
		.max_bytes = luaL_optinteger(L, 3, 1024),
		.seed = luaL_optinteger(L, 4, 1),
		.max_tuples_examined = luaL_optinteger(L, 5, 10000),
		.max_disk_sources = luaL_optinteger(L, 6, 10000),
		.max_page_reads = luaL_optinteger(L, 7, 10000),
		.max_iterator_keys = luaL_optinteger(L, 8, 10000),
		.max_buffer_bytes = luaL_optinteger(L, 9, 1024 * 1024),
	};
	lua_newtable(L);
	struct sample_capture capture = {
		.L = L,
		.ids = lua_gettop(L),
		.fail = lua_toboolean(L, 10),
	};
	struct sql_stats_sample_sink sink = {
		.context = &capture,
		.consume = capture_tuple,
	};
	struct sql_stats_sample_result result = {};
	diag_clear(diag_get());
	int rc = engine_sql_stats_sample(space, &request, &sink, &result);
	struct error *err = diag_last_error(diag_get());
	lua_newtable(L);
	lua_pushinteger(L, rc);
	lua_setfield(L, -2, "rc");
	lua_pushinteger(L, err != NULL ? box_error_code((box_error_t *)err) : 0);
	lua_setfield(L, -2, "code");
	set_integer(L, "rows", result.rows);
	set_integer(L, "bytes", result.bytes);
	set_integer(L, "delivered", capture.calls);
	set_integer(L, "population", result.visible_population);
	lua_pushboolean(L, result.population_known);
	lua_setfield(L, -2, "population_known");
	lua_pushboolean(L, result.with_replacement);
	lua_setfield(L, -2, "with_replacement");
	lua_pushvalue(L, capture.ids);
	lua_setfield(L, -2, "ids");
	return 1;
}

LUA_API int
luaopen_sql_stats_sample_vinyl(lua_State *L)
{
	static const struct luaL_Reg methods[] = {
		{"sample", lbox_sql_stats_sample_vinyl},
		{NULL, NULL},
	};
	luaL_register(L, "sql_stats_sample_vinyl", methods);
	return 1;
}
