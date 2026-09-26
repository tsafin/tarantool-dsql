#include "sql_plan_fallback.h"

enum sql_plan_fallback_reason
sql_plan_fallback_from_logical(enum sql_logical_reject_reason reason)
{
	switch (reason) {
	case SQL_LOGICAL_REJECT_UNRESOLVED:
		return SQL_PLAN_FALLBACK_UNRESOLVED_INPUT;
	case SQL_LOGICAL_REJECT_RELATION_COUNT:
		return SQL_PLAN_FALLBACK_UNSUPPORTED_RELATION_COUNT;
	case SQL_LOGICAL_REJECT_SUBQUERY:
		return SQL_PLAN_FALLBACK_UNSUPPORTED_SUBQUERY;
	case SQL_LOGICAL_REJECT_AGGREGATE:
		return SQL_PLAN_FALLBACK_UNSUPPORTED_AGGREGATE;
	case SQL_LOGICAL_REJECT_COMPOUND:
		return SQL_PLAN_FALLBACK_UNSUPPORTED_COMPOUND;
	case SQL_LOGICAL_REJECT_CTE:
		return SQL_PLAN_FALLBACK_UNSUPPORTED_CTE;
	case SQL_LOGICAL_REJECT_DISTINCT:
		return SQL_PLAN_FALLBACK_UNSUPPORTED_DISTINCT;
	case SQL_LOGICAL_REJECT_NONE:
	default:
		return SQL_PLAN_FALLBACK_NONE;
	}
}

enum sql_plan_fallback_reason
sql_plan_fallback_from_physical(enum sql_physical_reject_reason reason)
{
	switch (reason) {
	case SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN:
		return SQL_PLAN_FALLBACK_INVALID_LOGICAL_PLAN;
	case SQL_PHYSICAL_REJECT_NO_ACCESS_PATH:
		return SQL_PLAN_FALLBACK_NO_ACCESS_PATH;
	case SQL_PHYSICAL_REJECT_INVALID_CANDIDATE:
		return SQL_PLAN_FALLBACK_INVALID_CANDIDATE;
	case SQL_PHYSICAL_REJECT_NONE:
	default:
		return SQL_PLAN_FALLBACK_NONE;
	}
}

bool
sql_plan_producer_result_init(struct sql_plan_producer_result *result,
			      const struct sql_plan_descriptor *descriptor,
			      enum sql_logical_reject_reason logical_reason,
			      enum sql_physical_reject_reason physical_reason)
{
	if (result == NULL)
		return false;
	if (descriptor != NULL) {
		const struct sql_plan_descriptor_input *input =
			sql_plan_descriptor_get_input(descriptor);
		if (input == NULL || input->path_class != SQL_PLAN_NEW_PLANNER)
			return false;
		*result = (struct sql_plan_producer_result){
			.path_class = SQL_PLAN_NEW_PLANNER,
			.fallback_reason = SQL_PLAN_FALLBACK_NONE,
			.descriptor = descriptor,
		};
		return true;
	}
	enum sql_plan_fallback_reason reason =
		sql_plan_fallback_from_logical(logical_reason);
	if (reason == SQL_PLAN_FALLBACK_NONE)
		reason = sql_plan_fallback_from_physical(physical_reason);
	if (reason == SQL_PLAN_FALLBACK_NONE)
		return false;
	*result = (struct sql_plan_producer_result){
		.path_class = SQL_PLAN_FALLBACK,
		.fallback_reason = reason,
		.descriptor = NULL,
	};
	return true;
}
