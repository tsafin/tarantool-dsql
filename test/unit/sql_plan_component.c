#include <assert.h>

#include "unit.h"
#include "box/sql/sql_plan_component.h"

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
test_incomplete_and_bounded_ledger(void)
{
	plan(8);
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
	test_component_routes_and_mixed_summary();
	test_incomplete_and_bounded_ledger();
	return 0;
}
