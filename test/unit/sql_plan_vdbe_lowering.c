#include <limits.h>
#include <string.h>

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
new_point_null_filter_descriptor(enum sql_plan_filter_op op)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	struct sql_plan_filter filter = {
		.expr_ref = 2,
		.selectivity = 0.5,
		.column = 3,
		.op = op,
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "integer-point-key"},
		{.id = 2, .canonical = "direct-column-null-filter"},
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
			.integer_point_key = 42,
		},
		.filters = &filter,
		.filter_count = 1,
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = 2,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_composite_point_descriptor(void)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1},
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 2},
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 3},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "composite-key-part-0"},
		{.id = 2, .canonical = "composite-key-part-1"},
		{.id = 3, .canonical = "composite-key-part-2"},
	};
	static const struct sql_plan_point_key_part parts[] = {
		{.integer_value = -7},
		{.unsigned_value = UINT64_MAX, .is_unsigned = true},
		{.integer_value = INT64_MIN},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_PK_POINT_LOOKUP,
			.bounds = bounds,
			.bound_count = 3,
			.point_key_parts = parts,
			.point_key_part_count = 3,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = sizeof(expressions) / sizeof(expressions[0]),
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_composite_prefix_descriptor(bool with_limit, uint64_t limit,
				uint64_t offset)
{
	static const uint32_t columns[] = {2};
	static const struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1},
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 2},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "prefix-key-part-0"},
		{.id = 2, .canonical = "prefix-key-part-1"},
	};
	static const struct sql_plan_point_key_part parts[] = {
		{.integer_value = 1, .column = 0},
		{.unsigned_value = UINT64_MAX, .column = 1, .is_unsigned = true},
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
			.kind = SQL_PLAN_PK_PREFIX_SCAN,
			.bounds = bounds,
			.bound_count = 2,
			.prefix_key_parts = parts,
			.prefix_key_part_count = 2,
			.direction = SQL_PLAN_ASC,
		},
		.finalize = with_limit ? &finalize : NULL,
		.finalize_count = with_limit ? 1 : 0,
		.projection_columns = columns,
		.projection_column_count = 1,
		.expressions = expressions,
		.expression_count = 2,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_composite_prefix_range_descriptor(bool bounded)
{
	static const uint32_t columns[] = {2};
	static const struct sql_plan_point_key_part prefix[] = {
		{.integer_value = 1, .column = 0},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "prefix-key-part"},
		{.id = 2, .canonical = "unsigned-range-lower"},
		{.id = 3, .canonical = "unsigned-range-upper"},
	};
	struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1},
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_GT, .expr_ref = 2},
		{.side = SQL_PLAN_UPPER, .op = SQL_PLAN_LT, .expr_ref = 3},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_PK_PREFIX_SCAN,
			.bounds = bounds,
			.bound_count = bounded ? 3 : 2,
			.prefix_key_parts = prefix,
			.prefix_key_part_count = 1,
			.has_unsigned_range_key = true,
			.unsigned_range_key = 20,
			.integer_range_op = SQL_PLAN_GT,
			.has_unsigned_range_end_key = bounded,
			.unsigned_range_end_key = 40,
			.integer_range_end_op = SQL_PLAN_LT,
			.range_key_column = 1,
			.direction = SQL_PLAN_ASC,
		},
		.projection_columns = columns,
		.projection_column_count = 1,
		.expressions = expressions,
		.expression_count = bounded ? 3 : 2,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_range_descriptor(enum sql_plan_bound_op op, int64_t key,
		    bool unsigned_key, enum sql_plan_direction direction,
		    const struct sql_plan_filter *filter)
{
	static const uint32_t columns[] = {2, 0};
	struct sql_plan_bound bound = {
		.side = op == SQL_PLAN_LT || op == SQL_PLAN_LE ?
			SQL_PLAN_UPPER : SQL_PLAN_LOWER,
		.op = op,
		.expr_ref = 1,
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "integer-range-key"},
		{.id = 2, .canonical = "direct-column-null-filter"},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_RANGE_SCAN,
			.bounds = &bound,
			.bound_count = 1,
			.has_integer_range_key = !unsigned_key,
			.integer_range_key = key,
			.has_unsigned_range_key = unsigned_key,
			.unsigned_range_key = (uint64_t)key,
			.integer_range_op = op,
			.direction = direction,
		},
		.filters = filter,
		.filter_count = filter == NULL ? 0 : 1,
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = filter == NULL ? 1 : 2,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_bounded_range_descriptor(enum sql_plan_bound_op upper_op,
			     int64_t lower_key, int64_t upper_key,
			     const struct sql_plan_filter *filter)
{
	static const uint32_t columns[] = {0};
	struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_GE, .expr_ref = 1},
		{.side = SQL_PLAN_UPPER, .op = upper_op, .expr_ref = 2},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "integer-range-lower"},
		{.id = 2, .canonical = "integer-range-upper"},
		{.id = 3, .canonical = "direct-column-null-filter"},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_RANGE_SCAN,
			.bounds = bounds,
			.bound_count = 2,
			.has_integer_range_key = true,
			.integer_range_key = lower_key,
			.integer_range_op = SQL_PLAN_GE,
			.has_integer_range_end_key = true,
			.integer_range_end_key = upper_key,
			.integer_range_end_op = upper_op,
			.range_key_column = 0,
			.direction = SQL_PLAN_ASC,
		},
		.filters = filter,
		.filter_count = filter == NULL ? 0 : 1,
		.projection_columns = columns,
		.projection_column_count = 1,
		.expressions = expressions,
		.expression_count = filter == NULL ? 2 : 3,
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
new_late_invalid_point_descriptor(int64_t key)
{
	static const uint32_t columns[] = {UINT32_MAX};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	static const struct sql_plan_expression expression = {
		.id = 1,
		.canonical = "invalid-projection-column",
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
	plan(45);
	header();
	static const struct sql_plan_filter filter = {
		.expr_ref = 1, .selectivity = 0.5, .confidence = 1,
	};
	static const struct sql_plan_filter is_null_filter = {
		.expr_ref = 1, .selectivity = 0.5, .confidence = 1,
		.column = 1, .op = SQL_PLAN_FILTER_IS_NULL,
	};
	static const struct sql_plan_filter is_not_null_filter = {
		.expr_ref = 1, .selectivity = 0.5, .confidence = 1,
		.column = 1, .op = SQL_PLAN_FILTER_IS_NOT_NULL,
	};
	static const struct sql_plan_filter range_null_filter = {
		.expr_ref = 3, .selectivity = 0.5, .confidence = 0,
		.column = 1, .op = SQL_PLAN_FILTER_IS_NULL,
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
	struct sql_plan_descriptor *is_null_desc =
		new_scan_descriptor(&is_null_filter, 1, SQL_PLAN_ASC, NULL, 0);
	struct sql_plan_descriptor *is_not_null_desc =
		new_scan_descriptor(&is_not_null_filter, 1, SQL_PLAN_ASC, NULL, 0);
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
	struct sql_plan_descriptor *point_null_filter_desc =
		new_point_null_filter_descriptor(SQL_PLAN_FILTER_IS_NULL);
	struct sql_plan_descriptor *point_not_null_filter_desc =
		new_point_null_filter_descriptor(SQL_PLAN_FILTER_IS_NOT_NULL);
	struct sql_plan_descriptor *composite_point_desc =
		new_composite_point_descriptor();
	struct sql_plan_descriptor *composite_prefix_desc =
		new_composite_prefix_descriptor(false, 0, 0);
	struct sql_plan_descriptor *composite_prefix_limit_desc =
		new_composite_prefix_descriptor(true, 1, 0);
	struct sql_plan_descriptor *composite_prefix_offset_desc =
		new_composite_prefix_descriptor(true, 1, 1);
	struct sql_plan_descriptor *composite_prefix_zero_desc =
		new_composite_prefix_descriptor(true, 0, 0);
	struct sql_plan_descriptor *composite_prefix_range_desc =
		new_composite_prefix_range_descriptor(false);
	struct sql_plan_descriptor *composite_prefix_bounded_range_desc =
		new_composite_prefix_range_descriptor(true);
	struct sql_plan_descriptor *range_gt_desc =
		new_range_descriptor(SQL_PLAN_GT, INT64_MAX, false, SQL_PLAN_ASC,
				     NULL);
	struct sql_plan_descriptor *range_le_desc =
		new_range_descriptor(SQL_PLAN_LE, 7, false, SQL_PLAN_DESC, NULL);
	struct sql_plan_descriptor *range_ge_desc =
		new_range_descriptor(SQL_PLAN_GE, INT64_MIN, false, SQL_PLAN_ASC,
				     NULL);
	struct sql_plan_descriptor *range_lt_desc =
		new_range_descriptor(SQL_PLAN_LT, 7, false, SQL_PLAN_DESC, NULL);
	struct sql_plan_descriptor *unsigned_range_desc =
		new_range_descriptor(SQL_PLAN_GT, (int64_t)UINT64_MAX, true,
				     SQL_PLAN_ASC, NULL);
	struct sql_plan_descriptor *invalid_direction_range_desc =
		new_range_descriptor(SQL_PLAN_GT, 7, false, SQL_PLAN_DESC, NULL);
	struct sql_plan_descriptor *bounded_range_desc =
		new_bounded_range_descriptor(SQL_PLAN_LT, 1, 3, NULL);
	struct sql_plan_descriptor *wide_bounded_range_desc =
		new_bounded_range_descriptor(SQL_PLAN_LE, INT64_MIN, INT64_MAX,
					     NULL);
	struct sql_plan_descriptor *invalid_bounded_range_desc =
		new_bounded_range_descriptor(SQL_PLAN_EQ, 1, 3, NULL);
	struct sql_plan_descriptor *filtered_bounded_range_desc =
		new_bounded_range_descriptor(SQL_PLAN_LT, 1, 3,
					     &range_null_filter);
	struct sql_plan_descriptor *invalid_point_desc =
		new_invalid_point_descriptor();
	struct sql_plan_descriptor *late_invalid_point_desc =
		new_late_invalid_point_descriptor(INT64_MAX);
	struct sql_plan_descriptor *point_limit_desc =
		new_point_limit_descriptor(1, 1, 0);
	struct sql_plan_descriptor *point_zero_limit_desc =
		new_point_limit_descriptor(1, 0, 0);
	struct sql_plan_descriptor *point_offset_desc =
		new_point_limit_descriptor(1, 1, 1);
	ok(plan_desc != NULL && filtered_desc != NULL && is_null_desc != NULL &&
	   is_not_null_desc != NULL && descending_desc != NULL &&
	   limit_one_desc != NULL && limit_zero_desc != NULL &&
	   offset_limit_desc != NULL && wide_offset_limit_desc != NULL &&
	   invalid_offset_desc != NULL && point_desc != NULL &&
	   negative_point_desc != NULL && unsigned_point_desc != NULL &&
	   point_null_filter_desc != NULL &&
	   point_not_null_filter_desc != NULL &&
	   composite_point_desc != NULL &&
	   composite_prefix_desc != NULL &&
	   composite_prefix_limit_desc != NULL &&
	   composite_prefix_offset_desc != NULL &&
	   composite_prefix_zero_desc != NULL &&
	   range_gt_desc != NULL && range_le_desc != NULL &&
	   unsigned_range_desc != NULL && bounded_range_desc != NULL &&
	   wide_bounded_range_desc != NULL && filtered_bounded_range_desc != NULL &&
	   invalid_direction_range_desc != NULL &&
	   invalid_point_desc != NULL && late_invalid_point_desc != NULL &&
	   point_limit_desc != NULL && point_zero_limit_desc != NULL &&
	   point_offset_desc != NULL,
	   "scan and literal-limit descriptors are constructed");
	ok(invalid_bounded_range_desc == NULL,
	   "bounded range rejects an invalid upper-bound operator");
	struct Parse parse = {};
	struct Vdbe vdbe = {};
	vdbe.magic = VDBE_MAGIC_INIT;
	vdbe.pParse = &parse;
	parse.pVdbe = &vdbe;
	int old_op = sqlVdbeAddOp0(&vdbe, OP_Noop);
	assert(old_op == 0);
	int op_count = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(invalid_direction_range_desc, &vdbe, 4,
					20) == -1 && vdbe.nOp == op_count,
	   "one-sided lower range rejects descending scan before VDBE mutation");
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
	int before_is_null = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(is_null_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_is_null + 1].opcode == OP_Column &&
	   vdbe.aOp[before_is_null + 1].p2 == 1 &&
	   vdbe.aOp[before_is_null + 2].opcode == OP_NotNull &&
	   vdbe.aOp[before_is_null + 2].p2 == before_is_null + 6 &&
	   vdbe.aOp[before_is_null + 6].opcode == OP_Next,
	   "IS NULL filter skips rejected rows to the cursor next opcode");
	int before_is_not_null = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan(is_not_null_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_is_not_null + 2].opcode == OP_IsNull &&
	   vdbe.aOp[before_is_not_null + 2].p2 == before_is_not_null + 6 &&
	   vdbe.aOp[before_is_not_null + 6].opcode == OP_Next,
	   "IS NOT NULL filter skips rejected rows to the cursor next opcode");
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
	int saved_point_n_mem = parse.nMem;
	int before_late_invalid_point = vdbe.nOp;
	int saved_n_tab = parse.nTab;
	int saved_n_label = parse.nLabel;
	int saved_n_range_reg = parse.nRangeReg;
	int saved_i_range_reg = parse.iRangeReg;
	int saved_n_temp_reg = parse.nTempReg;
	int saved_n_col_cache = parse.nColCache;
	int saved_i_cache_level = parse.iCacheLevel;
	int saved_i_cache_count = parse.iCacheCnt;
	int saved_i_self_tab = parse.iSelfTab;
	int saved_vdbe_field_ref_reg = parse.vdbe_field_ref_reg;
	int saved_col_names_set = parse.colNamesSet;
	bool saved_parse_is_aborted = parse.is_aborted;
	u32 saved_n_query_loop = parse.nQueryLoop;
	unsigned char saved_col_cache[sizeof(parse.aColCache)];
	memcpy(saved_col_cache, parse.aColCache, sizeof(saved_col_cache));
	struct VdbeOp saved_last_op = vdbe.aOp[before_late_invalid_point - 1];
	ok(sql_plan_lower_vdbe_pk_point(late_invalid_point_desc, &vdbe, 4, 20) == -1 &&
	   vdbe.nOp == before_late_invalid_point &&
	   memcmp(&vdbe.aOp[before_late_invalid_point - 1], &saved_last_op,
		  sizeof(saved_last_op)) == 0 &&
	   parse.nMem == saved_point_n_mem && parse.nTab == saved_n_tab &&
	   parse.nLabel == saved_n_label &&
	   parse.nRangeReg == saved_n_range_reg &&
	   parse.iRangeReg == saved_i_range_reg &&
	   parse.nTempReg == saved_n_temp_reg &&
	   parse.nColCache == saved_n_col_cache &&
	   parse.iCacheLevel == saved_i_cache_level &&
	   parse.iCacheCnt == saved_i_cache_count &&
	   parse.iSelfTab == saved_i_self_tab &&
	   parse.vdbe_field_ref_reg == saved_vdbe_field_ref_reg &&
	   parse.colNamesSet == saved_col_names_set &&
	   parse.is_aborted == saved_parse_is_aborted &&
	   parse.nQueryLoop == saved_n_query_loop &&
	   memcmp(parse.aColCache, saved_col_cache, sizeof(saved_col_cache)) == 0 &&
	   vdbe.aOp[before_late_invalid_point].opcode == 0 &&
	   vdbe.aOp[before_late_invalid_point].p4type == P4_NOTUSED &&
	   vdbe.aOp[before_late_invalid_point].p4.p == NULL &&
	   vdbe.aOp[before_late_invalid_point + 1].opcode == 0 &&
	   vdbe.aOp[before_late_invalid_point + 1].p4type == P4_NOTUSED &&
	   vdbe.aOp[before_late_invalid_point + 1].p4.p == NULL,
	   "post-emission reject restores checkpoint counters/register cache and clears key P4/opcode suffix");
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
	int before_point_filter = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_null_filter_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_point_filter + 1].opcode == OP_NotFound &&
	   vdbe.aOp[before_point_filter + 2].opcode == OP_Column &&
	   vdbe.aOp[before_point_filter + 2].p2 == 3 &&
	   vdbe.aOp[before_point_filter + 3].opcode == OP_NotNull &&
	   vdbe.aOp[before_point_filter + 4].opcode == OP_Column &&
	   vdbe.aOp[before_point_filter + 6].opcode == OP_ResultRow &&
	   vdbe.aOp[before_point_filter + 1].p2 == before_point_filter + 7 &&
	   vdbe.aOp[before_point_filter + 3].p2 == before_point_filter + 7,
	   "point lookup filters the matched row before projection");
	int before_point_not_null_filter = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_not_null_filter_desc, &vdbe, 4,
					       20) == 0 &&
	   vdbe.aOp[before_point_not_null_filter + 2].opcode == OP_Column &&
	   vdbe.aOp[before_point_not_null_filter + 2].p2 == 3 &&
	   vdbe.aOp[before_point_not_null_filter + 3].opcode == OP_IsNull &&
	   vdbe.aOp[before_point_not_null_filter + 3].p2 ==
		before_point_not_null_filter + 7,
	   "point lookup rejects NULL residuals before projection");
	int before_composite_point = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(composite_point_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_composite_point].opcode == OP_Integer &&
	   vdbe.aOp[before_composite_point].p1 == -7 &&
	   vdbe.aOp[before_composite_point + 1].opcode == OP_Int64 &&
	   vdbe.aOp[before_composite_point + 1].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_composite_point + 1].p4.pI64 == UINT64_MAX &&
	   vdbe.aOp[before_composite_point + 2].opcode == OP_Int64 &&
	   vdbe.aOp[before_composite_point + 2].p4type == P4_INT64 &&
	   *vdbe.aOp[before_composite_point + 2].p4.pI64 == INT64_MIN &&
	   vdbe.aOp[before_composite_point + 3].opcode == OP_NotFound &&
	   vdbe.aOp[before_composite_point + 3].p4type == P4_INT32 &&
	   vdbe.aOp[before_composite_point + 3].p4.i == 3,
	   "composite point lookup emits ordered registers and a three-part seek");
	int before_prefix_scan = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(composite_prefix_desc, &vdbe,
						      4, 20) == 0 &&
	   vdbe.aOp[before_prefix_scan].opcode == OP_Integer &&
	   vdbe.aOp[before_prefix_scan + 1].opcode == OP_Int64 &&
	   vdbe.aOp[before_prefix_scan + 1].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_prefix_scan + 1].p4.pI64 == UINT64_MAX &&
	   vdbe.aOp[before_prefix_scan + 2].opcode == OP_SeekGE &&
	   vdbe.aOp[before_prefix_scan + 2].p4type == P4_INT32 &&
	   vdbe.aOp[before_prefix_scan + 2].p4.i == 2 &&
	   vdbe.aOp[before_prefix_scan + 3].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_scan + 4].opcode == OP_Ne &&
	   vdbe.aOp[before_prefix_scan + 5].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_scan + 6].opcode == OP_Ne &&
	   vdbe.aOp[before_prefix_scan + 7].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_scan + 8].opcode == OP_ResultRow &&
	   vdbe.aOp[before_prefix_scan + 9].opcode == OP_Next,
	   "composite prefix scan seeks by two fields and stops on prefix mismatch");
	int before_prefix_limit = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(composite_prefix_limit_desc, &vdbe,
						      4, 20) == 0 &&
	   vdbe.aOp[before_prefix_limit + 2].opcode == OP_Integer &&
	   vdbe.aOp[before_prefix_limit + 3].opcode == OP_SeekGE &&
	   vdbe.aOp[before_prefix_limit + 10].opcode == OP_DecrJumpZero,
	   "composite prefix scan applies LIMIT after each emitted row");
	int before_prefix_offset = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(composite_prefix_offset_desc, &vdbe,
						      4, 20) == 0 &&
	   vdbe.aOp[before_prefix_offset + 2].opcode == OP_Integer &&
	   vdbe.aOp[before_prefix_offset + 3].opcode == OP_Integer &&
	   vdbe.aOp[before_prefix_offset + 4].opcode == OP_SeekGE &&
	   vdbe.aOp[before_prefix_offset + 9].opcode == OP_IfNotZero,
	   "composite prefix scan skips offset rows before projection");
	int before_prefix_zero = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(composite_prefix_zero_desc, &vdbe,
						      4, 20) == 0 &&
	   vdbe.aOp[before_prefix_zero].opcode == OP_Goto &&
	   vdbe.nOp == before_prefix_zero + 1,
	   "zero LIMIT suppresses composite prefix key setup and seek");
	int before_prefix_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(composite_prefix_range_desc,
						      &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_prefix_range].opcode == OP_Integer &&
	   vdbe.aOp[before_prefix_range].p1 == 1 &&
	   vdbe.aOp[before_prefix_range + 1].opcode == OP_Integer &&
	   vdbe.aOp[before_prefix_range + 1].p1 == 20 &&
	   vdbe.aOp[before_prefix_range + 2].opcode == OP_SeekGT &&
	   vdbe.aOp[before_prefix_range + 2].p4.i == 2,
	   "prefix range seeks with equality prefix plus strict unsigned lower bound");
	int before_prefix_bounded_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(
		   composite_prefix_bounded_range_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_prefix_bounded_range + 3].opcode == OP_SeekGT &&
	   vdbe.aOp[before_prefix_bounded_range + 6].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_bounded_range + 7].opcode == OP_Le &&
	   vdbe.aOp[before_prefix_bounded_range + 10].opcode == OP_Next,
	   "bounded prefix range stops at its exclusive unsigned upper endpoint");
	int before_range_gt = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_gt_desc, &vdbe, 4, 20) == 0,
	   "strict lower range descriptor lowers successfully");
	ok(vdbe.aOp[before_range_gt].opcode == OP_Int64 &&
	   vdbe.aOp[before_range_gt].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_range_gt].p4.pI64 == INT64_MAX &&
	   vdbe.aOp[before_range_gt + 1].opcode == OP_SeekGT &&
	   vdbe.aOp[before_range_gt + 1].p2 == before_range_gt + 6 &&
	   vdbe.aOp[before_range_gt + 5].opcode == OP_Next,
	   "strict lower range emits SeekGT and scans ascending");
	int before_range_le = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_le_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_range_le].opcode == OP_Integer &&
	   vdbe.aOp[before_range_le + 1].opcode == OP_SeekLE &&
	   vdbe.aOp[before_range_le + 5].opcode == OP_Prev,
	   "inclusive upper range emits SeekLE and scans descending");
	int before_range_ge = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_ge_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_range_ge + 1].opcode == OP_SeekGE &&
	   vdbe.aOp[before_range_ge + 1].p2 == before_range_ge + 6 &&
	   vdbe.aOp[before_range_ge + 5].opcode == OP_Next,
	   "inclusive lower range emits SeekGE and scans ascending");
	int before_range_lt = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_lt_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_range_lt + 1].opcode == OP_SeekLT &&
	   vdbe.aOp[before_range_lt + 1].p2 == before_range_lt + 6 &&
	   vdbe.aOp[before_range_lt + 5].opcode == OP_Prev,
	   "strict upper range emits SeekLT and scans descending");
	int before_unsigned_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(unsigned_range_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_unsigned_range].opcode == OP_Int64 &&
	   vdbe.aOp[before_unsigned_range].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_unsigned_range].p4.pI64 == UINT64_MAX &&
	   vdbe.aOp[before_unsigned_range + 1].opcode == OP_SeekGT,
	   "unsigned range seek retains UINT64_MAX in P4_UINT64");
	int before_bounded_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(bounded_range_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_bounded_range].opcode == OP_Integer &&
	   vdbe.aOp[before_bounded_range].p1 == 3 &&
	   vdbe.aOp[before_bounded_range + 1].opcode == OP_Integer &&
	   vdbe.aOp[before_bounded_range + 1].p1 == 1 &&
	   vdbe.aOp[before_bounded_range + 2].opcode == OP_SeekGE &&
	   vdbe.aOp[before_bounded_range + 3].opcode == OP_Column &&
	   vdbe.aOp[before_bounded_range + 4].opcode == OP_Le &&
	   vdbe.aOp[before_bounded_range + 4].p2 == before_bounded_range + 8 &&
	   vdbe.aOp[before_bounded_range + 7].opcode == OP_Next,
	   "bounded range seeks at lower bound and exits at exclusive upper bound");
	int before_filtered_bounded_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(filtered_bounded_range_desc, &vdbe, 4,
					20) == 0 &&
	   vdbe.aOp[before_filtered_bounded_range + 4].opcode == OP_Le &&
	   vdbe.aOp[before_filtered_bounded_range + 5].opcode == OP_Column &&
	   vdbe.aOp[before_filtered_bounded_range + 6].opcode == OP_NotNull &&
	   vdbe.aOp[before_filtered_bounded_range + 6].p2 ==
		before_filtered_bounded_range + 9 &&
	   vdbe.aOp[before_filtered_bounded_range + 9].opcode == OP_Next,
	   "bounded range ends before residual null filtering skips to Next");
	int before_wide_bounded_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(wide_bounded_range_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_wide_bounded_range].opcode == OP_Int64 &&
	   vdbe.aOp[before_wide_bounded_range].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_wide_bounded_range].p4.pI64 == INT64_MAX &&
	   vdbe.aOp[before_wide_bounded_range + 1].opcode == OP_Int64 &&
	   vdbe.aOp[before_wide_bounded_range + 1].p4type == P4_INT64 &&
	   *vdbe.aOp[before_wide_bounded_range + 1].p4.pI64 == INT64_MIN &&
	   vdbe.aOp[before_wide_bounded_range + 2].opcode == OP_SeekGE,
	   "wide bounded range encodes positive end and negative seek bounds safely");
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
	sql_plan_descriptor_delete(point_null_filter_desc);
	sql_plan_descriptor_delete(point_not_null_filter_desc);
	sql_plan_descriptor_delete(composite_point_desc);
	sql_plan_descriptor_delete(composite_prefix_desc);
	sql_plan_descriptor_delete(composite_prefix_limit_desc);
	sql_plan_descriptor_delete(composite_prefix_offset_desc);
	sql_plan_descriptor_delete(composite_prefix_zero_desc);
	sql_plan_descriptor_delete(composite_prefix_range_desc);
	sql_plan_descriptor_delete(composite_prefix_bounded_range_desc);
	sql_plan_descriptor_delete(range_gt_desc);
	sql_plan_descriptor_delete(range_le_desc);
	sql_plan_descriptor_delete(range_ge_desc);
	sql_plan_descriptor_delete(range_lt_desc);
	sql_plan_descriptor_delete(unsigned_range_desc);
	sql_plan_descriptor_delete(invalid_direction_range_desc);
	sql_plan_descriptor_delete(bounded_range_desc);
	sql_plan_descriptor_delete(wide_bounded_range_desc);
	sql_plan_descriptor_delete(filtered_bounded_range_desc);
	sql_plan_descriptor_delete(invalid_bounded_range_desc);
	sql_plan_descriptor_delete(invalid_point_desc);
	sql_plan_descriptor_delete(late_invalid_point_desc);
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
