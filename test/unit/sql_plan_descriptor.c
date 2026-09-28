#include <assert.h>
#include <limits.h>
#include <string.h>

#include "unit.h"
#include "box/sql/sql_plan_descriptor.h"

static void
test_descriptor_owns_input(void)
{
	plan(9);
	header();
	char canonical[] = "col(c0) > 7";
	char space_name[] = "t1";
	struct sql_plan_expression expr[] = {{1, canonical}};
	struct sql_plan_bound bound[] = {{SQL_PLAN_LOWER, SQL_PLAN_GT, 1}};
	struct sql_plan_filter filter[] = {{
		.expr_ref = 1, .selectivity = 0.4, .confidence = 0.8,
	}};
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
	filter[0].op = SQL_PLAN_FILTER_IS_NULL;
	filter[0].column = 3;
	d = sql_plan_descriptor_new(&input);
	ok(d != NULL, "typed null filter descriptor accepted");
	sql_plan_descriptor_delete(d);
	filter[0].column = (uint32_t)INT_MAX + 1;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "typed null filter rejects a column outside VDBE integer range");
	filter[0].op = (enum sql_plan_filter_op)INT_MAX;
	filter[0].column = 0;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "unknown typed filter operation rejected");
	footer();
	check_plan();
}

static void
test_rejects_invalid_contract(void)
{
	plan(6);
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
	ok(sql_plan_descriptor_get_input(NULL) == NULL &&
	   sql_plan_descriptor_version(NULL) == 0,
	   "NULL descriptor getters are safe");
	input.descriptor_version = 1;
	input.path_class = SQL_PLAN_FALLBACK;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "fallback requires a stable reason code");
	input.fallback_reason = 1;
	d = sql_plan_descriptor_new(&input);
	ok(d != NULL, "fallback with reason accepted");
	sql_plan_descriptor_delete(d);
	input.path_class = (enum sql_plan_path_class)-1;
	input.fallback_reason = 0;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "negative enum values rejected");
	footer();
	check_plan();
}

static void
test_composite_prefix_range_contract(void)
{
	plan(6);
	header();
	struct sql_plan_expression expr[] = {
		{1, "prefix-equality"}, {2, "lower-bound"}, {3, "upper-bound"},
	};
	struct sql_plan_point_key_part prefix[] = {
		{.column = 0, .integer_value = 1},
	};
	struct sql_plan_bound bounds[] = {
		{SQL_PLAN_LOWER, SQL_PLAN_EQ, 1},
		{SQL_PLAN_LOWER, SQL_PLAN_GT, 2},
		{SQL_PLAN_UPPER, SQL_PLAN_LE, 3},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.access = {
			.kind = SQL_PLAN_PK_PREFIX_SCAN,
			.prefix_key_parts = prefix,
			.prefix_key_part_count = 1,
			.has_unsigned_range_key = true,
			.unsigned_range_key = 20,
			.integer_range_op = SQL_PLAN_GT,
			.has_unsigned_range_end_key = true,
			.unsigned_range_end_key = 40,
			.integer_range_end_op = SQL_PLAN_LE,
			.range_key_column = 1,
			.bounds = bounds,
			.bound_count = 3,
			.direction = SQL_PLAN_ASC,
		},
		.expressions = expr, .expression_count = 3,
		.cost_total = 1, .cost_confidence = 1,
	};
	struct sql_plan_descriptor *d = sql_plan_descriptor_new(&input);
	ok(d != NULL, "bounded unsigned suffix range descriptor accepted");
	sql_plan_descriptor_delete(d);

	bounds[2].op = SQL_PLAN_LT;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "suffix upper bound must match the endpoint operator");
	bounds[2].op = SQL_PLAN_LE;
	input.access.has_unsigned_range_end_key = false;
	input.access.has_integer_range_end_key = true;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "suffix range endpoints must use the same key type");
	input.access.has_integer_range_end_key = false;
	input.access.has_unsigned_range_end_key = true;
	bounds[0].op = SQL_PLAN_GE;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "composite prefix bounds must be equality terms");

	struct sql_plan_bound upper_only[] = {
		{SQL_PLAN_LOWER, SQL_PLAN_EQ, 1},
		{SQL_PLAN_UPPER, SQL_PLAN_LT, 3},
	};
	input.access.bounds = upper_only;
	input.access.bound_count = 2;
	input.access.integer_range_op = SQL_PLAN_LT;
	input.access.has_integer_range_key = false;
	input.access.has_unsigned_range_key = true;
	input.access.has_integer_range_end_key = false;
	input.access.has_unsigned_range_end_key = false;
	ok(sql_plan_descriptor_new(&input) != NULL,
	   "one-sided unsigned suffix upper bound descriptor accepted");
	input.access.direction = SQL_PLAN_DESC;
	d = sql_plan_descriptor_new(&input);
	ok(d != NULL, "descending suffix range descriptor accepted");
	sql_plan_descriptor_delete(d);
	footer();
	check_plan();
}

static void
test_expression_projection_contract(void)
{
	plan(4);
	header();
	static const uint32_t columns[] = {2, UINT32_MAX};
	static const uint32_t refs[] = {0, 7};
	static const struct sql_plan_expression expressions[] = {
		{7, "literal(9)"},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.access = {.kind = SQL_PLAN_TABLE_FULL_SCAN},
		.projection_columns = columns, .projection_column_count = 2,
		.projection_expr_refs = refs,
		.expressions = expressions, .expression_count = 1,
	};
	struct sql_plan_descriptor *d = sql_plan_descriptor_new(&input);
	const struct sql_plan_descriptor_input *copy =
		sql_plan_descriptor_get_input(d);
	ok(copy != NULL && copy->projection_expr_refs != refs &&
	   copy->projection_expr_refs[0] == 0 &&
	   copy->projection_expr_refs[1] == 7,
	   "projection expression refs are copied with direct/expression slots");
	sql_plan_descriptor_delete(d);
	uint32_t bad_refs[] = {7, 0};
	input.projection_expr_refs = bad_refs;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "expression reference requires UINT32_MAX projection sentinel");
	uint32_t missing_refs[] = {0, 8};
	input.projection_expr_refs = missing_refs;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "projection expression reference must resolve in descriptor table");
	input.projection_expr_refs = NULL;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "sentinel cannot be used without expression reference array");
	footer();
	check_plan();
}

int
main(void)
{
	test_descriptor_owns_input();
	test_rejects_invalid_contract();
	test_composite_prefix_range_contract();
	test_expression_projection_contract();
	return 0;
}
