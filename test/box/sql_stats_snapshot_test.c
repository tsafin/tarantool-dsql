#include <lua.h>
#include <lauxlib.h>

#include "box/index.h"
#include "box/schema.h"
#include "box/space_cache.h"
#include "box/sql.h"
#include "box/sql/sql_stats_snapshot.h"
#include "box/sql/sqlInt.h"

static int
lbox_install_snapshot(lua_State *L)
{
	uint32_t space_id = luaL_checkinteger(L, 1);
	uint32_t index_id = luaL_checkinteger(L, 2);
	uint64_t relation_rows = luaL_checkinteger(L, 3);
	uint64_t index_rows = luaL_checkinteger(L, 4);
	uint64_t distinct_prefix = luaL_checkinteger(L, 5);
	bool stale = lua_toboolean(L, 6);
	struct sql_stats_index_input index = {
		.index_id = index_id,
		.tuple_count = index_rows,
		.tuple_count_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.population_basis = "test_visible",
		.ndv_basis = "test_visible",
		.definition_version = 1,
		.distinct_prefixes = distinct_prefix == 0 ? NULL :
			&distinct_prefix,
		.prefix_count = distinct_prefix == 0 ? 0 : 1,
	};
	struct sql_stats_relation_input relation = {
		.space_id = space_id,
		.row_count = relation_rows,
		.population_basis = "test_visible",
		.average_row_width = 8.5,
		.width_basis = "test_payload/test_rows",
		.width_denominator_count = relation_rows == 0 ? 1 : relation_rows,
		.confidence = 1,
		.confidence_source = "test_fixture",
		.cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.collected_at = 1,
		.modification_epoch = 1,
		.visibility_id = 1,
		.indexes = &index,
		.index_count = 1,
	};
	uint64_t schema_version = box_schema_version();
	if (stale && schema_version > 0)
		schema_version--;
	struct sql_stats_snapshot *snapshot = sql_stats_snapshot_new(
		1, schema_version, &relation, 1, 64 * 1024);
	if (snapshot == NULL)
		return luaL_error(L, "failed to create SQL stats test snapshot");
	sql_set_stats_snapshot(snapshot);
	sql_stats_snapshot_release(snapshot);
	return 0;
}

static int
lbox_clear_snapshot(lua_State *L)
{
	(void)L;
	sql_set_stats_snapshot(NULL);
	return 0;
}

static int
lbox_snapshot_state(lua_State *L)
{
	struct sql_stats_snapshot *snapshot = sql_get_stats_snapshot();
	if (snapshot == NULL) {
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 2);
	lua_pushinteger(L, sql_stats_snapshot_relation_count(snapshot));
	lua_setfield(L, -2, "relation_count");
	lua_newtable(L);
	size_t count = sql_stats_snapshot_relation_count(snapshot);
	for (size_t i = 0; i < count; i++) {
		const struct sql_stats_relation *relation = NULL;
		if (sql_stats_snapshot_relation_at(snapshot, i, &relation) !=
		    SQL_STATS_LOOKUP_AVAILABLE) {
			sql_stats_snapshot_release(snapshot);
			return luaL_error(L, "failed to enumerate SQL stats snapshot");
		}
		lua_createtable(L, 0, 2);
		lua_pushinteger(L, sql_stats_relation_space_id(relation));
		lua_setfield(L, -2, "space_id");
		lua_pushnumber(L, sql_stats_relation_row_count(relation));
		lua_setfield(L, -2, "row_count");
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "relations");
	sql_stats_snapshot_release(snapshot);
	return 1;
}

static int
lbox_estimates(lua_State *L)
{
	uint32_t space_id = luaL_checkinteger(L, 1);
	uint32_t index_id = luaL_checkinteger(L, 2);
	struct space *space = space_by_id(space_id);
	struct index *index = space == NULL ? NULL : space_index(space, index_id);
	if (index == NULL)
		return luaL_error(L, "SQL stats test index does not exist");
	lua_createtable(L, 0, 2);
	lua_pushinteger(L, sql_space_tuple_log_count(space));
	lua_setfield(L, -2, "relation");
	lua_pushinteger(L, index_field_tuple_est(index->def, 1));
	lua_setfield(L, -2, "prefix");
	return 1;
}

LUA_API int
luaopen_sql_stats_snapshot_test(lua_State *L)
{
	static const struct luaL_Reg methods[] = {
		{"install", lbox_install_snapshot},
		{"clear", lbox_clear_snapshot},
		{"state", lbox_snapshot_state},
		{"estimates", lbox_estimates},
		{NULL, NULL},
	};
	luaL_register(L, "sql_stats_snapshot_test", methods);
	return 1;
}
