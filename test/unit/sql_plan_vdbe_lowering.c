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
new_variable_point_descriptor(uint32_t variable)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	static const struct sql_plan_expression expression = {
		.id = 1,
		.canonical = "integer-point-parameter",
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
			.point_key_variable = variable,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = &expression,
		.expression_count = 1,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_secondary_equality_descriptor(int64_t key)
{
	static const uint32_t columns[] = {0};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	static const struct sql_plan_expression expression = {
		.id = 1,
		.canonical = "secondary-integer-equality-key",
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_EQUALITY_SCAN,
			.index_id = 1,
			.bounds = &bound,
			.bound_count = 1,
			.has_integer_point_key = true,
			.integer_point_key = key,
			.range_key_column = 3,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = &expression,
		.expression_count = 1,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_composite_secondary_equality_descriptor(void)
{
	static const uint32_t columns[] = {0};
	static const struct sql_plan_point_key_part parts[] = {
		{.integer_value = 7, .column = 3, .is_unsigned = false},
		{.unsigned_value = UINT64_MAX, .column = 4, .is_unsigned = true},
	};
	static const struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1},
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 2},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "secondary-integer-equality-part"},
		{.id = 2, .canonical = "secondary-unsigned-equality-part"},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_EQUALITY_SCAN,
			.index_id = 1,
			.bounds = bounds,
			.bound_count = 2,
			.point_key_parts = parts,
			.point_key_part_count = 2,
			.range_key_column = 3,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = sizeof(expressions) / sizeof(expressions[0]),
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_secondary_range_descriptor(bool bounded,
			       enum sql_plan_direction direction)
{
	static const uint32_t columns[] = {0};
	struct sql_plan_bound bounds[2] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_GE, .expr_ref = 1},
		{.side = SQL_PLAN_UPPER, .op = SQL_PLAN_LT, .expr_ref = 2},
	};
	struct sql_plan_expression expressions[2] = {
		{.id = 1, .canonical = "secondary-integer-range-lower"},
		{.id = 2, .canonical = "secondary-integer-range-upper"},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_RANGE_SCAN,
			.index_id = 1,
			.bounds = bounds,
			.bound_count = bounded ? 2 : 1,
			.has_integer_range_key = true,
			.integer_range_key = bounded ? 7 : 10,
			.integer_range_op = bounded ? SQL_PLAN_GE : SQL_PLAN_LE,
			.has_integer_range_end_key = bounded,
			.integer_range_end_key = 10,
			.integer_range_end_op = SQL_PLAN_LT,
			.range_key_column = 3,
			.direction = direction,
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = bounded ? 2 : 1,
	};
	if (!bounded) {
		bounds[0] = (struct sql_plan_bound) {
			.side = SQL_PLAN_UPPER,
			.op = SQL_PLAN_LE,
			.expr_ref = 1,
		};
		expressions[0].canonical = "secondary-integer-range-upper";
	}
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_secondary_prefix_range_descriptor(bool ordered, bool descending)
{
	static const uint32_t columns[] = {0};
	static const struct sql_plan_order_term asc_order[] = {
		{.column = 4, .direction = SQL_PLAN_ASC},
		{.column = 5, .direction = SQL_PLAN_ASC},
	};
	static const struct sql_plan_order_term desc_order[] = {
		{.column = 4, .direction = SQL_PLAN_DESC},
		{.column = 5, .direction = SQL_PLAN_DESC},
	};
	static const struct sql_plan_point_key_part prefix = {
		.column = 3, .integer_value = 7,
	};
	static const struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1},
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_GE, .expr_ref = 2},
		{.side = SQL_PLAN_UPPER, .op = SQL_PLAN_LT, .expr_ref = 3},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "secondary-integer-equality-part"},
		{.id = 2, .canonical = "secondary-unsigned-prefix-range-lower"},
		{.id = 3, .canonical = "secondary-unsigned-prefix-range-upper"},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100, .space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_RANGE_SCAN,
			.index_id = 1,
			.bounds = bounds, .bound_count = 3,
			.prefix_key_parts = &prefix, .prefix_key_part_count = 1,
			.has_unsigned_range_key = true, .unsigned_range_key = 10,
			.integer_range_op = SQL_PLAN_GE,
			.has_unsigned_range_end_key = true,
			.unsigned_range_end_key = 11,
			.integer_range_end_op = SQL_PLAN_LT,
			.range_key_column = 4,
			.direction = descending ? SQL_PLAN_DESC : SQL_PLAN_ASC,
			.produced_order = !ordered ? NULL : descending ?
				desc_order : asc_order,
			.produced_order_count = ordered ? 2 : 0,
		},
		.projection_columns = columns, .projection_column_count = 1,
		.expressions = expressions,
		.expression_count = sizeof(expressions) / sizeof(expressions[0]),
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_secondary_prefix_scan_descriptor(bool ordered, bool two_term,
				     bool natural_desc)
{
	static const uint32_t columns[] = {0};
	static const struct sql_plan_order_term order[] = {
		{.column = 4, .direction = SQL_PLAN_DESC},
		{.column = 5, .direction = SQL_PLAN_DESC},
	};
	static const struct sql_plan_point_key_part prefix = {
		.column = 3, .integer_value = 7,
	};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1,
	};
	static const struct sql_plan_expression expression = {
		.id = 1, .canonical = "secondary-integer-equality-part",
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100, .space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_PREFIX_SCAN,
			.index_id = 1,
			.bounds = &bound, .bound_count = 1,
			.prefix_key_parts = &prefix, .prefix_key_part_count = 1,
			.range_key_column = ordered ? 4 : 0,
			.direction = ordered && !natural_desc ? SQL_PLAN_DESC : SQL_PLAN_ASC,
			.produced_order = ordered ? order : NULL,
			.produced_order_count = ordered ? two_term ? 2 : 1 : 0,
		},
		.projection_columns = columns, .projection_column_count = 1,
		.expressions = &expression, .expression_count = 1,
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_secondary_full_scan_descriptor(enum sql_plan_direction direction,
				   enum sql_plan_direction order_direction)
{
	static const uint32_t columns[] = {0};
	const struct sql_plan_order_term order[] = {
		{.column = 3, .direction = order_direction},
		{.column = 4, .direction = order_direction},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_FULL_SCAN,
			.index_id = 1,
			.direction = direction,
			.produced_order = order,
			.produced_order_count = sizeof(order) / sizeof(order[0]),
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_secondary_mixed_full_scan_descriptor(enum sql_plan_direction direction,
						 bool inverse)
{
	static const uint32_t columns[] = {0};
	const struct sql_plan_order_term order[] = {
		{.column = 3, .direction = inverse ? SQL_PLAN_DESC : SQL_PLAN_ASC},
		{.column = 4, .direction = inverse ? SQL_PLAN_ASC : SQL_PLAN_DESC},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = SQL_PLAN_INDEX_FULL_SCAN,
			.index_id = 1,
			.direction = direction,
			.produced_order = order,
			.produced_order_count = sizeof(order) / sizeof(order[0]),
		},
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
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
new_point_multi_filter_descriptor(void)
{
	static const uint32_t columns[] = {2, 0};
	static const struct sql_plan_bound bound = {
		.side = SQL_PLAN_LOWER,
		.op = SQL_PLAN_EQ,
		.expr_ref = 1,
	};
	static const struct sql_plan_filter filters[] = {
		{.expr_ref = 2, .selectivity = 0.5, .column = 3,
		 .op = SQL_PLAN_FILTER_IS_NULL},
		{.expr_ref = 3, .selectivity = 0.5, .column = 4,
		 .op = SQL_PLAN_FILTER_IS_NOT_NULL},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "integer-point-key"},
		{.id = 2, .canonical = "direct-column-null-filter"},
		{.id = 3, .canonical = "direct-column-not-null-filter"},
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
		.filters = filters,
		.filter_count = sizeof(filters) / sizeof(filters[0]),
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = sizeof(expressions) / sizeof(expressions[0]),
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
new_composite_point_filter_descriptor(void)
{
	static const uint32_t columns[] = {2};
	static const struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1},
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 2},
	};
	static const struct sql_plan_point_key_part parts[] = {
		{.integer_value = -7, .column = 0},
		{.unsigned_value = UINT64_MAX, .column = 1, .is_unsigned = true},
	};
	static const struct sql_plan_filter filters[] = {
		{.expr_ref = 3, .selectivity = 0.5, .column = 3,
		 .op = SQL_PLAN_FILTER_IS_NULL},
		{.expr_ref = 4, .selectivity = 0.5, .column = 4,
		 .op = SQL_PLAN_FILTER_IS_NOT_NULL},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "composite-key-part-0"},
		{.id = 2, .canonical = "composite-key-part-1"},
		{.id = 3, .canonical = "direct-column-null-filter"},
		{.id = 4, .canonical = "direct-column-not-null-filter"},
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
			.bound_count = 2,
			.point_key_parts = parts,
			.point_key_part_count = 2,
		},
		.filters = filters,
		.filter_count = sizeof(filters) / sizeof(filters[0]),
		.projection_columns = columns,
		.projection_column_count = sizeof(columns) / sizeof(columns[0]),
		.expressions = expressions,
		.expression_count = sizeof(expressions) / sizeof(expressions[0]),
	};
	return sql_plan_descriptor_new(&input);
}

static struct sql_plan_descriptor *
new_composite_prefix_descriptor(bool with_limit, uint64_t limit,
				uint64_t offset, enum sql_plan_direction direction)
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
			.direction = direction,
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
new_composite_prefix_range_descriptor(bool bounded,
				      enum sql_plan_direction direction,
				      enum sql_plan_bound_op lower_op,
				      bool with_null_filter)
{
	static const uint32_t columns[] = {2};
	static const struct sql_plan_point_key_part prefix[] = {
		{.integer_value = 1, .column = 0},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "prefix-key-part"},
		{.id = 2, .canonical = "unsigned-range-lower"},
		{.id = 3, .canonical = "unsigned-range-upper"},
		{.id = 4, .canonical = "direct-column-null-filter"},
	};
	static const struct sql_plan_filter filter = {
		.expr_ref = 4,
		.column = 3,
		.op = SQL_PLAN_FILTER_IS_NULL,
	};
	struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1},
		{.side = SQL_PLAN_LOWER, .op = lower_op, .expr_ref = 2},
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
			.integer_range_op = lower_op,
			.has_unsigned_range_end_key = bounded,
			.unsigned_range_end_key = 40,
			.integer_range_end_op = SQL_PLAN_LT,
			.range_key_column = 1,
			.direction = direction,
		},
		.filters = with_null_filter ? &filter : NULL,
		.filter_count = with_null_filter ? 1 : 0,
		.projection_columns = columns,
		.projection_column_count = 1,
		.expressions = expressions,
		.expression_count = (bounded ? 3 : 2) + with_null_filter,
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
new_multi_filter_descriptor(bool bounded_range)
{
	static const uint32_t columns[] = {0};
	static const struct sql_plan_filter filters[] = {
		{.expr_ref = 3, .selectivity = 0.5, .column = 1,
		 .op = SQL_PLAN_FILTER_IS_NULL},
		{.expr_ref = 4, .selectivity = 0.5, .column = 2,
		 .op = SQL_PLAN_FILTER_IS_NOT_NULL},
	};
	static const struct sql_plan_expression expressions[] = {
		{.id = 1, .canonical = "integer-range-lower"},
		{.id = 2, .canonical = "integer-range-upper"},
		{.id = 3, .canonical = "direct-column-null-filter"},
		{.id = 4, .canonical = "direct-column-not-null-filter"},
	};
	static const struct sql_plan_bound bounds[] = {
		{.side = SQL_PLAN_LOWER, .op = SQL_PLAN_GE, .expr_ref = 1},
		{.side = SQL_PLAN_UPPER, .op = SQL_PLAN_LT, .expr_ref = 2},
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1,
		.planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.space_id = 100,
		.space_name = "lowering_t",
		.access = {
			.kind = bounded_range ? SQL_PLAN_INDEX_RANGE_SCAN :
				SQL_PLAN_TABLE_FULL_SCAN,
			.bounds = bounded_range ? bounds : NULL,
			.bound_count = bounded_range ? 2 : 0,
			.has_integer_range_key = bounded_range,
			.integer_range_key = 1,
			.integer_range_op = SQL_PLAN_GE,
			.has_integer_range_end_key = bounded_range,
			.integer_range_end_key = 3,
			.integer_range_end_op = SQL_PLAN_LT,
			.range_key_column = 0,
			.direction = SQL_PLAN_ASC,
			.est_rows = 4,
			.est_rows_confidence = 1,
		},
		.filters = filters,
		.filter_count = sizeof(filters) / sizeof(filters[0]),
		.projection_columns = columns,
		.projection_column_count = 1,
		.expressions = expressions,
		.expression_count = sizeof(expressions) / sizeof(expressions[0]),
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
	static const uint32_t columns[] = {UINT32_MAX - 1};
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

static struct sql_plan_descriptor *
new_expression_projection_descriptor(enum sql_plan_access_kind kind)
{
	static const uint32_t columns[] = {2, UINT32_MAX, 0};
	static const uint32_t refs[] = {0, 9, 0};
	static const struct sql_plan_expression expressions[] = {
		{1, "access-key"}, {9, "literal(42)"},
	};
	static const struct sql_plan_bound table_bound = {
		.side = SQL_PLAN_LOWER, .op = SQL_PLAN_GE, .expr_ref = 1,
	};
	static const struct sql_plan_bound point_bound = {
		.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1,
	};
	static const struct sql_plan_point_key_part prefix = {
		.column = 0, .integer_value = 1,
	};
	struct sql_plan_descriptor_input input = {
		.descriptor_version = 1, .planner_version = 1,
		.path_class = SQL_PLAN_NEW_PLANNER,
		.access = {.kind = kind, .direction = SQL_PLAN_ASC},
		.projection_columns = columns, .projection_column_count = 3,
		.projection_expr_refs = refs,
		.expressions = expressions,
		.expression_count = sizeof(expressions) / sizeof(expressions[0]),
	};
	switch (kind) {
	case SQL_PLAN_TABLE_FULL_SCAN:
		break;
	case SQL_PLAN_INDEX_RANGE_SCAN:
		input.access.has_integer_range_key = true;
		input.access.integer_range_key = 1;
		input.access.integer_range_op = SQL_PLAN_GE;
		input.access.bounds = &table_bound;
		input.access.bound_count = 1;
		break;
	case SQL_PLAN_PK_POINT_LOOKUP:
		input.access.has_integer_point_key = true;
		input.access.integer_point_key = 1;
		input.access.bounds = &point_bound;
		input.access.bound_count = 1;
		break;
	case SQL_PLAN_PK_PREFIX_SCAN: {
		static const struct sql_plan_bound prefix_bound = {
			.side = SQL_PLAN_LOWER, .op = SQL_PLAN_EQ, .expr_ref = 1,
		};
		input.access.prefix_key_parts = &prefix;
		input.access.prefix_key_part_count = 1;
		input.access.bounds = &prefix_bound;
		input.access.bound_count = 1;
		break;
	}
	default:
		return NULL;
	}
	return sql_plan_descriptor_new(&input);
}

struct projection_projector_ctx {
	struct Vdbe *vdbe;
	int count;
	uint32_t refs[3];
	int regs[3];
	int addrs[3];
};

static int
emit_projection_literal(void *context, uint32_t expr_ref, int result_reg)
{
	struct projection_projector_ctx *ctx = context;
	int slot = ctx->count++;
	assert(slot < 3);
	ctx->refs[slot] = expr_ref;
	ctx->regs[slot] = result_reg;
	ctx->addrs[slot] = sqlVdbeAddOp2(ctx->vdbe, OP_Integer, 42,
						 result_reg);
	return ctx->addrs[slot] == ctx->vdbe->nOp - 1 ? 0 : -1;
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
	plan(85);
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
	struct sql_plan_descriptor *variable_point_desc =
		new_variable_point_descriptor(2);
	struct sql_plan_descriptor *secondary_equality_desc =
		new_secondary_equality_descriptor(7);
	struct sql_plan_descriptor *composite_secondary_equality_desc =
		new_composite_secondary_equality_descriptor();
	struct sql_plan_descriptor *secondary_bounded_range_desc =
		new_secondary_range_descriptor(true, SQL_PLAN_ASC);
	struct sql_plan_descriptor *secondary_upper_range_desc =
		new_secondary_range_descriptor(false, SQL_PLAN_DESC);
	struct sql_plan_descriptor *secondary_bounded_reverse_range_desc =
		new_secondary_range_descriptor(true, SQL_PLAN_DESC);
	struct sql_plan_descriptor *secondary_prefix_range_desc =
		new_secondary_prefix_range_descriptor(false, false);
	struct sql_plan_descriptor *secondary_prefix_multi_range_order_desc =
		new_secondary_prefix_range_descriptor(true, false);
	struct sql_plan_descriptor *secondary_prefix_multi_range_reverse_desc =
		new_secondary_prefix_range_descriptor(true, true);
	struct sql_plan_descriptor *secondary_prefix_scan_desc =
		new_secondary_prefix_scan_descriptor(false, false, false);
	struct sql_plan_descriptor *secondary_prefix_order_desc =
		new_secondary_prefix_scan_descriptor(true, false, false);
	struct sql_plan_descriptor *secondary_prefix_multi_order_desc =
		new_secondary_prefix_scan_descriptor(true, true, false);
	struct sql_plan_descriptor *secondary_prefix_natural_desc_order =
		new_secondary_prefix_scan_descriptor(true, false, true);
	struct sql_plan_descriptor *secondary_full_asc_desc =
		new_secondary_full_scan_descriptor(SQL_PLAN_ASC, SQL_PLAN_ASC);
	struct sql_plan_descriptor *secondary_full_desc_desc =
		new_secondary_full_scan_descriptor(SQL_PLAN_DESC, SQL_PLAN_DESC);
	struct sql_plan_descriptor *secondary_full_desc_key_desc =
		new_secondary_full_scan_descriptor(SQL_PLAN_ASC, SQL_PLAN_DESC);
	struct sql_plan_descriptor *secondary_full_desc_key_asc =
		new_secondary_full_scan_descriptor(SQL_PLAN_DESC, SQL_PLAN_ASC);
	struct sql_plan_descriptor *negative_point_desc =
		new_point_descriptor(INT64_MIN);
	struct sql_plan_descriptor *unsigned_point_desc =
		new_unsigned_point_descriptor(UINT64_MAX);
	struct sql_plan_descriptor *point_null_filter_desc =
		new_point_null_filter_descriptor(SQL_PLAN_FILTER_IS_NULL);
	struct sql_plan_descriptor *point_not_null_filter_desc =
		new_point_null_filter_descriptor(SQL_PLAN_FILTER_IS_NOT_NULL);
	struct sql_plan_descriptor *point_multi_filter_desc =
		new_point_multi_filter_descriptor();
	struct sql_plan_descriptor *composite_point_desc =
		new_composite_point_descriptor();
	struct sql_plan_descriptor *composite_point_filter_desc =
		new_composite_point_filter_descriptor();
	struct sql_plan_descriptor *composite_prefix_desc =
		new_composite_prefix_descriptor(false, 0, 0, SQL_PLAN_ASC);
	struct sql_plan_descriptor *composite_prefix_limit_desc =
		new_composite_prefix_descriptor(true, 1, 0, SQL_PLAN_ASC);
	struct sql_plan_descriptor *composite_prefix_offset_desc =
		new_composite_prefix_descriptor(true, 1, 1, SQL_PLAN_ASC);
	struct sql_plan_descriptor *composite_prefix_zero_desc =
		new_composite_prefix_descriptor(true, 0, 0, SQL_PLAN_ASC);
	struct sql_plan_descriptor *composite_prefix_desc_reverse_desc =
		new_composite_prefix_descriptor(false, 0, 0, SQL_PLAN_DESC);
	struct sql_plan_descriptor *composite_prefix_range_desc =
		new_composite_prefix_range_descriptor(false, SQL_PLAN_ASC,
						      SQL_PLAN_GT, false);
	struct sql_plan_descriptor *composite_prefix_bounded_range_desc =
		new_composite_prefix_range_descriptor(true, SQL_PLAN_ASC,
						      SQL_PLAN_GT, false);
	struct sql_plan_descriptor *composite_prefix_desc_lower_only_desc =
		new_composite_prefix_range_descriptor(false, SQL_PLAN_DESC,
						      SQL_PLAN_GE, false);
	struct sql_plan_descriptor *composite_prefix_range_filter_desc =
		new_composite_prefix_range_descriptor(true, SQL_PLAN_ASC,
						      SQL_PLAN_GE, true);
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
	struct sql_plan_descriptor *range_lt_ascending_desc =
		new_range_descriptor(SQL_PLAN_LT, 7, false, SQL_PLAN_ASC, NULL);
	struct sql_plan_descriptor *range_le_ascending_desc =
		new_range_descriptor(SQL_PLAN_LE, 7, false, SQL_PLAN_ASC, NULL);
	struct sql_plan_descriptor *unsigned_range_desc =
		new_range_descriptor(SQL_PLAN_GT, (int64_t)UINT64_MAX, true,
				     SQL_PLAN_ASC, NULL);
	struct sql_plan_descriptor *range_gt_descending_desc =
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
	struct sql_plan_descriptor *multi_scan_filter_desc =
		new_multi_filter_descriptor(false);
	struct sql_plan_descriptor *multi_bounded_range_filter_desc =
		new_multi_filter_descriptor(true);
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
	   secondary_equality_desc != NULL &&
	   negative_point_desc != NULL && unsigned_point_desc != NULL &&
	   point_null_filter_desc != NULL &&
	   point_not_null_filter_desc != NULL &&
	   point_multi_filter_desc != NULL &&
	   multi_scan_filter_desc != NULL &&
	   multi_bounded_range_filter_desc != NULL &&
	   composite_point_desc != NULL &&
	   composite_point_filter_desc != NULL &&
	   composite_prefix_desc != NULL &&
	   composite_prefix_range_filter_desc != NULL &&
	   composite_prefix_limit_desc != NULL &&
	   composite_prefix_offset_desc != NULL &&
	   composite_prefix_zero_desc != NULL &&
	   range_gt_desc != NULL && range_le_desc != NULL &&
	   unsigned_range_desc != NULL && bounded_range_desc != NULL &&
	   range_lt_ascending_desc != NULL &&
	   range_le_ascending_desc != NULL &&
	   wide_bounded_range_desc != NULL && filtered_bounded_range_desc != NULL &&
	   range_gt_descending_desc != NULL &&
	   invalid_point_desc != NULL && late_invalid_point_desc != NULL &&
	   point_limit_desc != NULL && point_zero_limit_desc != NULL &&
	   point_offset_desc != NULL && variable_point_desc != NULL,
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
	struct projection_projector_ctx filter_projector_ctx = {
		.vdbe = &vdbe,
	};
	int before_expression_filter = vdbe.nOp;
	ok(sql_plan_lower_vdbe_table_scan_with_projector(filtered_desc, &vdbe,
		4, 20, emit_projection_literal, &filter_projector_ctx) == 0 &&
	   filter_projector_ctx.count == 1 &&
	   filter_projector_ctx.refs[0] == 1 &&
	   vdbe.aOp[filter_projector_ctx.addrs[0] + 1].opcode == OP_IfNot &&
	   vdbe.aOp[filter_projector_ctx.addrs[0] + 1].p3 == 1 &&
	   vdbe.aOp[filter_projector_ctx.addrs[0] + 2].opcode == OP_Column,
	   "expression filters project then reject false and NULL before output");
	vdbe.nOp = before_expression_filter;
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
	int before_multi_scan = vdbe.nOp;
	bool multi_scan_lowered = sql_plan_lower_vdbe_table_scan(
		multi_scan_filter_desc, &vdbe, 4, 20) == 0;
	int first_filter = -1;
	int second_filter = -1;
	int result_row = -1;
	int next_row = -1;
	for (int i = before_multi_scan; multi_scan_lowered && i < vdbe.nOp; ++i) {
		if (vdbe.aOp[i].opcode == OP_NotNull)
			first_filter = i;
		if (vdbe.aOp[i].opcode == OP_IsNull)
			second_filter = i;
		if (vdbe.aOp[i].opcode == OP_ResultRow)
			result_row = i;
		if (vdbe.aOp[i].opcode == OP_Next)
			next_row = i;
	}
	ok(multi_scan_lowered && first_filter > before_multi_scan &&
	   second_filter > first_filter && result_row > second_filter &&
	   next_row > result_row &&
	   vdbe.aOp[first_filter].p2 == next_row &&
	   vdbe.aOp[second_filter].p2 == next_row,
	   "full scan evaluates all null filters before projection and continues on rejection");
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
	int before_variable_point = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(variable_point_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_variable_point].opcode == OP_Variable &&
	   vdbe.aOp[before_variable_point].p1 == 2 &&
	   vdbe.aOp[before_variable_point + 1].opcode == OP_MustBeInt &&
	   vdbe.aOp[before_variable_point + 1].p2 == vdbe.nOp &&
	   vdbe.aOp[before_variable_point + 2].opcode == OP_IsNull &&
	   vdbe.aOp[before_variable_point + 2].p2 == vdbe.nOp &&
	   vdbe.aOp[before_variable_point + 3].opcode == OP_NotFound,
	   "parameter point lookup validates integer type and skips NULL before seek");
	static const uint32_t secondary_pk_columns[] = {0, 2};
	const struct sql_plan_secondary_index secondary_index = {
		.index_id = 1,
		.key_column = 3,
		.key_unsigned = false,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	static const uint32_t secondary_key_columns[] = {3, 4};
	static const bool secondary_key_unsigned[] = {false, true};
	static const bool secondary_key_descending[] = {false, false};
	static const bool secondary_mixed_key_descending[] = {false, true};
	static const bool secondary_desc_key_descending[] = {true, true};
	const struct sql_plan_secondary_index secondary_desc_range_index = {
		.index_id = 1,
		.key_column = 3,
		.key_unsigned = false,
		.key_columns = secondary_key_columns,
		.key_parts_unsigned = secondary_key_unsigned,
		.key_parts_descending = secondary_desc_key_descending,
		.key_part_count = 2,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	const struct sql_plan_secondary_index secondary_full_index = {
		.index_id = 1,
		.key_column = 3,
		.key_unsigned = false,
		.key_columns = secondary_key_columns,
		.key_parts_unsigned = secondary_key_unsigned,
		.key_parts_descending = secondary_key_descending,
		.key_part_count = 2,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	static const uint32_t secondary_prefix_order_columns[] = {3, 4, 5};
	static const bool secondary_prefix_order_unsigned[] = {false, true, false};
	static const bool secondary_prefix_order_descending[] = {false, false, false};
	static const bool secondary_prefix_mixed_descending[] = {false, true, false};
	const struct sql_plan_secondary_index secondary_prefix_order_index = {
		.index_id = 1,
		.key_column = 3,
		.key_unsigned = false,
		.key_columns = secondary_prefix_order_columns,
		.key_parts_unsigned = secondary_prefix_order_unsigned,
		.key_parts_descending = secondary_prefix_order_descending,
		.key_part_count = 3,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	const struct sql_plan_secondary_index secondary_prefix_mixed_index = {
		.index_id = 1,
		.key_column = 3,
		.key_unsigned = false,
		.key_columns = secondary_prefix_order_columns,
		.key_parts_unsigned = secondary_prefix_order_unsigned,
		.key_parts_descending = secondary_prefix_mixed_descending,
		.key_part_count = 3,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	const struct sql_plan_secondary_index secondary_prefix_range_index = {
		.index_id = 1,
		.key_column = 4,
		.key_unsigned = true,
		.key_columns = secondary_key_columns,
		.key_parts_unsigned = secondary_key_unsigned,
		.key_parts_descending = secondary_key_descending,
		.key_part_count = 2,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	static const uint32_t secondary_prefix_range_order_columns[] = {3, 4, 5};
	static const bool secondary_prefix_range_order_unsigned[] = {false, true, false};
	static const bool secondary_prefix_range_order_descending[] = {false, false, false};
	const struct sql_plan_secondary_index secondary_prefix_range_order_index = {
		.index_id = 1,
		.key_column = 4,
		.key_unsigned = true,
		.key_columns = secondary_prefix_range_order_columns,
		.key_parts_unsigned = secondary_prefix_range_order_unsigned,
		.key_parts_descending = secondary_prefix_range_order_descending,
		.key_part_count = 3,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	const struct sql_plan_secondary_index secondary_desc_full_index = {
		.index_id = 1,
		.key_column = 3,
		.key_unsigned = false,
		.key_columns = secondary_key_columns,
		.key_parts_unsigned = secondary_key_unsigned,
		.key_parts_descending = secondary_desc_key_descending,
		.key_part_count = 2,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	const struct sql_plan_secondary_index secondary_mixed_full_index = {
		.index_id = 1,
		.key_column = 3,
		.key_unsigned = false,
		.key_columns = secondary_key_columns,
		.key_parts_unsigned = secondary_key_unsigned,
		.key_parts_descending = secondary_mixed_key_descending,
		.key_part_count = 2,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	struct sql_plan_secondary_index wrong_secondary_index = secondary_index;
	wrong_secondary_index.index_id = 2;
	int before_wrong_secondary_index = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_equality_desc, &vdbe, 4, 5, &wrong_secondary_index, 20,
		NULL, NULL) == -1 && vdbe.nOp == before_wrong_secondary_index,
	   "secondary equality lowering rejects an index-ID mismatch atomically");
	wrong_secondary_index = secondary_index;
	wrong_secondary_index.key_column = 4;
	int before_wrong_secondary_column = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_equality_desc, &vdbe, 4, 5, &wrong_secondary_index, 20,
		NULL, NULL) == -1 && vdbe.nOp == before_wrong_secondary_column,
	   "secondary equality lowering rejects an indexed-column mismatch atomically");
	int before_secondary_equality = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_equality_desc, &vdbe, 4, 5, &secondary_index, 20,
		NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_equality].opcode == OP_Integer &&
	   vdbe.aOp[before_secondary_equality + 1].opcode == OP_SeekGE &&
	   vdbe.aOp[before_secondary_equality + 2].opcode == OP_IdxGT &&
	   vdbe.aOp[before_secondary_equality + 3].opcode == OP_Column &&
	   vdbe.aOp[before_secondary_equality + 3].p1 == 5 &&
	   vdbe.aOp[before_secondary_equality + 3].p2 == 0 &&
	   vdbe.aOp[before_secondary_equality + 4].opcode == OP_Column &&
	   vdbe.aOp[before_secondary_equality + 4].p2 == 2 &&
	   vdbe.aOp[before_secondary_equality + 5].opcode == OP_NotFound &&
	   vdbe.aOp[before_secondary_equality + 5].p1 == 4 &&
	   vdbe.aOp[before_secondary_equality + 5].p4.i == 2 &&
	   vdbe.aOp[before_secondary_equality + 6].opcode == OP_Column &&
	   vdbe.aOp[before_secondary_equality + 7].opcode == OP_ResultRow &&
	   vdbe.aOp[before_secondary_equality + 8].opcode == OP_Next &&
	   vdbe.aOp[before_secondary_equality + 8].p2 ==
		before_secondary_equality + 2 &&
	   vdbe.aOp[before_secondary_equality + 5].p2 < 0 &&
	   parse.aLabel[ADDR(vdbe.aOp[before_secondary_equality + 5].p2)] ==
		before_secondary_equality + 8 &&
	   vdbe.aOp[before_secondary_equality + 1].p2 ==
		before_secondary_equality + 9 &&
	   vdbe.aOp[before_secondary_equality + 2].p2 ==
		before_secondary_equality + 9,
	   "secondary equality scans the full key run and resolves composite primary keys before projection");
	static const uint32_t secondary_composite_columns[] = {3, 4};
	static const bool secondary_composite_unsigned[] = {false, true};
	const struct sql_plan_secondary_index composite_secondary_index = {
		.index_id = 1,
		.key_columns = secondary_composite_columns,
		.key_parts_unsigned = secondary_composite_unsigned,
		.key_part_count = 2,
		.key_column = 3,
		.key_unsigned = false,
		.primary_key_columns = secondary_pk_columns,
		.primary_key_count = 2,
	};
	struct sql_plan_secondary_index wrong_composite_secondary_index =
		composite_secondary_index;
	uint32_t wrong_composite_key_columns[] = {3, 5};
	wrong_composite_secondary_index.key_columns =
		wrong_composite_key_columns;
	int before_wrong_composite_secondary = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		composite_secondary_equality_desc, &vdbe, 4, 5,
		&wrong_composite_secondary_index, 20, NULL, NULL) == -1 &&
	   vdbe.nOp == before_wrong_composite_secondary,
	   "composite secondary lowering rejects a mismatched key part atomically");
	int before_composite_secondary = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		composite_secondary_equality_desc, &vdbe, 4, 5,
		&composite_secondary_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_composite_secondary].opcode == OP_Integer &&
	   vdbe.aOp[before_composite_secondary + 1].opcode == OP_Int64 &&
	   vdbe.aOp[before_composite_secondary + 1].p4type == P4_UINT64 &&
	   (uint64_t)*vdbe.aOp[before_composite_secondary + 1].p4.pI64 ==
		UINT64_MAX &&
	   vdbe.aOp[before_composite_secondary + 2].opcode == OP_SeekGE &&
	   vdbe.aOp[before_composite_secondary + 2].p4.i == 2 &&
	   vdbe.aOp[before_composite_secondary + 3].opcode == OP_IdxGT &&
	   vdbe.aOp[before_composite_secondary + 3].p4.i == 2,
	   "composite secondary equality seeks and guards the full typed key arity");
	int before_secondary_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_bounded_range_desc, &vdbe, 4, 5, &secondary_index, 20,
		NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_range].opcode == OP_Integer &&
	   vdbe.aOp[before_secondary_range + 2].opcode == OP_SeekGE &&
	   vdbe.aOp[before_secondary_range + 2].p4.i == 1 &&
	   vdbe.aOp[before_secondary_range + 3].opcode == OP_Column &&
	   vdbe.aOp[before_secondary_range + 4].opcode == OP_IsNull &&
	   vdbe.aOp[before_secondary_range + 5].opcode == OP_Le &&
	   vdbe.aOp[before_secondary_range + 11].opcode == OP_Next,
	   "bounded secondary range seeks at its lower endpoint and guards its upper endpoint");
	int before_secondary_prefix_range = vdbe.nOp;
	int secondary_prefix_rc = sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_prefix_range_desc, &vdbe, 4, 5,
		&secondary_prefix_range_index, 20, NULL, NULL);
	ok(secondary_prefix_rc == 0 &&
	   vdbe.aOp[before_secondary_prefix_range + 3].opcode == OP_SeekGE &&
	   vdbe.aOp[before_secondary_prefix_range + 3].p4.i == 2 &&
	   vdbe.aOp[before_secondary_prefix_range + 4].opcode == OP_IdxGT &&
	   vdbe.aOp[before_secondary_prefix_range + 4].p4.i == 1 &&
	   vdbe.aOp[before_secondary_prefix_range + 7].opcode == OP_Le &&
	   vdbe.aOp[before_secondary_prefix_range + 13].opcode == OP_Next,
	   "composite secondary range seeks prefix plus suffix and guards both boundaries");
	int before_secondary_prefix_multi_range_order = vdbe.nOp;
	int secondary_prefix_multi_range_rc =
		sql_plan_lower_vdbe_secondary_scan_with_projector(
			secondary_prefix_multi_range_order_desc, &vdbe, 4, 5,
			&secondary_prefix_range_order_index, 20, NULL, NULL);
	bool secondary_prefix_multi_range_has_next = false;
	for (int i = before_secondary_prefix_multi_range_order;
	     i < vdbe.nOp; ++i)
		secondary_prefix_multi_range_has_next |=
			vdbe.aOp[i].opcode == OP_Next;
	ok(secondary_prefix_multi_range_rc == 0 &&
	   secondary_prefix_multi_range_has_next,
	   "composite secondary range accepts a contiguous multi-term suffix order");
	int before_secondary_prefix_multi_range_reverse = vdbe.nOp;
	int secondary_prefix_multi_range_reverse_rc =
		sql_plan_lower_vdbe_secondary_scan_with_projector(
			secondary_prefix_multi_range_reverse_desc, &vdbe, 4, 5,
			&secondary_prefix_range_order_index, 20, NULL, NULL);
	bool has_reverse_range_seek = false;
	bool has_reverse_prefix_guard = false;
	bool has_reverse_range_step = false;
	for (int i = before_secondary_prefix_multi_range_reverse;
	     i < vdbe.nOp; ++i) {
		has_reverse_range_seek |= vdbe.aOp[i].opcode == OP_SeekLT;
		has_reverse_prefix_guard |= vdbe.aOp[i].opcode == OP_IdxLT;
		has_reverse_range_step |= vdbe.aOp[i].opcode == OP_Prev;
	}
	ok(secondary_prefix_multi_range_reverse_rc == 0 &&
	   has_reverse_range_seek && has_reverse_prefix_guard &&
	   has_reverse_range_step,
	   "bounded composite secondary range reverses suffix order and guards both limits");
	int before_secondary_prefix_scan = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_prefix_scan_desc, &vdbe, 4, 5, &secondary_full_index, 20,
		NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_prefix_scan + 1].opcode == OP_SeekGE &&
	   vdbe.aOp[before_secondary_prefix_scan + 1].p4.i == 1 &&
	   vdbe.aOp[before_secondary_prefix_scan + 2].opcode == OP_IdxGT &&
	   vdbe.aOp[before_secondary_prefix_scan + 2].p4.i == 1 &&
	   vdbe.aOp[before_secondary_prefix_scan + 8].opcode == OP_Next,
	   "secondary equality-prefix scan seeks and guards only the complete prefix");
	int before_secondary_prefix_order = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_prefix_order_desc, &vdbe, 4, 5, &secondary_full_index, 20,
		NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_prefix_order + 1].opcode == OP_SeekLE &&
	   vdbe.aOp[before_secondary_prefix_order + 2].opcode == OP_IdxLT &&
	   vdbe.aOp[before_secondary_prefix_order + 8].opcode == OP_Prev,
	   "secondary equality-prefix scan can walk a suffix in reverse index order");
	int before_secondary_prefix_multi_order = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_prefix_multi_order_desc, &vdbe, 4, 5,
		&secondary_prefix_order_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_prefix_multi_order + 1].opcode == OP_SeekLE &&
	   vdbe.aOp[before_secondary_prefix_multi_order + 2].opcode == OP_IdxLT &&
	   vdbe.aOp[before_secondary_prefix_multi_order + 8].opcode == OP_Prev,
	   "secondary equality-prefix scan emits a multi-term suffix order");
	int before_secondary_prefix_natural_desc = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_prefix_natural_desc_order, &vdbe, 4, 5,
		&secondary_prefix_mixed_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_prefix_natural_desc + 1].opcode == OP_SeekGE &&
	   vdbe.aOp[before_secondary_prefix_natural_desc + 2].opcode == OP_IdxGT &&
	   vdbe.aOp[before_secondary_prefix_natural_desc + 8].opcode == OP_Next,
	   "secondary prefix scan emits descending suffix order in a natural mixed-key walk");
	int before_secondary_upper_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_upper_range_desc, &vdbe, 4, 5, &secondary_index, 20,
		NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_upper_range + 1].opcode == OP_SeekLE &&
	   vdbe.aOp[before_secondary_upper_range + 2].opcode == OP_Column &&
	   vdbe.aOp[before_secondary_upper_range + 3].opcode == OP_IsNull &&
	   vdbe.aOp[before_secondary_upper_range + 9].opcode == OP_Prev,
	   "upper-only secondary range scans backward and terminates at nullable keys");
	int before_secondary_bounded_reverse = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_bounded_reverse_range_desc, &vdbe, 4, 5,
		&secondary_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_bounded_reverse].opcode == OP_Integer &&
	   vdbe.aOp[before_secondary_bounded_reverse].p1 == 10 &&
	   vdbe.aOp[before_secondary_bounded_reverse + 1].opcode == OP_Integer &&
	   vdbe.aOp[before_secondary_bounded_reverse + 1].p1 == 7 &&
	   vdbe.aOp[before_secondary_bounded_reverse + 2].opcode == OP_SeekLT &&
	   vdbe.aOp[before_secondary_bounded_reverse + 4].opcode == OP_IsNull &&
	   vdbe.aOp[before_secondary_bounded_reverse + 5].opcode == OP_Gt &&
	   vdbe.aOp[before_secondary_bounded_reverse + 11].opcode == OP_Prev,
	   "descending bounded secondary range seeks upper, stops below lower, and terminates at NULL");
	int before_secondary_desc_index_range = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_bounded_reverse_range_desc, &vdbe, 4, 5,
		&secondary_desc_range_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_desc_index_range].opcode == OP_Integer &&
	   vdbe.aOp[before_secondary_desc_index_range].p1 == 7 &&
	   vdbe.aOp[before_secondary_desc_index_range + 2].opcode == OP_SeekLE &&
	   vdbe.aOp[before_secondary_desc_index_range + 4].opcode == OP_IsNull &&
	   vdbe.aOp[before_secondary_desc_index_range + 5].opcode == OP_Le &&
	   vdbe.aOp[before_secondary_desc_index_range + 11].opcode == OP_Prev,
	   "descending secondary index inverts the seek and walks its natural order");
	int before_secondary_full_asc = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_full_asc_desc, &vdbe, 4, 5, &secondary_full_index, 20,
		NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_full_asc].opcode == OP_Rewind &&
	   vdbe.aOp[before_secondary_full_asc + 1].opcode == OP_Column &&
	   vdbe.aOp[before_secondary_full_asc + 3].opcode == OP_NotFound &&
	   vdbe.aOp[before_secondary_full_asc + 5].opcode == OP_ResultRow &&
	   vdbe.aOp[before_secondary_full_asc + 6].opcode == OP_Next,
	   "ascending secondary full scan resolves the base row and projects in index order");
	int before_secondary_full_desc = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_full_desc_desc, &vdbe, 4, 5, &secondary_full_index, 20,
		NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_full_desc].opcode == OP_Last &&
	   vdbe.aOp[before_secondary_full_desc + 5].opcode == OP_ResultRow &&
	   vdbe.aOp[before_secondary_full_desc + 6].opcode == OP_Prev,
	   "descending secondary full scan walks the index backward");
	int before_descending_index_forward = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_full_desc_key_desc, &vdbe, 4, 5,
		&secondary_desc_full_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_descending_index_forward].opcode == OP_Rewind &&
	   vdbe.aOp[before_descending_index_forward + 6].opcode == OP_Next,
	   "descending secondary index emits its natural order in a forward walk");
	int before_descending_index_reverse = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_full_desc_key_asc, &vdbe, 4, 5,
		&secondary_desc_full_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_descending_index_reverse].opcode == OP_Last &&
	   vdbe.aOp[before_descending_index_reverse + 6].opcode == OP_Prev,
	   "descending secondary index emits inverse order in a reverse walk");
	struct sql_plan_descriptor *secondary_mixed_forward_desc =
		new_secondary_mixed_full_scan_descriptor(SQL_PLAN_ASC, false);
	struct sql_plan_descriptor *secondary_mixed_reverse_desc =
		new_secondary_mixed_full_scan_descriptor(SQL_PLAN_DESC, true);
	int before_secondary_mixed_forward = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_mixed_forward_desc, &vdbe, 4, 5,
		&secondary_mixed_full_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_mixed_forward].opcode == OP_Rewind &&
	   vdbe.aOp[before_secondary_mixed_forward + 6].opcode == OP_Next,
	   "mixed-direction secondary key order uses its natural forward walk");
	int before_secondary_mixed_reverse = vdbe.nOp;
	ok(sql_plan_lower_vdbe_secondary_scan_with_projector(
		secondary_mixed_reverse_desc, &vdbe, 4, 5,
		&secondary_mixed_full_index, 20, NULL, NULL) == 0 &&
	   vdbe.aOp[before_secondary_mixed_reverse].opcode == OP_Last &&
	   vdbe.aOp[before_secondary_mixed_reverse + 6].opcode == OP_Prev,
	   "mixed-direction secondary key order reverses every term together");
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
	int before_point_multi_filter = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(point_multi_filter_desc, &vdbe, 4,
					       20) == 0 &&
	   vdbe.aOp[before_point_multi_filter + 2].opcode == OP_Column &&
	   vdbe.aOp[before_point_multi_filter + 2].p2 == 3 &&
	   vdbe.aOp[before_point_multi_filter + 3].opcode == OP_NotNull &&
	   vdbe.aOp[before_point_multi_filter + 4].opcode == OP_Column &&
	   vdbe.aOp[before_point_multi_filter + 4].p2 == 4 &&
	   vdbe.aOp[before_point_multi_filter + 5].opcode == OP_IsNull &&
	   vdbe.aOp[before_point_multi_filter + 6].opcode == OP_Column &&
	   vdbe.aOp[before_point_multi_filter + 8].opcode == OP_ResultRow &&
	   vdbe.aOp[before_point_multi_filter + 1].p2 ==
		before_point_multi_filter + 9 &&
	   vdbe.aOp[before_point_multi_filter + 3].p2 ==
		before_point_multi_filter + 9 &&
	   vdbe.aOp[before_point_multi_filter + 5].p2 ==
		before_point_multi_filter + 9,
	   "all point residual checks precede projection and share the result exit");
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
	int before_composite_point_filter = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_point(composite_point_filter_desc, &vdbe, 4,
					20) == 0 &&
	   vdbe.aOp[before_composite_point_filter + 2].opcode == OP_NotFound &&
	   vdbe.aOp[before_composite_point_filter + 2].p4.i == 2 &&
	   vdbe.aOp[before_composite_point_filter + 3].opcode == OP_Column &&
	   vdbe.aOp[before_composite_point_filter + 3].p2 == 3 &&
	   vdbe.aOp[before_composite_point_filter + 4].opcode == OP_NotNull &&
	   vdbe.aOp[before_composite_point_filter + 5].opcode == OP_Column &&
	   vdbe.aOp[before_composite_point_filter + 5].p2 == 4 &&
	   vdbe.aOp[before_composite_point_filter + 6].opcode == OP_IsNull &&
	   vdbe.aOp[before_composite_point_filter + 7].opcode == OP_Column &&
	   vdbe.aOp[before_composite_point_filter + 8].opcode == OP_ResultRow &&
	   vdbe.aOp[before_composite_point_filter + 2].p2 ==
		before_composite_point_filter + 9 &&
	   vdbe.aOp[before_composite_point_filter + 4].p2 ==
		before_composite_point_filter + 9 &&
	   vdbe.aOp[before_composite_point_filter + 6].p2 ==
		before_composite_point_filter + 9,
	   "composite point lookup applies every null residual before projection");
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
	int before_prefix_reverse = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(composite_prefix_desc_reverse_desc,
						      &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_prefix_reverse + 2].opcode == OP_SeekLE &&
	   vdbe.aOp[before_prefix_reverse + 2].p4.i == 2 &&
	   vdbe.aOp[before_prefix_reverse + 3].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_reverse + 4].opcode == OP_Ne &&
	   vdbe.aOp[before_prefix_reverse + 5].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_reverse + 6].opcode == OP_Ne &&
	   vdbe.aOp[before_prefix_reverse + 7].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_reverse + 8].opcode == OP_ResultRow &&
	   vdbe.aOp[before_prefix_reverse + 9].opcode == OP_Prev,
	   "descending composite prefix scan seeks backward and guards prefix");
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
	int before_prefix_desc_lower_only = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_prefix_scan(
		   composite_prefix_desc_lower_only_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_prefix_desc_lower_only + 2].opcode == OP_SeekLE &&
	   vdbe.aOp[before_prefix_desc_lower_only + 2].p4.i == 1 &&
	   vdbe.aOp[before_prefix_desc_lower_only + 3].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_desc_lower_only + 4].opcode == OP_Ne &&
	   vdbe.aOp[before_prefix_desc_lower_only + 5].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_desc_lower_only + 6].opcode == OP_Gt &&
	   vdbe.aOp[before_prefix_desc_lower_only + 7].opcode == OP_Column &&
	   vdbe.aOp[before_prefix_desc_lower_only + 8].opcode == OP_ResultRow &&
	   vdbe.aOp[before_prefix_desc_lower_only + 9].opcode == OP_Prev,
	   "descending lower-only prefix range seeks on prefix and guards suffix");
	int before_prefix_range_filter = vdbe.nOp;
	int prefix_filter_rc = sql_plan_lower_vdbe_pk_prefix_scan(
		composite_prefix_range_filter_desc, &vdbe, 4, 20);
	int filter_seek = -1, filter_range_end = -1, filter_null_check = -1;
	int filter_result = -1, filter_next = -1;
	if (prefix_filter_rc == 0) {
		for (int i = before_prefix_range_filter; i < vdbe.nOp; ++i) {
			if (vdbe.aOp[i].opcode == OP_SeekGE)
				filter_seek = i;
			if (vdbe.aOp[i].opcode == OP_Le)
				filter_range_end = i;
			if (vdbe.aOp[i].opcode == OP_NotNull)
				filter_null_check = i;
			if (vdbe.aOp[i].opcode == OP_ResultRow)
				filter_result = i;
			if (vdbe.aOp[i].opcode == OP_Next)
				filter_next = i;
		}
	}
	ok(prefix_filter_rc == 0,
	   "bounded prefix range accepts a null residual filter");
	ok(filter_seek >= before_prefix_range_filter &&
	   filter_range_end > filter_seek &&
	   filter_null_check > filter_range_end &&
	   filter_result > filter_null_check && filter_next > filter_result,
	   "bounded prefix range tests range end and residual before projection");
	ok(filter_null_check >= 0 && vdbe.aOp[filter_null_check].p2 == filter_next,
	   "prefix residual rejection jumps directly to the next cursor row");
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
	int before_range_gt_descending = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_gt_descending_desc, &vdbe, 4,
					20) == 0 &&
	   vdbe.aOp[before_range_gt_descending].opcode == OP_Integer &&
	   vdbe.aOp[before_range_gt_descending + 1].opcode == OP_Last &&
	   vdbe.aOp[before_range_gt_descending + 2].opcode == OP_Column &&
	   vdbe.aOp[before_range_gt_descending + 3].opcode == OP_Ge &&
	   vdbe.aOp[before_range_gt_descending + 3].p2 ==
		before_range_gt_descending + 8 &&
	   vdbe.aOp[before_range_gt_descending + 6].opcode == OP_ResultRow &&
	   vdbe.aOp[before_range_gt_descending + 7].opcode == OP_Prev,
	   "strict lower range scans backward from Last and stops at its guard");
	int before_range_lt = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_lt_desc, &vdbe, 4, 20) == 0 &&
	   vdbe.aOp[before_range_lt + 1].opcode == OP_SeekLT &&
	   vdbe.aOp[before_range_lt + 1].p2 == before_range_lt + 6 &&
	   vdbe.aOp[before_range_lt + 5].opcode == OP_Prev,
	   "strict upper range emits SeekLT and scans descending");
	int before_range_lt_ascending = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_lt_ascending_desc, &vdbe, 4,
					20) == 0 &&
	   vdbe.aOp[before_range_lt_ascending].opcode == OP_Integer &&
	   vdbe.aOp[before_range_lt_ascending + 1].opcode == OP_Rewind &&
	   vdbe.aOp[before_range_lt_ascending + 2].opcode == OP_Column &&
	   vdbe.aOp[before_range_lt_ascending + 3].opcode == OP_Le &&
	   vdbe.aOp[before_range_lt_ascending + 3].p2 ==
		before_range_lt_ascending + 8 &&
	   vdbe.aOp[before_range_lt_ascending + 6].opcode == OP_ResultRow &&
	   vdbe.aOp[before_range_lt_ascending + 7].opcode == OP_Next,
	   "strict upper range scans ascending from the first key and stops at its bound");
	int before_range_le_ascending = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(range_le_ascending_desc, &vdbe, 4,
					20) == 0 &&
	   vdbe.aOp[before_range_le_ascending + 1].opcode == OP_Rewind &&
	   vdbe.aOp[before_range_le_ascending + 3].opcode == OP_Lt &&
	   vdbe.aOp[before_range_le_ascending + 3].p2 ==
		before_range_le_ascending + 8 &&
	   vdbe.aOp[before_range_le_ascending + 7].opcode == OP_Next,
	   "inclusive upper range scans ascending through its bound");
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
	int before_multi_bounded = vdbe.nOp;
	ok(sql_plan_lower_vdbe_pk_range(multi_bounded_range_filter_desc, &vdbe,
					4, 20) == 0 &&
	   vdbe.aOp[before_multi_bounded + 3].opcode == OP_Column &&
	   vdbe.aOp[before_multi_bounded + 4].opcode == OP_Le &&
	   vdbe.aOp[before_multi_bounded + 5].opcode == OP_Column &&
	   vdbe.aOp[before_multi_bounded + 6].opcode == OP_NotNull &&
	   vdbe.aOp[before_multi_bounded + 6].p2 == before_multi_bounded + 11 &&
	   vdbe.aOp[before_multi_bounded + 7].opcode == OP_Column &&
	   vdbe.aOp[before_multi_bounded + 8].opcode == OP_IsNull &&
	   vdbe.aOp[before_multi_bounded + 8].p2 == before_multi_bounded + 11 &&
	   vdbe.aOp[before_multi_bounded + 11].opcode == OP_Next,
	   "bounded range end check precedes every residual filter and both reject to Next");
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
	struct sql_plan_descriptor *expr_scan =
		new_expression_projection_descriptor(SQL_PLAN_TABLE_FULL_SCAN);
	int before_missing_projector = vdbe.nOp;
	ok(expr_scan != NULL && sql_plan_lower_vdbe_table_scan(expr_scan,
		&vdbe, 4, 20) == -1 && vdbe.nOp == before_missing_projector,
	   "expression projections without a projector are rejected atomically");
	const enum sql_plan_access_kind projection_kinds[] = {
		SQL_PLAN_TABLE_FULL_SCAN, SQL_PLAN_PK_POINT_LOOKUP,
		SQL_PLAN_INDEX_RANGE_SCAN, SQL_PLAN_PK_PREFIX_SCAN,
	};
	for (size_t i = 0; i < sizeof(projection_kinds) /
	     sizeof(projection_kinds[0]); ++i) {
		struct sql_plan_descriptor *projection_desc = i == 0 ? expr_scan :
			new_expression_projection_descriptor(projection_kinds[i]);
		struct projection_projector_ctx projector_ctx = {
			.vdbe = &vdbe,
		};
		int rc;
		switch (projection_kinds[i]) {
		case SQL_PLAN_TABLE_FULL_SCAN:
			rc = sql_plan_lower_vdbe_table_scan_with_projector(
				projection_desc, &vdbe, 4, 20,
				emit_projection_literal, &projector_ctx);
			break;
		case SQL_PLAN_PK_POINT_LOOKUP:
			rc = sql_plan_lower_vdbe_pk_point_with_projector(
				projection_desc, &vdbe, 4, 20,
				emit_projection_literal, &projector_ctx);
			break;
		case SQL_PLAN_INDEX_RANGE_SCAN:
			rc = sql_plan_lower_vdbe_pk_range_with_projector(
				projection_desc, &vdbe, 4, 20,
				emit_projection_literal, &projector_ctx);
			break;
		case SQL_PLAN_PK_PREFIX_SCAN:
			rc = sql_plan_lower_vdbe_pk_prefix_scan_with_projector(
				projection_desc, &vdbe, 4, 20,
				emit_projection_literal, &projector_ctx);
			break;
		default:
			unreachable();
		}
		int expr_addr = projector_ctx.addrs[0];
		ok(projection_desc != NULL && rc == 0 &&
		   projector_ctx.count == 1 && projector_ctx.refs[0] == 9 &&
		   projector_ctx.regs[0] == 21 &&
		   vdbe.aOp[expr_addr - 1].opcode == OP_Column &&
		   vdbe.aOp[expr_addr].opcode == OP_Integer &&
		   vdbe.aOp[expr_addr + 1].opcode == OP_Column &&
		   vdbe.aOp[expr_addr + 2].opcode == OP_ResultRow,
		   "projector emits expression opcode in its ordered projection slot");
		if (i != 0)
			sql_plan_descriptor_delete(projection_desc);
	}
	sql_plan_descriptor_delete(expr_scan);

	sql_plan_descriptor_delete(plan_desc);
	sql_plan_descriptor_delete(filtered_desc);
	sql_plan_descriptor_delete(descending_desc);
	sql_plan_descriptor_delete(secondary_mixed_forward_desc);
	sql_plan_descriptor_delete(secondary_mixed_reverse_desc);
	sql_plan_descriptor_delete(limit_one_desc);
	sql_plan_descriptor_delete(limit_zero_desc);
	sql_plan_descriptor_delete(offset_limit_desc);
	sql_plan_descriptor_delete(wide_offset_limit_desc);
	sql_plan_descriptor_delete(invalid_offset_desc);
	sql_plan_descriptor_delete(point_desc);
	sql_plan_descriptor_delete(variable_point_desc);
	sql_plan_descriptor_delete(secondary_equality_desc);
	sql_plan_descriptor_delete(negative_point_desc);
	sql_plan_descriptor_delete(unsigned_point_desc);
	sql_plan_descriptor_delete(point_null_filter_desc);
	sql_plan_descriptor_delete(point_not_null_filter_desc);
	sql_plan_descriptor_delete(multi_scan_filter_desc);
	sql_plan_descriptor_delete(point_multi_filter_desc);
	sql_plan_descriptor_delete(composite_point_desc);
	sql_plan_descriptor_delete(composite_point_filter_desc);
	sql_plan_descriptor_delete(composite_prefix_desc);
	sql_plan_descriptor_delete(composite_prefix_limit_desc);
	sql_plan_descriptor_delete(composite_prefix_offset_desc);
	sql_plan_descriptor_delete(composite_prefix_zero_desc);
	sql_plan_descriptor_delete(composite_prefix_range_desc);
	sql_plan_descriptor_delete(composite_prefix_bounded_range_desc);
	sql_plan_descriptor_delete(composite_prefix_desc_lower_only_desc);
	sql_plan_descriptor_delete(composite_prefix_range_filter_desc);
	sql_plan_descriptor_delete(range_gt_desc);
	sql_plan_descriptor_delete(range_le_desc);
	sql_plan_descriptor_delete(range_ge_desc);
	sql_plan_descriptor_delete(range_lt_desc);
	sql_plan_descriptor_delete(range_lt_ascending_desc);
	sql_plan_descriptor_delete(range_le_ascending_desc);
	sql_plan_descriptor_delete(unsigned_range_desc);
	sql_plan_descriptor_delete(range_gt_descending_desc);
	sql_plan_descriptor_delete(bounded_range_desc);
	sql_plan_descriptor_delete(wide_bounded_range_desc);
	sql_plan_descriptor_delete(filtered_bounded_range_desc);
	sql_plan_descriptor_delete(multi_bounded_range_filter_desc);
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
	sql_xfree(parse.aLabel);
	footer();
	int rc = check_plan();
	box_free();
	event_free();
	coll_free();
	fiber_free();
	memory_free();
	return rc;
}
