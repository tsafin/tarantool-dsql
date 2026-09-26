#include <lua.h>
#include <lauxlib.h>

#include "box/engine.h"
#include "box/error.h"
#include "box/space.h"
#include "box/space_cache.h"
#include "box/sql/sql_stats_sample.h"
#include "diag.h"

struct sample_capture {
	uint64_t calls;
};

static int
capture_tuple(void *context, const char *tuple, size_t tuple_size,
	      const uint32_t *field_ids, size_t field_count)
{
	(void)tuple;
	(void)tuple_size;
	(void)field_ids;
	(void)field_count;
	struct sample_capture *capture = context;
	capture->calls++;
	return 0;
}

static int
lbox_sql_stats_sample_vinyl(lua_State *L)
{
	uint32_t space_id = (uint32_t)luaL_checkinteger(L, 1);
	struct space *space = space_by_id(space_id);
	struct sql_stats_sample_request request = {
		.max_rows = 4,
		.max_bytes = 1024,
		.seed = 1,
	};
	struct sample_capture capture = {};
	struct sql_stats_sample_sink sink = {
		.context = &capture,
		.consume = capture_tuple,
	};
	struct sql_stats_sample_result result = {};
	diag_clear(diag_get());
	int rc = engine_sql_stats_sample(space, &request, &sink, &result);
	struct error *err = diag_last_error(diag_get());
	lua_pushinteger(L, rc);
	lua_pushinteger(L, err != NULL ? box_error_code((box_error_t *)err) : 0);
	lua_pushinteger(L, result.rows);
	lua_pushinteger(L, capture.calls);
	return 4;
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
