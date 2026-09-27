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

static struct sql_plan_descriptor *
new_point_descriptor(int64_t key)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	static const struct sql_plan_expression expression = {
		.id = 1,
		.canonical = "integer-point-key",
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_PK_POINT_LOOKUP,
			.bounds = &bound,
			.bound_count = 1,
			.has_integer_point_key = true,
			.integer_point_key = key,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = &expression,
		.expression_count = 1,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_unsigned_point_descriptor(uint64_t key)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	static const struct sql_plan_expression expression = {
		.id = 1,
		.canonical = "unsigned-point-key",
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_PK_POINT_LOOKUP,
			.bounds = &bound,
			.bound_count = 1,
			.has_unsigned_point_key = true,
			.unsigned_point_key = key,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = &expression,
		.expression_count = 1,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_invalid_point_descriptor(void)
{
	static const uint32_t columns[] = {2};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_TABLE_FULL_SCAN,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_point_limit_descriptor(int64_t key, uint64_t limit, uint64_t offset)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	static const struct sql_plan_expression expression = {
		.id = 1,
		.canonical = "integer-point-key",
	};
	struct sql_plan_finalize finalize = {
		.kind = SQL_PLAN_LIMIT,
		.limit = limit,
		.offset = offset,
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_PK_POINT_LOOKUP,
			.bounds = &bound,
			.bound_count = 1,
			.has_integer_point_key = true,
			.integer_point_key = key,
		},
		.finalize = &finalize,
		.finalize_count = 1,
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = &expression,
		.expression_count = 1,
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
	plan(22);
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
	static const struct sql_plan_finalize wide_offset_limit = {
		.kind = SQL_PLAN_LIMIT,
		.limit = INT64_MAX,
		.offset = INT64_MAX - 1,
	};
	static const struct sql_plan_finalize invalid_offset = {
		.kind = SQL_PLAN_LIMIT, .limit = 1,
		.offset = UINT64_MAX,
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
	struct sql_plan_descriptor *wide_offset_limit_desc =
		new_scan_descriptor(NULL, 0, SQL_PLAN_ASC, &wide_offset_limit, 1);
	struct sql_plan_descriptor *invalid_offset_desc =
		new_scan_descriptor(NULL, 0, SQL_PLAN_ASC, &invalid_offset, 1);
	struct sql_plan_descriptor *point_desc = new_point_descriptor(INT64_MAX);
	struct sql_plan_descriptor *negative_point_desc =
		new_point_descriptor(INT64_MIN);
	struct sql_plan_descriptor *unsigned_point_desc =
		new_unsigned_point_descriptor(UINT64_MAX);
	struct sql_plan_descriptor *invalid_point_desc =
		new_invalid_point_descriptor();
	struct sql_plan_descriptor *point_limit_desc =
		new_point_limit_descriptor(1, 1, 0);
	struct sql_plan_descriptor *point_zero_limit_desc =
		new_point_limit_descriptor(1, 0, 0);
	struct sql_plan_descriptor *point_offset_desc =
		new_point_limit_descriptor(1, 1, 1);
	ok(plan_desc != NULL && filtered_desc != NULL && descending_desc != NULL &&
	   limit_one_desc != NULL && limit_zero_desc != NULL &&
	   offset_limit_desc != NULL && wide_offset_limit_desc != NULL &&
	   invalid_offset_desc != NULL && point_desc != NULL &&
	   negative_point_desc != NULL && unsigned_point_desc != NULL &&
	   invalid_point_desc != NULL &&
	   point_limit_desc != NULL && point_zero_limit_desc != NULL &&
	   point_offset_desc != NULL,
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
	int saved_n_mem = parse.nMem;
	parse.nMem = INT_MAX - 1;
	int before_register_overflow = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(wide_offset_limit_desc, &vdbe, 4, 20) == -1 &&
	   vdbe.nOp == before_register_overflow && parse.nMem == INT_MAX - 1,
	   "LIMIT/OFFSET register overflow is rejected before VDBE mutation");
	parse.nMem = INT_MAX;
	before_register_overflow = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_desc, &vdbe, 4, 20) == -1 &&
	   vdbe.nOp == before_register_overflow && parse.nMem == INT_MAX,
	   "point-key register overflow rolls back without changing VDBE state");
	parse.nMem = saved_n_mem;
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
	int before_wide_offset_limit = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(wide_offset_limit_desc, &vdbe, 4,
					  20) == 0 &&
	   vdbe.aOp[before_wide_offset_limit].opcode == OP_Int64 &&
	   vdbe.aOp[before_wide_offset_limit].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_wide_offset_limit].p4.pI64 ==
		INT64_MAX &&
	   vdbe.aOp[before_wide_offset_limit + 1].opcode == OP_Int64 &&
	   vdbe.aOp[before_wide_offset_limit + 1].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_wide_offset_limit + 1].p4.pI64 ==
		INT64_MAX - 1,
	   "maximum signed-64-bit LIMIT and OFFSET initialize unsigned counters");
	int before_invalid_offset = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(invalid_offset_desc, &vdbe, 4, 20) == -1 &&
	   vdbe.nOp == before_invalid_offset,
	   "out-of-range offset descriptor is rejected before VDBE mutation");
	int before_invalid_point = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(invalid_point_desc, &vdbe, 4, 20) == -1 &&
	   vdbe.nOp == before_invalid_point,
	   "non-point descriptor is rejected before VDBE mutation by the point lowerer");
	int before_point = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_point].opcode == OP_Int64 &&
	   vdbe.aOp[before_point].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_point].p4.pI64 == INT64_MAX &&
	   vdbe.aOp[before_point + 1].opcode == OP_NotFound &&
	   vdbe.aOp[before_point + 1].p1 == 4 &&
	   vdbe.aOp[before_point + 2].opcode == OP_Column &&
	   vdbe.aOp[before_point + 4].opcode == OP_ResultRow &&
	   vdbe.aOp[before_point + 1].p2 == before_point + 5,
	   "integer primary-key point path seeks, projects, and returns at most one row");
	int before_negative_point = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(negative_point_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_negative_point].opcode == OP_Int64 &&
	   vdbe.aOp[before_negative_point].p4type == P4_INT64 &&
	   *vdbe.aOp[before_negative_point].p4.pI64 == INT64_MIN,
	   "wide negative primary-key values retain signed VDBE representation");
	int before_unsigned_point = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(unsigned_point_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_unsigned_point].opcode == OP_Int64 &&
	   vdbe.aOp[before_unsigned_point].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_unsigned_point].p4.pI64 == UINT64_MAX,
	   "unsigned primary-key lookup preserves full uint64 key range");
	int before_point_limit = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_limit_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.nOp == before_point_limit + 5 &&
	   vdbe.aOp[before_point_limit + 1].opcode == OP_NotFound,
	   "positive LIMIT retains the primary-key point seek");
	int before_point_zero_limit = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_zero_limit_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.nOp == before_point_zero_limit + 1 &&
	   vdbe.aOp[before_point_zero_limit].opcode == OP_Goto,
	   "LIMIT 0 suppresses the point seek and result emission");
	int before_point_offset = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_offset_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.nOp == before_point_offset + 1 &&
	   vdbe.aOp[before_point_offset].opcode == OP_Goto,
	   "positive OFFSET suppresses the single matching row");

	sql_plan_descriptor_delete(plan_desc);
	sql_plan_descriptor_delete(filtered_desc);
	sql_plan_descriptor_delete(descending_desc);
	sql_plan_descriptor_delete(limit_one_desc);
	sql_plan_descriptor_delete(limit_zero_desc);
	sql_plan_descriptor_delete(offset_limit_desc);
	sql_plan_descriptor_delete(wide_offset_limit_desc);
	sql_plan_descriptor_delete(invalid_offset_desc);
	sql_plan_descriptor_delete(point_desc);
	sql_plan_descriptor_delete(negative_point_desc);
	sql_plan_descriptor_delete(unsigned_point_desc);
	sql_plan_descriptor_delete(invalid_point_desc);
	sql_plan_descriptor_delete(point_limit_desc);
	sql_plan_descriptor_delete(point_zero_limit_desc);
	sql_plan_descriptor_delete(point_offset_desc);
	for (int i = 0; i < vdbe.nOp; ++i) {
		if (vdbe.aOp[i].p4type == P4_INT64 ||
		    vdbe.aOp[i].p4type == P4_UINT64)
			sql_xfree(vdbe.aOp[i].p4.p);
	}
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
