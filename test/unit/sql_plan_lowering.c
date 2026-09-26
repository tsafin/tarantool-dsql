#include <assert.h>

#include "box/sql/sql_plan_descriptor.h"
#include "box/sql/sql_plan_lowering.h"
#include "unit.h"

struct capture {
	enum sql_plan_lowering_stage stages[8];
	size_t count;
	int stop_after;
};

static int
capture_event(void *context, const struct sql_plan_lowering_event *event)
{
	struct capture *capture = context;
	assert(capture->count < sizeof(capture->stages) /
	       sizeof(capture->stages[0]));
	capture->stages[capture->count++] = event->stage;
	if (event->stage == SQL_PLAN_LOWER_FILTER)
		assert(event->filter != NULL && event->filter->expr_ref == 1);
	if (event->stage == SQL_PLAN_LOWER_SORT ||
	    event->stage == SQL_PLAN_LOWER_LIMIT)
		assert(event->finalize != NULL);
	return capture->stop_after == (int)capture->count ? 17 : 0;
}

static void
test_lowering_contract(void)
{
	plan(10);
	header();
	struct sql_plan_expression expression = {1, "col(c0) > 7"};
	struct sql_plan_filter filter = {1, 0.5, 1};
	struct sql_plan_order_term key = {0, SQL_PLAN_ASC, 1};
	struct sql_plan_finalize finalize[] = {
		{SQL_PLAN_SORT, &key, 1, 0, 0},
		{SQL_PLAN_LIMIT, NULL, 0, 10, 2},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER, .space_id = 512,
		.space_name = "t1",
		.access = {.kind = SQL_PLAN_TABLE_FULL_SCAN,
			.est_rows = 20, .est_rows_confidence = 1},
		.filters = &filter, .filter_count = 1,
		.finalize = finalize, .finalize_count = 2,
		.expressions = &expression, .expression_count = 1,
		.cost_total = 20, .cost_rows = 10, .cost_row_width = 8,
		.cost_confidence = 1,
	};
	struct sql_plan_descriptor *plan = sql_plan_descriptor_new(&input);
	assert(plan != NULL);
	struct capture capture = {0};
	ok(sql_plan_lower(plan, capture_event, &capture) == 0,
	   "supported descriptor emits lowering contract");
	const enum sql_plan_lowering_stage expected[] = {
		SQL_PLAN_LOWER_SCAN, SQL_PLAN_LOWER_FILTER,
		SQL_PLAN_LOWER_PROJECT, SQL_PLAN_LOWER_SORT,
		SQL_PLAN_LOWER_LIMIT, SQL_PLAN_LOWER_RESULT,
	};
	ok(capture.count == sizeof(expected) / sizeof(expected[0]),
	   "one ordered event per lowering stage");
	for (size_t i = 0; i < capture.count; ++i)
		ok(capture.stages[i] == expected[i],
		   "scan/filter/project/finalize/result order is stable");

	capture = (struct capture){.stop_after = 2};
	ok(sql_plan_lower(plan, capture_event, &capture) == 17 &&
	   capture.count == 2,
	   "backend failure stops lowering and propagates unchanged");
	ok(sql_plan_lower(plan, NULL, &capture) == -1 &&
	   sql_plan_lower(NULL, capture_event, &capture) == -1,
	   "invalid lowering inputs are rejected");
	sql_plan_descriptor_delete(plan);
	footer();
	check_plan();
}

int
main(void)
{
	test_lowering_contract();
	return 0;
}
