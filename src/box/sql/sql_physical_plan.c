#include "sql_physical_plan.h"

#include <stdbool.h>
#include <math.h>

#include "sql_logical_plan.h"

static const struct sql_logical_node *
find_scan(const struct sql_logical_plan *logical)
{
	const struct sql_logical_node *node = sql_logical_plan_root(logical);
	while (node != NULL && node->op != SQL_LOGICAL_SCAN)
		node = node->input;
	return node;
}

static bool
candidate_is_better(const struct sql_physical_candidate *a,
		    const struct sql_physical_candidate *b)
{
	if (b == NULL || a->total_cost < b->total_cost)
		return true;
	if (a->total_cost > b->total_cost)
		return false;
	if (a->access.kind != b->access.kind)
		return a->access.kind < b->access.kind;
	return a->access.index_id < b->access.index_id;
}

struct sql_plan_descriptor *
sql_physical_plan_from_logical(const struct sql_logical_plan *logical,
			       const struct sql_physical_candidate *candidates,
			       size_t candidate_count,
			       enum sql_physical_reject_reason *reason)
{
	if (reason != NULL)
		*reason = SQL_PHYSICAL_REJECT_NONE;
	const struct sql_logical_node *scan = find_scan(logical);
	if (scan == NULL || scan->space_name == NULL) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN;
		return NULL;
	}
	if (candidates == NULL || candidate_count == 0) {
		if (reason != NULL)
			*reason = SQL_PHYSICAL_REJECT_NO_ACCESS_PATH;
		return NULL;
	}
	const struct sql_physical_candidate *best = NULL;
	struct sql_plan_descriptor *best_plan = NULL;
	bool invalid_candidate = false;
	for (size_t i = 0; i < candidate_count; ++i) {
		const struct sql_physical_candidate *candidate = &candidates[i];
		if (!isfinite(candidate->startup_cost) ||
		    !isfinite(candidate->total_cost) ||
		    !isfinite(candidate->rows) || !isfinite(candidate->row_width) ||
		    !isfinite(candidate->confidence) ||
		    candidate->startup_cost < 0 ||
		    candidate->total_cost < candidate->startup_cost ||
		    candidate->rows < 0 || candidate->row_width < 0 ||
		    candidate->confidence < 0 || candidate->confidence > 1) {
			invalid_candidate = true;
			continue;
		}
		if (!candidate_is_better(candidate, best))
			continue;
		struct sql_plan_descriptor_input input = {
			.descriptor_version = 1,
			.planner_version = 1,
			.path_class = SQL_PLAN_NEW_PLANNER,
			.space_id = scan->space_id,
			.space_name = scan->space_name,
			.access = candidate->access,
			.expressions = candidate->expressions,
			.expression_count = candidate->expression_count,
			.cost_startup = candidate->startup_cost,
			.cost_total = candidate->total_cost,
			.cost_rows = candidate->rows,
			.cost_row_width = candidate->row_width,
			.cost_confidence = candidate->confidence,
		};
		struct sql_plan_descriptor *plan = sql_plan_descriptor_new(&input);
		if (plan == NULL) {
			invalid_candidate = true;
			continue;
		}
		sql_plan_descriptor_delete(best_plan);
		best_plan = plan;
		best = candidate;
	}
	if (best_plan == NULL && reason != NULL)
		*reason = invalid_candidate ? SQL_PHYSICAL_REJECT_INVALID_CANDIDATE :
			SQL_PHYSICAL_REJECT_NO_ACCESS_PATH;
	return best_plan;
}
