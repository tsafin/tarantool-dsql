#include <limits.h>

#include "unit.h"

#include "box/box.h"
#include "box/sql.h"
#include "box/sql/sqlInt.h"
#include "box/sql/sql_plan_descriptor.h"
#include "box/sql/sql_plan_lowering.h"
#include "opcodes.h"
#include "box/sql/vdbe.h"
#include "box/sql/vdbeInt.h"
#include "coll/coll.h"
#include "core/diag.h"
#include "core/event.h"
#include "core/fiber.h"
#include "core/memory.h"
#include "main.h"

char tarantool_path[PATH_MAX];
long tarantool_start_time;

sigint_cb_t
set_sigint_cb(sigint_cb_t new_sigint_cb)
{
	static sigint_cb_t sigint_cb;
	sigint_cb = new_sigint_cb;
	return NULL;
}

static struct sql_plan_descriptor *
new_scan_descriptor(const struct sql_plan_filter *filters, size_t filter_count,
		    enum sql_plan_direction direction,
		    const struct sql_plan_finalize *finalize,
		    size_t finalize_count)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "col(c0) > 7"},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_TABLE_FULL_SCAN,
			.direction = direction,
			.est_rows = 10,
			.est_rows_confidence = 1,
		},
		.filters = filters,
		.filter_count = filter_count,
		.finalize = finalize,
		.finalize_count = finalize_count,
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = filter_count == 0 ? 0 : 1,
		.cost_total = 10,
		.cost_rows = 10,
		.cost_row_width = 8,
		.cost_confidence = 1,
	};
	return sql_plan_descriptor_new(&input);
}

int
main(void)
{
	memory_init();
	fiber_init(fiber_c_invoke);
	coll_init();
	event_init();
	box_init();
	sql_init();
	plan(12);
	header();
	static const struct sql_plan_filter filter = {
		.expr_ref = 1, .selectivity = 0.5, .confidence = 1,
	};
	static const struct sql_plan_finalize limit_one = {
		.kind = SQL_PLAN_LIMIT, .limit = 1,
	};
	static const struct sql_plan_finalize limit_zero = {
		.kind = SQL_PLAN_LIMIT, .limit = 0,
	};
	static const struct sql_plan_finalize offset_limit = {
		.kind = SQL_PLAN_LIMIT, .limit = 1, .offset = 1,
	};
	static const struct sql_plan_finalize invalid_offset = {
		.kind = SQL_PLAN_LIMIT, .limit = 1,
		.offset = (uint64_t)INT_MAX + 1,
	};
	struct sql_plan_descriptor *plan_desc = new_scan_descriptor(NULL, 0,
		SQL_PLAN_ASC, NULL, 0);
	struct sql_plan_descriptor *filtered_desc =
		new_scan_descriptor(&filter, 1, SQL_PLAN_ASC, NULL, 0);
	struct sql_plan_descriptor *descending_desc =
		new_scan_descriptor(NULL, 0, SQL_PLAN_DESC, NULL, 0);
	struct sql_plan_descriptor *limit_one_desc =
		new_scan_descriptor(NULL, 0, SQL_PLAN_ASC, &limit_one, 1);
	struct sql_plan_descriptor *limit_zero_desc =
		new_scan_descriptor(NULL, 0, SQL_PLAN_ASC, &limit_zero, 1);
	struct sql_plan_descriptor *offset_limit_desc =
		new_scan_descriptor(NULL, 0, SQL_PLAN_ASC, &offset_limit, 1);
	struct sql_plan_descriptor *invalid_offset_desc =
		new_scan_descriptor(NULL, 0, SQL_PLAN_ASC, &invalid_offset, 1);
	ok(plan_desc != NULL && filtered_desc != NULL && descending_desc != NULL &&
	   limit_one_desc != NULL && limit_zero_desc != NULL &&
	   offset_limit_desc != NULL && invalid_offset_desc != NULL,
	   "scan and literal-limit descriptors are constructed");
	struct Parse parse = {};
	struct Vdbe vdbe = {};
	vdbe.magic = VDBE_MAGIC_INIT;
	vdbe.pParse = &parse;
	parse.pVdbe = &vdbe;
	int old_op = sqlVdbeAddOp0(&vdbe, OP_Noop);
	assert(old_op == 0);
	int op_count = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(filtered_desc, &vdbe, 4, 20) == -1 &&
	   vdbe.nOp == op_count,
	   "unsupported filter descriptor is rejected before VDBE mutation");
	ok(sql_plan_lower_vdbe_table_scan(plan_desc, &vdbe, 4, INT_MAX) == -1 &&
	   vdbe.nOp == op_count,
	   "register overflow is rejected before VDBE mutation");
	ok(sql_plan_lower_vdbe_table_scan(plan_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.nOp == op_count + 5,
	   "table scan emits a complete VDBE loop");
	const struct VdbeOp *ops = vdbe.aOp;
	ok(ops[1].opcode == OP_Rewind && ops[1].p1 == 4 &&
	   ops[1].p2 == 6 && ops[2].opcode == OP_Column &&
	   ops[2].p1 == 4 && ops[2].p2 == 2 && ops[2].p3 == 20 &&
	   ops[3].opcode == OP_Column && ops[3].p2 == 0 && ops[3].p3 == 21,
	   "scan jump and ordered column-to-register bindings are correct");
	ok(ops[4].opcode == OP_ResultRow && ops[4].p1 == 20 &&
	   ops[4].p2 == 2 && ops[5].opcode == OP_Next &&
	   ops[5].p1 == 4 && ops[5].p2 == 2,
	   "result delivery and next-row branch complete the loop");
	int before_bad_call = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(plan_desc, &vdbe, -1, 20) == -1 &&
	   vdbe.nOp == before_bad_call,
	   "invalid cursor is rejected without emitting opcodes");
	int before_descending = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(descending_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_descending].opcode == OP_Last &&
	   vdbe.aOp[before_descending + 4].opcode == OP_Prev,
	   "descending scan uses last/previous cursor operations");
	int before_limit = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(limit_one_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_limit].opcode == OP_Integer &&
	   vdbe.aOp[before_limit].p1 == 1 &&
	   vdbe.aOp[before_limit + 5].opcode == OP_DecrJumpZero &&
	   vdbe.aOp[before_limit + 5].p2 == before_limit + 7 &&
	   vdbe.aOp[before_limit + 6].opcode == OP_Next,
	   "literal LIMIT decrements after result and exits after its final row");
	int before_zero_limit = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(limit_zero_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_zero_limit].opcode == OP_Goto &&
	   vdbe.aOp[before_zero_limit].p2 == before_zero_limit + 1,
	   "LIMIT 0 skips scan emission and reaches cursor close");
	int before_offset_limit = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(offset_limit_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_offset_limit].opcode == OP_Integer &&
	   vdbe.aOp[before_offset_limit + 1].opcode == OP_Integer &&
	   vdbe.aOp[before_offset_limit + 3].opcode == OP_IfNotZero &&
	   vdbe.aOp[before_offset_limit + 3].p2 == before_offset_limit + 8 &&
	   vdbe.aOp[before_offset_limit + 7].opcode == OP_DecrJumpZero &&
	   vdbe.aOp[before_offset_limit + 8].opcode == OP_Next,
	   "literal OFFSET skips rows before projection and limit accounting");
	int before_invalid_offset = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(invalid_offset_desc, &vdbe, 4, 20) == -1 &&
	   vdbe.nOp == before_invalid_offset,
	   "out-of-range offset descriptor is rejected before VDBE mutation");

	sql_plan_descriptor_delete(plan_desc);
	sql_plan_descriptor_delete(filtered_desc);
	sql_plan_descriptor_delete(descending_desc);
	sql_plan_descriptor_delete(limit_one_desc);
	sql_plan_descriptor_delete(limit_zero_desc);
	sql_plan_descriptor_delete(offset_limit_desc);
	sql_plan_descriptor_delete(invalid_offset_desc);
	sql_xfree(vdbe.aOp);
	footer();
	int rc = check_plan();
	box_free();
	event_free();
	coll_free();
	fiber_free();
	memory_free();
	return rc;
}
