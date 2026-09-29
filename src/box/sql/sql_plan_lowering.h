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

/* Emit expression bytecode into the assigned result register. */
typedef int (*sql_plan_projection_projector_f)(void *ctx, uint32_t expr_ref,
						       int result_reg);

struct sql_plan_secondary_index {
	uint32_t index_id;
	uint32_t key_column;
	bool key_unsigned;
	const uint32_t *primary_key_columns;
	size_t primary_key_count;
};

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
int
sql_plan_lower_vdbe_table_scan_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe, int cursor,
	int result_first_reg, sql_plan_projection_projector_f projector,
	void *projector_ctx);

int
sql_plan_lower_vdbe_pk_range(const struct sql_plan_descriptor *plan,
			     struct Vdbe *vdbe, int cursor,
			     int result_first_reg);
int
sql_plan_lower_vdbe_pk_range_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe, int cursor,
	int result_first_reg, sql_plan_projection_projector_f projector,
	void *projector_ctx);

int
sql_plan_lower_vdbe_pk_point(const struct sql_plan_descriptor *plan,
			     struct Vdbe *vdbe, int cursor,
			     int result_first_reg);
int
sql_plan_lower_vdbe_pk_point_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe, int cursor,
	int result_first_reg, sql_plan_projection_projector_f projector,
	void *projector_ctx);

int
sql_plan_lower_vdbe_pk_prefix_scan(const struct sql_plan_descriptor *plan,
				   struct Vdbe *vdbe, int cursor,
				   int result_first_reg);
int
sql_plan_lower_vdbe_pk_prefix_scan_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe, int cursor,
	int result_first_reg, sql_plan_projection_projector_f projector,
	void *projector_ctx);

int
sql_plan_lower_vdbe_secondary_equality_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe,
	int table_cursor, int index_cursor,
	const struct sql_plan_secondary_index *index,
	int result_first_reg, sql_plan_projection_projector_f projector,
	void *projector_ctx);

#endif /* TARANTOOL_SQL_PLAN_LOWERING_H */
