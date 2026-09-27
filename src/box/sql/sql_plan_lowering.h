#ifndef TARANTOOL_SQL_PLAN_LOWERING_H
#define TARANTOOL_SQL_PLAN_LOWERING_H

#include <stddef.h>

#include "sql_plan_descriptor.h"

struct Vdbe;

/*
 * M3.4 boundary prototype. This is an ordered lowering contract, not a VDBE
 * program: expression lowering, cursor allocation, engine-specific seeks,
 * sorter setup, and SQL result delivery remain the backend's responsibility.
 * All descriptor pointers passed to callbacks are borrowed for the call.
 */
enum sql_plan_lowering_stage {
	SQL_PLAN_LOWER_SCAN,
	SQL_PLAN_LOWER_FILTER,
	SQL_PLAN_LOWER_PROJECT,
	SQL_PLAN_LOWER_SORT,
	SQL_PLAN_LOWER_LIMIT,
	SQL_PLAN_LOWER_RESULT,
};

struct sql_plan_lowering_event {
	enum sql_plan_lowering_stage stage;
	const struct sql_plan_descriptor_input *plan;
	const struct sql_plan_filter *filter;
	const struct sql_plan_finalize *finalize;
};

/* Return 0 to continue, nonzero to stop and propagate the callback result. */
typedef int (*sql_plan_lowering_emit_f)(
	void *context, const struct sql_plan_lowering_event *event);

/* Emits scan, filters, projection, finalizers, then result in that order. */
int
sql_plan_lower(const struct sql_plan_descriptor *plan,
	       sql_plan_lowering_emit_f emit, void *context);

/*
 * Emit an executable table-full-scan loop for a descriptor with direct
 * projection columns and no filters/finalizers. The caller owns cursor
 * opening, result metadata, and result-register allocation. Unsupported
 * descriptors are rejected before any VDBE state is changed.
 */
int
sql_plan_lower_vdbe_table_scan(const struct sql_plan_descriptor *plan,
			       struct Vdbe *vdbe, int cursor,
			       int result_first_reg);

#endif /* TARANTOOL_SQL_PLAN_LOWERING_H */
