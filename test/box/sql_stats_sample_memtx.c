#include <lua.h>
#include <lauxlib.h>
#include <string.h>

#include "box/engine.h"
#include "box/error.h"
#include "box/space_cache.h"
#include "box/sql/sql_stats_sample.h"
#include "diag.h"
#include "msgpuck.h"

struct sample_capture {
	lua_State *L;
	int ids;
	uint64_t calls;
	uint64_t bytes;
	bool fields_match;
};

static int
capture_tuple(void *context, const char *tuple, size_t size,
	      const uint32_t *fields, size_t count)
{
	struct sample_capture *capture = context;
	capture->fields_match &= count == 2 && fields != NULL &&
		fields[0] == 0 && fields[1] == 1;
	const char *data = tuple;
	if (mp_decode_array(&data) != 2 || mp_typeof(*data) != MP_UINT) {
		diag_set(ClientError, ER_ILLEGAL_PARAMS, "Unexpected sampled tuple");
		return -1;
	}
	lua_pushinteger(capture->L, mp_decode_uint(&data));
	lua_rawseti(capture->L, capture->ids, ++capture->calls);
	capture->bytes += size;
	return 0;
}

static void
set_integer(lua_State *L, const char *name, lua_Integer value)
{
	lua_pushinteger(L, value);
	lua_setfield(L, -2, name);
}

static int
lbox_sample(lua_State *L)
{
	struct space *space = space_by_id(luaL_checkinteger(L, 1));
	uint32_t fields[] = {0, 1};
	struct sql_stats_sample_request request = {
		.max_rows = luaL_checkinteger(L, 2),
		.max_bytes = luaL_checkinteger(L, 3),
		.seed = luaL_checkinteger(L, 4),
		.field_ids = fields,
		.field_count = 2,
	};
	const char *invalid = luaL_optstring(L, 5, "");
	lua_newtable(L);
	struct sample_capture capture = {
		.L = L, .ids = lua_gettop(L), .fields_match = true,
	};
	struct sql_stats_sample_sink sink = {
		.context = &capture, .consume = capture_tuple,
	};
	if (strcmp(invalid, "fields") == 0)
		request.field_ids = NULL;
	if (strcmp(invalid, "sink") == 0)
		sink.consume = NULL;
	struct sql_stats_sample_result result = {};
	diag_clear(diag_get());
	int rc = engine_sql_stats_sample(space,
		strcmp(invalid, "request") == 0 ? NULL : &request,
		&sink, &result);
	struct error *err = diag_last_error(diag_get());
	lua_newtable(L);
	set_integer(L, "rc", rc);
	set_integer(L, "code", err == NULL ? 0 : box_error_code((box_error_t *)err));
	set_integer(L, "rows", result.rows);
	set_integer(L, "bytes", result.bytes);
	set_integer(L, "delivered", capture.calls);
	set_integer(L, "delivered_bytes", capture.bytes);
	lua_pushboolean(L, result.with_replacement);
	lua_setfield(L, -2, "with_replacement");
	lua_pushboolean(L, capture.fields_match);
	lua_setfield(L, -2, "fields_match");
	lua_pushvalue(L, capture.ids);
	lua_setfield(L, -2, "ids");
	return 1;
}

LUA_API int
luaopen_sql_stats_sample_memtx(lua_State *L)
{
	static const struct luaL_Reg methods[] = {
		{"sample", lbox_sample}, {NULL, NULL},
	};
	luaL_register(L, "sql_stats_sample_memtx", methods);
	return 1;
}
