#ifndef TARANTOOL_SQL_PLAN_FALLBACK_H
#define TARANTOOL_SQL_PLAN_FALLBACK_H

#include <stdbool.h>

#include "sql_logical_plan.h"
#include "sql_physical_plan.h"

/*
 * Observable planner-producer outcome. A new-planner descriptor is borrowed
 * for the result lifetime. A fallback outcome has no descriptor but carries
 * stable path_class/reason metadata for the current planner and diagnostics.
 * This contract does not itself route a statement to where.c.
 */
struct sql_plan_producer_result {
	enum sql_plan_path_class path_class;
	enum sql_plan_fallback_reason fallback_reason;
	const struct sql_plan_descriptor *descriptor;
};

/* Translate current logical/physical reject enums to stable reason codes. */
enum sql_plan_fallback_reason sql_plan_fallback_from_logical(
	enum sql_logical_reject_reason reason);
enum sql_plan_fallback_reason sql_plan_fallback_from_physical(
	enum sql_physical_reject_reason reason);

/*
 * Initialize an observable producer result. A descriptor denotes success;
 * otherwise at least one reject reason is required. Logical rejects take
 * precedence so unsupported SQL shape is not obscured by missing candidates.
 */
bool sql_plan_producer_result_init(struct sql_plan_producer_result *result,
				   const struct sql_plan_descriptor *descriptor,
				   enum sql_logical_reject_reason logical_reason,
				   enum sql_physical_reject_reason physical_reason);

#endif
