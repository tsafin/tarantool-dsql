#include <string.h>

#include "box/sql/sql_plan_fallback.h"
#include "unit.h"

static void
test_reason_mapping(void)
{
	plan(25);
	header();
	const struct {
		enum sql_logical_reject_reason input;
		enum sql_plan_fallback_reason output;
		const char *name;
	} logical[] = {
		{SQL_LOGICAL_REJECT_UNRESOLVED,
		 SQL_PLAN_FALLBACK_UNRESOLVED_INPUT, "UNRESOLVED_INPUT"},
		{SQL_LOGICAL_REJECT_RELATION_COUNT,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_RELATION_COUNT,
		 "UNSUPPORTED_RELATION_COUNT"},
		{SQL_LOGICAL_REJECT_SUBQUERY,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_SUBQUERY, "UNSUPPORTED_SUBQUERY"},
		{SQL_LOGICAL_REJECT_AGGREGATE,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_AGGREGATE, "UNSUPPORTED_AGGREGATE"},
		{SQL_LOGICAL_REJECT_COMPOUND,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_COMPOUND, "UNSUPPORTED_COMPOUND"},
		{SQL_LOGICAL_REJECT_CTE,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_CTE, "UNSUPPORTED_CTE"},
		{SQL_LOGICAL_REJECT_DISTINCT,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_DISTINCT, "UNSUPPORTED_DISTINCT"},
		{SQL_LOGICAL_REJECT_NONDETERMINISTIC,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_NONDETERMINISTIC,
		 "UNSUPPORTED_NONDETERMINISTIC"},
		{SQL_LOGICAL_REJECT_ACCESS_HINT,
		 SQL_PLAN_FALLBACK_UNSUPPORTED_ACCESS_HINT,
		 "UNSUPPORTED_ACCESS_HINT"},
	};
	for (size_t i = 0; i < sizeof(logical) / sizeof(logical[0]); ++i) {
	enum sql_plan_fallback_reason reason =
		sql_plan_fallback_from_logical(logical[i].input);
	ok(reason == logical[i].output, "logical rejection maps to stable code");
	ok(strcmp(sql_plan_fallback_reason_name(reason), logical[i].name) == 0,
	   "logical reason has stable external name");
	}
	const struct {
		enum sql_physical_reject_reason input;
		enum sql_plan_fallback_reason output;
		const char *name;
	} physical[] = {
		{SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN,
		 SQL_PLAN_FALLBACK_INVALID_LOGICAL_PLAN, "INVALID_LOGICAL_PLAN"},
		{SQL_PHYSICAL_REJECT_NO_ACCESS_PATH,
		 SQL_PLAN_FALLBACK_NO_ACCESS_PATH, "NO_ACCESS_PATH"},
		{SQL_PHYSICAL_REJECT_INVALID_CANDIDATE,
		 SQL_PLAN_FALLBACK_INVALID_CANDIDATE, "INVALID_CANDIDATE"},
	};
	for (size_t i = 0; i < sizeof(physical) / sizeof(physical[0]); ++i) {
	enum sql_plan_fallback_reason reason =
		sql_plan_fallback_from_physical(physical[i].input);
	ok(reason == physical[i].output, "physical rejection maps to stable code");
	ok(strcmp(sql_plan_fallback_reason_name(reason), physical[i].name) == 0,
	   "physical reason has stable external name");
	}
	ok(sql_plan_fallback_reason_name(0) == NULL &&
	   sql_plan_fallback_reason_name(999) == NULL,
	   "none and unknown reason codes have no external name");
	footer();
	check_plan();
}

static void
test_producer_contract(void)
{
	plan(9);
	header();
	struct sql_plan_producer_result result;
	ok(sql_plan_producer_result_init(&result, NULL,
		SQL_LOGICAL_REJECT_SUBQUERY, SQL_PHYSICAL_REJECT_NO_ACCESS_PATH),
	   "logical rejection produces observable fallback result");
	ok(result.path_class == SQL_PLAN_FALLBACK &&
	   result.fallback_reason == SQL_PLAN_FALLBACK_UNSUPPORTED_SUBQUERY &&
	   result.descriptor == NULL,
	   "logical reason takes precedence and identifies fallback path");
	ok(sql_plan_producer_result_init(&result, NULL, SQL_LOGICAL_REJECT_NONE,
		SQL_PHYSICAL_REJECT_NO_ACCESS_PATH) &&
	   result.path_class == SQL_PLAN_FALLBACK &&
	   result.fallback_reason == SQL_PLAN_FALLBACK_NO_ACCESS_PATH,
	   "physical rejection produces observable fallback result");
	ok(!sql_plan_producer_result_init(&result, NULL, SQL_LOGICAL_REJECT_NONE,
		SQL_PHYSICAL_REJECT_NONE),
	   "missing plan and rejection is not a valid producer result");
	ok(!sql_plan_producer_result_init(NULL, NULL,
		SQL_LOGICAL_REJECT_SUBQUERY, SQL_PHYSICAL_REJECT_NONE),
	   "producer result requires output storage");
	struct sql_plan_expression expression = {1, "true"};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER, .space_id = 512,
		.space_name = "t1",
		.access = {.kind = SQL_PLAN_TABLE_FULL_SCAN},
		.expressions = &expression, .expression_count = 1,
		.cost_confidence = 1,
	};
	struct sql_plan_descriptor *descriptor = sql_plan_descriptor_new(&input);
	ok(descriptor != NULL && sql_plan_producer_result_init(&result,
		descriptor, SQL_LOGICAL_REJECT_NONE, SQL_PHYSICAL_REJECT_NONE),
	   "new-planner descriptor produces observable success result");
	ok(result.path_class == SQL_PLAN_NEW_PLANNER &&
	   result.fallback_reason == SQL_PLAN_FALLBACK_NONE &&
	   result.descriptor == descriptor,
	   "success result exposes descriptor and new-planner path class");
	sql_plan_descriptor_delete(descriptor);
	input.path_class = SQL_PLAN_FALLBACK;
	input.fallback_reason = SQL_PLAN_FALLBACK_NO_ACCESS_PATH;
	descriptor = sql_plan_descriptor_new(&input);
	ok(descriptor != NULL,
	   "descriptor accepts a known stable fallback reason");
	sql_plan_descriptor_delete(descriptor);
	input.fallback_reason = 999;
	ok(sql_plan_descriptor_new(&input) == NULL,
	   "descriptor rejects unknown fallback reason codes");
	footer();
	check_plan();
}

int
main(void)
{
	test_reason_mapping();
	test_producer_contract();
	return 0;
}
