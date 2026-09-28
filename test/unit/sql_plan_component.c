#include <assert.h>
#include <string.h>

#include "unit.h"
#include "box/sql/sql_plan_component.h"

static void
test_uniform_fallback_uses_root_reason(void)
{
	plan(5);
	header();
	struct sql_plan_component_ledger ledger;
	sql_plan_component_ledger_create(&ledger);
	ok(sql_plan_component_add(&ledger, 1, 0, SQL_PLAN_COMPONENT_ROOT) ==
	   SQL_PLAN_COMPONENT_OK, "fallback root accepted");
	ok(sql_plan_component_add(&ledger, 2, 1,
			  SQL_PLAN_COMPONENT_SCALAR_SUBQUERY) ==
	   SQL_PLAN_COMPONENT_OK, "fallback child accepted");
	ok(sql_plan_component_set_route(&ledger, 1,
				 SQL_PLAN_COMPONENT_FALLBACK,
				 SQL_PLAN_FALLBACK_UNSUPPORTED_CTE) ==
	   SQL_PLAN_COMPONENT_OK, "root fallback reason recorded");
	ok(sql_plan_component_set_route(&ledger, 2,
				 SQL_PLAN_COMPONENT_FALLBACK,
				 SQL_PLAN_FALLBACK_UNSUPPORTED_SUBQUERY) ==
	   SQL_PLAN_COMPONENT_OK, "child fallback reason recorded");
	struct sql_plan_component_summary summary;
	ok(sql_plan_component_finalize(&ledger, &summary) ==
	   SQL_PLAN_COMPONENT_OK &&
	   summary.route == SQL_PLAN_COMPONENT_FALLBACK &&
	   summary.fallback_reason == SQL_PLAN_FALLBACK_UNSUPPORTED_CTE,
	   "uniform route summary keeps root reason");
	footer();
	check_plan();
}

static void
test_component_routes_and_mixed_summary(void)
{
	plan(12);
	header();
	struct sql_plan_component_ledger ledger;
	sql_plan_component_ledger_create(&ledger);
	ok(sql_plan_component_add(&ledger, 1, 0, SQL_PLAN_COMPONENT_ROOT) ==
	   SQL_PLAN_COMPONENT_OK, "root component accepted");
	ok(sql_plan_component_add(&ledger, 2, 1,
			  SQL_PLAN_COMPONENT_SCALAR_SUBQUERY) ==
	   SQL_PLAN_COMPONENT_OK, "child records parent and role");
	ok(sql_plan_component_add(&ledger, 4, 0,
			  SQL_PLAN_COMPONENT_ROOT) ==
	   SQL_PLAN_COMPONENT_INVALID, "multiple roots rejected");
	ok(sql_plan_component_add(&ledger, 3, 99,
			  SQL_PLAN_COMPONENT_FROM_SUBQUERY) ==
	   SQL_PLAN_COMPONENT_MISSING_PARENT, "unknown parent rejected");
	ok(sql_plan_component_add(&ledger, 1, 0, SQL_PLAN_COMPONENT_ROOT) ==
	   SQL_PLAN_COMPONENT_DUPLICATE_ID, "duplicate identity rejected");
	ok(sql_plan_component_set_route(&ledger, 1,
				 SQL_PLAN_COMPONENT_FALLBACK,
				 SQL_PLAN_FALLBACK_UNSUPPORTED_CTE) ==
	   SQL_PLAN_COMPONENT_OK, "fallback route records stable reason");
	ok(sql_plan_component_set_route(&ledger, 2,
				 SQL_PLAN_COMPONENT_DIRECT_VALUES,
				 SQL_PLAN_FALLBACK_NONE) ==
	   SQL_PLAN_COMPONENT_OK, "direct producer has explicit route class");
	ok(sql_plan_component_set_route(&ledger, 1,
				 SQL_PLAN_COMPONENT_FALLBACK,
				 SQL_PLAN_FALLBACK_UNSUPPORTED_CTE) ==
	   SQL_PLAN_COMPONENT_OK, "idempotent route assignment accepted");
	ok(sql_plan_component_set_route(&ledger, 1,
				 SQL_PLAN_COMPONENT_NEW_PLANNER,
				 SQL_PLAN_FALLBACK_NONE) ==
	   SQL_PLAN_COMPONENT_CONFLICT, "route overwrite rejected");
	struct sql_plan_component_summary summary;
	ok(sql_plan_component_finalize(&ledger, &summary) ==
	   SQL_PLAN_COMPONENT_OK, "complete component ledger finalizes");
	ok(summary.route == SQL_PLAN_COMPONENT_MIXED &&
	   summary.fallback_reason == SQL_PLAN_FALLBACK_NONE,
	   "mixed component routes suppress statement fallback reason");
	ok(summary.component_count == 2, "summary retains component count");
	footer();
	check_plan();
}

static void
test_embedded_dml_view_component_is_root(void)
{
	plan(5);
	header();
	struct sql_plan_component_ledger ledger;
	sql_plan_component_ledger_create(&ledger);
	ok(sql_plan_component_add(&ledger, 1, 0,
		SQL_PLAN_COMPONENT_DML_VIEW_MATERIALIZATION_ROOT) ==
	   SQL_PLAN_COMPONENT_OK, "DML view materialization root accepted");
	ok(sql_plan_component_add(&ledger, 2, 1,
		SQL_PLAN_COMPONENT_FROM_SUBQUERY) == SQL_PLAN_COMPONENT_OK,
	   "nested SELECT retains its child relationship");
	ok(sql_plan_component_set_route(&ledger, 1,
		SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
		SQL_PLAN_FALLBACK_NONE) == SQL_PLAN_COMPONENT_OK,
	   "embedded DML route recorded");
	ok(sql_plan_component_set_route(&ledger, 2,
		SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
		SQL_PLAN_FALLBACK_NONE) == SQL_PLAN_COMPONENT_OK,
	   "nested route recorded");
	struct sql_plan_component_summary summary;
	ok(sql_plan_component_finalize(&ledger, &summary) ==
	   SQL_PLAN_COMPONENT_OK &&
	   summary.route == SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
	   "summary resolves the embedded DML root route");
	footer();
	check_plan();
}

static void
test_trigger_component_roles(void)
{
	plan(9);
	header();
	struct sql_plan_component_ledger ledger;
	sql_plan_component_ledger_create(&ledger);
	ok(sql_plan_component_add(&ledger, 1, 0,
		SQL_PLAN_COMPONENT_TRIGGER_SELECT_ROOT) == SQL_PLAN_COMPONENT_OK,
	   "trigger SELECT root accepted");
	ok(sql_plan_component_add(&ledger, 2, 1,
		SQL_PLAN_COMPONENT_TRIGGER_SELECT) == SQL_PLAN_COMPONENT_OK,
	   "trigger SELECT child accepted");
	ok(ledger.records[0].id == 1 && ledger.records[0].parent_id == 0 &&
	   ledger.records[0].role == SQL_PLAN_COMPONENT_TRIGGER_SELECT_ROOT,
	   "trigger root identity and role are retained");
	ok(ledger.records[1].id == 2 && ledger.records[1].parent_id == 1 &&
	   ledger.records[1].role == SQL_PLAN_COMPONENT_TRIGGER_SELECT,
	   "trigger child retains its parent edge");
	ok(strcmp(sql_plan_component_role_name(
		SQL_PLAN_COMPONENT_TRIGGER_SELECT_ROOT), "trigger_select_root") == 0,
	   "trigger root role has a stable name");
	ok(strcmp(sql_plan_component_role_name(
		SQL_PLAN_COMPONENT_TRIGGER_SELECT), "trigger_select") == 0,
	   "trigger child role has a stable name");
	ok(sql_plan_component_set_route(&ledger, 1,
		SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
		SQL_PLAN_FALLBACK_NONE) == SQL_PLAN_COMPONENT_OK,
	   "trigger root route is recorded");
	ok(sql_plan_component_set_route(&ledger, 2,
		SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
		SQL_PLAN_FALLBACK_NONE) == SQL_PLAN_COMPONENT_OK,
	   "trigger child route is recorded");
	struct sql_plan_component_summary summary;
	ok(sql_plan_component_finalize(&ledger, &summary) ==
	   SQL_PLAN_COMPONENT_OK &&
	   summary.route == SQL_PLAN_COMPONENT_CURRENT_WHERE_C &&
	   summary.component_count == 2,
	   "trigger root and child finalize as a complete ledger");
	footer();
	check_plan();
}

static void
test_incomplete_and_bounded_ledger(void)
{
	plan(9);
	header();
	struct sql_plan_component_ledger ledger;
	sql_plan_component_ledger_create(&ledger);
	ok(sql_plan_component_add(&ledger, 1, 0, SQL_PLAN_COMPONENT_ROOT) ==
	   SQL_PLAN_COMPONENT_OK, "root accepted before route known");
	struct sql_plan_component_summary summary;
	ok(sql_plan_component_finalize(&ledger, &summary) ==
	   SQL_PLAN_COMPONENT_INCOMPLETE, "pending producer cannot finalize");
	ok(sql_plan_component_set_route(&ledger, 1,
				 SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
				 SQL_PLAN_FALLBACK_NONE) ==
	   SQL_PLAN_COMPONENT_OK, "legacy route is recorded explicitly");
	ok(sql_plan_component_finalize(&ledger, &summary) ==
	   SQL_PLAN_COMPONENT_OK, "single completed root finalizes");
	ok(summary.route == SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
	   "uniform summary mirrors root route");
	ok(sql_plan_component_add(&ledger, 2, 0,
			  SQL_PLAN_COMPONENT_FROM_SUBQUERY) ==
	   SQL_PLAN_COMPONENT_INVALID, "only root role may omit parent");
	ok(SQL_PLAN_COMPONENT_MAX > 1000,
	   "ledger capacity covers the reviewed thousand-row VALUES query");
	for (uint32_t id = 2; id <= SQL_PLAN_COMPONENT_MAX; id++) {
		assert(sql_plan_component_add(&ledger, id, 1,
			SQL_PLAN_COMPONENT_SCALAR_SUBQUERY) == SQL_PLAN_COMPONENT_OK);
	}
	ok(sql_plan_component_add(&ledger, SQL_PLAN_COMPONENT_MAX + 1, 1,
			  SQL_PLAN_COMPONENT_SCALAR_SUBQUERY) ==
	   SQL_PLAN_COMPONENT_OVERFLOW, "component capacity fails closed");
	ok(sql_plan_component_finalize(&ledger, &summary) ==
	   SQL_PLAN_COMPONENT_INCOMPLETE, "overflowed ledger cannot finalize");
	footer();
	check_plan();
}

int
main(void)
{
	test_uniform_fallback_uses_root_reason();
	test_component_routes_and_mixed_summary();
	test_embedded_dml_view_component_is_root();
	test_trigger_component_roles();
	test_incomplete_and_bounded_ledger();
	return 0;
}
