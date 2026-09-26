#include <assert.h>
#include <string.h>

#include "unit.h"
#include "box/sql/sql_plan_descriptor.h"

static void
test_descriptor_owns_input(void)
{
	plan(6);
	header();
	char canonical[] = "col(c0) > 7";
	char space_name[] = "t1";
	struct sql_plan_expression expr[] = {{1, canonical}};
	struct sql_plan_bound bound[] = {{SQL_PLAN_LOWER, SQL_PLAN_GT, 1}};
	struct sql_plan_filter filter[] = {{1, 0.4, 0.8}};
	struct sql_plan_order_term order[] = {{0, SQL_PLAN_ASC, 1}};
	struct sql_plan_finalize fin[] = {{SQL_PLAN_SORT, order, 1, 0, 0}};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER, .space_id = 512,
		.space_name = space_name,
		.access = {.kind = SQL_PLAN_INDEX_RANGE_SCAN, .index_id = 0,
			.bounds = bound, .bound_count = 1,
			.produced_order = order, .produced_order_count = 1,
			.est_rows = 12, .est_rows_confidence = 0.75},
		.filters = filter, .filter_count = 1,
		.finalize = fin, .finalize_count = 1,
		.expressions = expr, .expression_count = 1,
		.cost_startup = 0, .cost_total = 12, .cost_rows = 4,
		.cost_row_width = 16, .cost_confidence = 0.7,
	};
	struct sql_plan_descriptor *d = sql_plan_descriptor_new(&input);
	assert(d != NULL);
	strcpy(canonical, "col(c0) = 7");
	strcpy(space_name, "t2");
	ok(sql_plan_descriptor_version(d) == 1, "schema version retained");
	ok(sql_plan_descriptor_space_id(d) == 512, "space ID retained");
	ok(strcmp(sql_plan_descriptor_space_name(d), "t1") == 0,
	   "space label deep-copied");
	ok(sql_plan_descriptor_access_kind(d) == SQL_PLAN_INDEX_RANGE_SCAN,
	   "access path retained");
	ok(sql_plan_descriptor_filter_count(d) == 1, "filters retained");
	ok(sql_plan_descriptor_expression_count(d) == 1,
	   "expression table retained");
	sql_plan_descriptor_delete(d);
	footer();
	check_plan();
}

static void
test_rejects_invalid_contract(void)
{
	plan(4);
	header();
	struct sql_plan_expression expr[] = {{1, "x"}};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.access = {.kind = SQL_PLAN_TABLE_FULL_SCAN, .est_rows = 1,
			.est_rows_confidence = 1},
		.expressions = expr, .expression_count = 1,
		.cost_total = 1, .cost_confidence = 1,
	};
	struct sql_plan_descriptor *d = sql_plan_descriptor_new(&input);
	ok(d != NULL, "valid descriptor accepted");
	sql_plan_descriptor_delete(d);
	input.descriptor_version = 2;
	ok(sql_plan_descriptor_new(&input) == NULL, "unknown schema rejected");
	input.descriptor_version = 1;
	input.path_class = SQL_PLAN_FALLBACK;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "fallback requires a stable reason code");
	input.fallback_reason = 1;
	d = sql_plan_descriptor_new(&input);
	ok(d != NULL, "fallback with reason accepted");
	sql_plan_descriptor_delete(d);
	footer();
	check_plan();
}

int
main(void)
{
	test_descriptor_owns_input();
	test_rejects_invalid_contract();
	return 0;
}
