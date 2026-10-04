#include <lua.h>
#include <lauxlib.h>

#include "box/index.h"
#include "box/schema.h"
#include "box/space_cache.h"
#include "box/sql.h"
#include "box/sql/sql_stats_snapshot.h"
#include "box/sql/sqlInt.h"
#include "box/sql/vdbeInt.h"

static int
lbox_install_snapshot(lua_State *L)
{
	uint32_t space_id = luaL_checkinteger(L, 1);
	uint32_t index_id = luaL_checkinteger(L, 2);
	uint64_t relation_rows = luaL_checkinteger(L, 3);
	uint64_t index_rows = luaL_checkinteger(L, 4);
	uint64_t distinct_prefix = luaL_checkinteger(L, 5);
	bool stale = lua_toboolean(L, 6);
	uint64_t catalog_version = lua_isnoneornil(L, 7) ? 1 :
		luaL_checkinteger(L, 7);
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
		catalog_version, schema_version, &relation, 1, 64 * 1024);
	if (snapshot == NULL)
		return luaL_error(L, "failed to create SQL stats test snapshot");
	sql_set_stats_snapshot(snapshot);
	sql_stats_snapshot_release(snapshot);
	return 0;
}

static int
lbox_prepare_snapshot_stmt(lua_State *L)
{
	const char *sql = luaL_checkstring(L, 1);
	struct Vdbe *stmt = NULL;
	const char *tail = NULL;
	if (sql_stmt_compile(sql, -1, NULL, &stmt, &tail, true) != 0 ||
	    stmt == NULL)
		return luaL_error(L, "failed to compile SQL stats ownership fixture");
	lua_pushlightuserdata(L, stmt);
	return 1;
}

static int
lbox_stmt_snapshot_catalog_version(lua_State *L)
{
	struct Vdbe *stmt = lua_touserdata(L, 1);
	if (stmt == NULL)
		return luaL_error(L, "invalid SQL statement handle");
	if (stmt->stats_snapshot == NULL) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, sql_stats_snapshot_catalog_version(stmt->stats_snapshot));
	return 1;
}

static int
lbox_delete_snapshot_stmt(lua_State *L)
{
	struct Vdbe *stmt = lua_touserdata(L, 1);
	if (stmt == NULL)
		return luaL_error(L, "invalid SQL statement handle");
	sqlVdbeDelete(stmt);
	return 0;
}

/** Test-only selected WHERE output estimate for a plain top-level JOIN. */
static int
lbox_join_output_estimate(lua_State *L)
{
	const char *sql = luaL_checkstring(L, 1);
	struct Vdbe *stmt = NULL;
	const char *tail = NULL;
	if (sql_stmt_compile(sql, -1, NULL, &stmt, &tail, true) != 0 ||
	    stmt == NULL)
		return luaL_error(L, "failed to compile JOIN estimate fixture");
	if (!stmt->planner_join_output_valid) {
		sqlVdbeDelete(stmt);
		lua_pushnil(L);
		return 1;
	}
	uint64_t rows = sqlLogEstToInt(stmt->planner_join_output_logest);
	sqlVdbeDelete(stmt);
	lua_pushnumber(L, rows);
	return 1;
}

/** Test-only counters for the same selected JOIN planning invocation. */
static int
lbox_join_planner_metrics(lua_State *L)
{
	const char *sql = luaL_checkstring(L, 1);
	struct Vdbe *stmt = NULL;
	const char *tail = NULL;
	if (sql_stmt_compile(sql, -1, NULL, &stmt, &tail, true) != 0 ||
	    stmt == NULL)
		return luaL_error(L, "failed to compile JOIN metrics fixture");
	if (!stmt->planner_join_output_valid) {
		sqlVdbeDelete(stmt);
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	static const char *names[] = {
		"generated", "dominated", "truncated", "retained"
	};
	for (int i = 0; i < 4; i++) {
		lua_pushnumber(L, stmt->planner_path_metrics[i]);
		lua_setfield(L, -2, names[i]);
	}
	lua_pushnumber(L, stmt->planner_elapsed_us);
	lua_setfield(L, -2, "planner_elapsed_us");
	lua_pushnumber(L, stmt->planner_path_peak_frontier);
	lua_setfield(L, -2, "peak_frontier");
	lua_pushnumber(L, stmt->planner_path_peak_bytes);
	lua_setfield(L, -2, "peak_solver_bytes");
	sqlVdbeDelete(stmt);
	return 1;
}

/** Test-only selected INNER JOIN prefix estimates and FROM-position masks. */
static int
lbox_join_prefix_estimates(lua_State *L)
{
	const char *sql = luaL_checkstring(L, 1);
	struct Vdbe *stmt = NULL;
	const char *tail = NULL;
	if (sql_stmt_compile(sql, -1, NULL, &stmt, &tail, true) != 0 ||
	    stmt == NULL)
		return luaL_error(L, "failed to compile JOIN prefix fixture");
	if (stmt->planner_join_prefix_count == 0) {
		sqlVdbeDelete(stmt);
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	for (int i = 0; i < stmt->planner_join_prefix_count; i++) {
		lua_newtable(L);
		lua_pushnumber(L, stmt->planner_join_prefix_masks[i]);
		lua_setfield(L, -2, "relation_mask");
		lua_pushnumber(L,
			       sqlLogEstToInt(stmt->planner_join_prefix_logest[i]));
		lua_setfield(L, -2, "estimated_rows");
		lua_rawseti(L, -2, i + 1);
	}
	sqlVdbeDelete(stmt);
	return 1;
}

/** Test-only selected INNER JOIN prefix estimates and executed row counts. */
static int
lbox_join_prefix_actuals(lua_State *L)
{
	const char *sql = luaL_checkstring(L, 1);
	struct Vdbe *stmt = NULL;
	const char *tail = NULL;
	sql_test_join_prefix_counters_enable(true);
	int compile_rc = sql_stmt_compile(sql, -1, NULL, &stmt, &tail, true);
	sql_test_join_prefix_counters_enable(false);
	if (compile_rc != 0 || stmt == NULL)
		return luaL_error(L, "failed to compile JOIN prefix fixture");
	if (stmt->planner_join_prefix_count == 0 ||
	    stmt->planner_join_prefix_counter_count !=
	    stmt->planner_join_prefix_count) {
		sqlVdbeDelete(stmt);
		lua_pushnil(L);
		return 1;
	}
	int rc;
	while ((rc = sql_step(stmt)) == SQL_ROW) {
	}
	if (rc != SQL_DONE) {
		sqlVdbeDelete(stmt);
		return luaL_error(L, "failed to execute JOIN prefix fixture");
	}
	lua_newtable(L);
	for (int i = 0; i < stmt->planner_join_prefix_count; i++) {
		lua_newtable(L);
		lua_pushnumber(L, stmt->planner_join_prefix_masks[i]);
		lua_setfield(L, -2, "relation_mask");
		lua_pushnumber(L,
			       sqlLogEstToInt(stmt->planner_join_prefix_logest[i]));
		lua_setfield(L, -2, "estimated_rows");
		lua_pushnumber(L, stmt->planner_join_prefix_actuals[i]);
		lua_setfield(L, -2, "actual_rows");
		lua_rawseti(L, -2, i + 1);
	}
	sqlVdbeDelete(stmt);
	return 1;
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
	lua_createtable(L, 0, 3);
	lua_pushinteger(L, sql_stats_snapshot_relation_count(snapshot));
	lua_setfield(L, -2, "relation_count");
	lua_pushnumber(L, sql_stats_snapshot_catalog_version(snapshot));
	lua_setfield(L, -2, "catalog_version");
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
		{"prepare", lbox_prepare_snapshot_stmt},
		{"stmt_catalog_version", lbox_stmt_snapshot_catalog_version},
		{"delete_stmt", lbox_delete_snapshot_stmt},
		{"join_output_estimate", lbox_join_output_estimate},
		{"join_planner_metrics", lbox_join_planner_metrics},
		{"join_prefix_estimates", lbox_join_prefix_estimates},
		{"join_prefix_actuals", lbox_join_prefix_actuals},
		{NULL, NULL},
	};
	luaL_register(L, "sql_stats_snapshot_test", methods);
	return 1;
}
