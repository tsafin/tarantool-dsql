#include <stdlib.h>
#include <string.h>

#include "box/sql/sqlInt.h"
#include "box/sql/sql_logical_plan.h"
#include "box/sql/sql_physical_plan.h"
#include "box/space.h"
#include "unit.h"

static struct sql_logical_plan *
make_logical_plan(struct space_def **def_out)
{
	struct space_def *def = calloc(1, sizeof(*def) + sizeof("t1"));
	strcpy(def->name, "t1");
	def->id = 512;
	struct space *space = calloc(1, sizeof(*space));
	space->def = def;
	struct SrcList *source = calloc(1, sizeof(*source));
	source->nSrc = 1;
	source->a[0].space = space;
	struct ExprList_item item = {0};
	struct ExprList projection = {.nExpr = 1, .a = &item};
	struct Select select = {
		.selFlags = SF_Resolved,
		.pEList = &projection,
		.pSrc = source,
	};
	struct sql_logical_plan *plan =
		sql_logical_plan_from_select(&select, NULL);
	/* The prototype borrows the resolved query tree for the call only. */
	free(source);
	free(space);
	*def_out = def;
	return plan;
}

static void
test_access_path_choices(void)
{
	plan(10);
	header();
	struct space_def *def;
	struct sql_logical_plan *logical = make_logical_plan(&def);
	struct sql_plan_bound bound = {SQL_PLAN_LOWER, SQL_PLAN_EQ, 1};
	struct sql_plan_expression expression = {1, "param(1)"};
	struct sql_physical_candidate candidate = {
		.access = {.kind = SQL_PLAN_PK_POINT_LOOKUP, .index_id = 0,
			.bounds = &bound, .bound_count = 1,
			.est_rows = 1, .est_rows_confidence = 1},
		.expressions = &expression, .expression_count = 1,
		.startup_cost = 0, .total_cost = 1, .rows = 1,
		.row_width = 8, .confidence = 1,
	};
	enum sql_physical_reject_reason reason;
	struct sql_plan_descriptor *physical = sql_physical_plan_from_logical(
		logical, &candidate, 1, &reason);
	ok(physical != NULL && reason == SQL_PHYSICAL_REJECT_NONE,
	   "primary point candidate becomes a physical descriptor");
	ok(sql_plan_descriptor_space_id(physical) == 512,
	   "physical scan inherits resolved relation identity");
	ok(sql_plan_descriptor_access_kind(physical) == SQL_PLAN_PK_POINT_LOOKUP,
	   "primary point access is preserved");
	sql_plan_descriptor_delete(physical);

	enum sql_plan_access_kind kinds[] = {
		SQL_PLAN_INDEX_POINT_LOOKUP, SQL_PLAN_INDEX_RANGE_SCAN,
		SQL_PLAN_INDEX_FULL_SCAN, SQL_PLAN_TABLE_FULL_SCAN,
	};
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
		candidate.access.kind = kinds[i];
		candidate.access.index_id = 2;
		candidate.access.bound_count = 0;
		if (kinds[i] == SQL_PLAN_INDEX_POINT_LOOKUP) {
			candidate.access.bounds = &bound;
			candidate.access.bound_count = 1;
		}
		if (kinds[i] == SQL_PLAN_INDEX_RANGE_SCAN) {
			bound.op = SQL_PLAN_GE;
			candidate.access.bounds = &bound;
			candidate.access.bound_count = 1;
		}
		physical = sql_physical_plan_from_logical(logical, &candidate, 1,
							  &reason);
		ok(physical != NULL && sql_plan_descriptor_access_kind(physical) ==
		   kinds[i], "secondary/full access candidate is represented");
		sql_plan_descriptor_delete(physical);
	}

	struct sql_physical_candidate alternatives[2] = {candidate, candidate};
	alternatives[0].access.kind = SQL_PLAN_TABLE_FULL_SCAN;
	alternatives[0].access.bound_count = 0;
	alternatives[0].total_cost = 9;
	alternatives[1].access.kind = SQL_PLAN_INDEX_FULL_SCAN;
	alternatives[1].access.bound_count = 0;
	alternatives[1].total_cost = 3;
	physical = sql_physical_plan_from_logical(logical, alternatives, 2,
							  &reason);
	ok(physical != NULL && sql_plan_descriptor_access_kind(physical) ==
	   SQL_PLAN_INDEX_FULL_SCAN,
	   "lowest estimated total cost selects the index full scan");
	sql_plan_descriptor_delete(physical);

	physical = sql_physical_plan_from_logical(logical, NULL, 0, &reason);
	ok(physical == NULL && reason == SQL_PHYSICAL_REJECT_NO_ACCESS_PATH,
	   "absence of candidates has stable rejection reason");
	candidate.access.kind = SQL_PLAN_INDEX_RANGE_SCAN;
	candidate.access.bound_count = 0;
	physical = sql_physical_plan_from_logical(logical, &candidate, 1, &reason);
	ok(physical == NULL && reason == SQL_PHYSICAL_REJECT_INVALID_CANDIDATE,
	   "malformed candidate has stable rejection reason");
	sql_logical_plan_delete(logical);
	free(def);
	footer();
	check_plan();
}

int
main(void)
{
	test_access_path_choices();
	return 0;
}
