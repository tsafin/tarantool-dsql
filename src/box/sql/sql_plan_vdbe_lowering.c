#include "sql_plan_lowering.h"

#include <limits.h>
#include <stdint.h>

#include "sqlInt.h"
#include "core/diag.h"
#include "opcodes.h"
#include "vdbe.h"
#include "vdbeInt.h"

static int
sql_plan_emit_projection(const struct sql_plan_descriptor_input *input,
			 struct Vdbe *vdbe, Parse *parse,
			 const struct vdbe_codegen_checkpoint *checkpoint,
			 int cursor, size_t slot, int result_reg,
			 sql_plan_projection_projector_f projector,
			 void *projector_ctx)
{
	uint32_t expr_ref = input->projection_expr_refs == NULL ? 0 :
		input->projection_expr_refs[slot];
	int addr;
	if (expr_ref != 0) {
		if (projector == NULL || projector(projector_ctx, expr_ref,
						    result_reg) != 0)
			return -1;
		addr = vdbe->nOp - 1;
	} else {
		addr = sqlVdbeAddOp3(vdbe, OP_Column, cursor,
				     input->projection_columns[slot], result_reg);
	}
	return addr == vdbe->nOp - 1 && !parse->is_aborted &&
		diag_last_error(diag_get()) == checkpoint->diag_error ? 0 : -1;
}

static bool
sql_plan_filter_is_valid(const struct sql_plan_filter *filter,
			 sql_plan_projection_projector_f projector)
{
	if (filter->op == SQL_PLAN_FILTER_EXPRESSION)
		return filter->expr_ref != 0 && projector != NULL;
	return (filter->op == SQL_PLAN_FILTER_IS_NULL ||
		filter->op == SQL_PLAN_FILTER_IS_NOT_NULL) &&
		filter->column <= INT_MAX;
}

static int
sql_plan_emit_filter(const struct sql_plan_filter *filter,
		     struct Vdbe *vdbe, Parse *parse,
		     const struct vdbe_codegen_checkpoint *checkpoint,
		     int cursor, int filter_reg,
		     sql_plan_projection_projector_f projector,
		     void *projector_ctx)
{
	int addr;
	if (filter->op == SQL_PLAN_FILTER_EXPRESSION) {
		if (projector == NULL || projector(projector_ctx, filter->expr_ref,
						   filter_reg) != 0)
			return -1;
		addr = vdbe->nOp - 1;
	} else {
		addr = sqlVdbeAddOp3(vdbe, OP_Column, cursor,
				     (int)filter->column, filter_reg);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint->diag_error)
			return -1;
		int op = filter->op == SQL_PLAN_FILTER_IS_NULL ? OP_NotNull :
			OP_IsNull;
		addr = sqlVdbeAddOp2(vdbe, op, filter_reg, 0);
	}
	if (addr != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint->diag_error)
		return -1;
	if (filter->op == SQL_PLAN_FILTER_EXPRESSION) {
		addr = sqlVdbeAddOp3(vdbe, OP_IfNot, filter_reg, 0, 1);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint->diag_error)
			return -1;
	}
	return addr;
}

int
sql_plan_lower_vdbe_pk_point_with_projector(
			     const struct sql_plan_descriptor *plan,
			     struct Vdbe *vdbe, int cursor,
			     int result_first_reg,
			     sql_plan_projection_projector_f projector,
			     void *projector_ctx)
{
	if (plan == NULL || vdbe == NULL || vdbe->pParse == NULL || cursor < 0 ||
	    result_first_reg < 1 || vdbe->magic != VDBE_MAGIC_INIT)
		return -1;
	const struct sql_plan_descriptor_input *input =
		sql_plan_descriptor_get_input(plan);
	if (input == NULL)
		return -1;
	bool composite_point = input->access.point_key_part_count != 0;
	bool multi_point = input->access.point_key_value_count != 0;
	if (input->path_class != SQL_PLAN_NEW_PLANNER ||
	    input->access.kind != SQL_PLAN_PK_POINT_LOOKUP ||
	    (multi_point &&
	     (input->access.point_key_value_count == 0 ||
	      input->access.point_key_value_count > SQL_PLAN_PK_MULTI_VALUE_MAX ||
	      input->access.point_key_values == NULL || composite_point ||
	      input->access.has_integer_point_key ||
	      input->access.has_unsigned_point_key ||
	      input->access.point_key_variable != 0)) ||
	    (composite_point ?
	     (input->access.has_integer_point_key ||
	      input->access.has_unsigned_point_key ||
	      input->access.point_key_variable != 0 ||
	      input->access.point_key_parts == NULL ||
	      input->access.point_key_part_count > INT_MAX ||
	      input->access.bound_count != input->access.point_key_part_count) :
	     (!multi_point && ((input->access.point_key_variable == 0 &&
	       input->access.has_integer_point_key ==
	       input->access.has_unsigned_point_key) ||
	      (input->access.point_key_variable != 0 &&
	       (input->access.has_integer_point_key ||
		input->access.has_unsigned_point_key ||
		input->access.point_key_variable > INT_MAX))))) ||
	    input->filter_count > SQL_PLAN_FILTER_MAX ||
	    (input->filter_count != 0 && input->filters == NULL) ||
	    input->finalize_count > 1 ||
	    (input->finalize_count == 1 &&
	     (input->finalize == NULL ||
	      input->finalize[0].kind != SQL_PLAN_LIMIT ||
	      input->finalize[0].limit > INT64_MAX ||
	      input->finalize[0].offset > INT64_MAX)) ||
	    input->projection_columns == NULL ||
	    input->projection_column_count == 0 ||
	    input->projection_column_count > INT_MAX ||
	    result_first_reg > INT_MAX - (int)input->projection_column_count + 1)
		return -1;
	for (size_t i = 0; i < input->filter_count; ++i) {
		if (!sql_plan_filter_is_valid(&input->filters[i], projector))
			return -1;
	}
	Parse *parse = vdbe->pParse;
	struct vdbe_codegen_checkpoint checkpoint;
	if (vdbe_codegen_checkpoint_init(&checkpoint, vdbe) != 0)
		return -1;
	if (input->finalize_count == 1 &&
	    (input->finalize[0].limit == 0 ||
	     (!multi_point && input->finalize[0].offset != 0))) {
		int skip = sqlVdbeAddOp2(vdbe, OP_Goto, 0, 0);
		if (skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
		sqlVdbeJumpHere(vdbe, skip);
		vdbe_codegen_checkpoint_commit(&checkpoint);
		return 0;
	}
	if (multi_point) {
		bool has_limit = input->finalize_count == 1;
		bool has_offset = has_limit && input->finalize[0].offset != 0;
		int registers_needed = 1 + (has_limit ? 1 : 0) +
			(has_offset ? 1 : 0) + (input->filter_count != 0 ? 1 : 0);
		if (parse->nMem > INT_MAX - registers_needed)
			goto error;
		int key_reg = ++parse->nMem;
		int limit_reg = has_limit ? ++parse->nMem : 0;
		int offset_reg = has_offset ? ++parse->nMem : 0;
		int filter_reg = input->filter_count != 0 ? ++parse->nMem : 0;
		if (has_limit) {
			uint64_t limit = input->finalize[0].limit;
			int addr = limit <= INT_MAX ?
				sqlVdbeAddOp2(vdbe, OP_Integer, (int)limit, limit_reg) :
				sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, limit_reg, 0,
						  (const u8 *)&limit, P4_UINT64);
			if (addr != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto error;
		}
		if (has_offset) {
			uint64_t offset = input->finalize[0].offset;
			int addr = offset <= INT_MAX ?
				sqlVdbeAddOp2(vdbe, OP_Integer, (int)offset, offset_reg) :
				sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, offset_reg, 0,
						  (const u8 *)&offset, P4_UINT64);
			if (addr != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto error;
		}
		int limit_breaks[SQL_PLAN_PK_MULTI_VALUE_MAX];
		size_t limit_break_count = 0;
		for (size_t key_no = 0;
		     key_no < input->access.point_key_value_count; ++key_no) {
			size_t value_no = input->access.direction == SQL_PLAN_DESC ?
				input->access.point_key_value_count - key_no - 1 : key_no;
			const struct sql_plan_point_key_part *part =
				&input->access.point_key_values[value_no];
			int key_op;
			if (part->is_unsigned) {
				uint64_t key = part->unsigned_value;
				if (key <= INT_MAX)
					key_op = sqlVdbeAddOp2(vdbe, OP_Integer,
							       (int)key, key_reg);
				else
					key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0,
						key_reg, 0, (const u8 *)&key, P4_UINT64);
			} else {
				int64_t key = part->integer_value;
				if (key >= INT_MIN && key <= INT_MAX)
					key_op = sqlVdbeAddOp2(vdbe, OP_Integer,
							       (int)key, key_reg);
				else if (key < 0)
					key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0,
						key_reg, 0, (const u8 *)&key, P4_INT64);
				else {
					uint64_t value = (uint64_t)key;
					key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0,
						key_reg, 0, (const u8 *)&value, P4_UINT64);
				}
			}
			if (key_op != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto error;
			int miss = sqlVdbeAddOp4Int(vdbe, OP_NotFound, cursor, 0,
						    key_reg, 1);
			if (miss != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto error;
			/* The cursor now points at a different tuple. A projector may have
			 * cached a column from the preceding point lookup. */
			sqlExprCacheClear(parse);
			int filter_breaks[SQL_PLAN_FILTER_MAX];
			for (size_t i = 0; i < input->filter_count; ++i) {
				filter_breaks[i] = sql_plan_emit_filter(&input->filters[i],
					vdbe, parse, &checkpoint, cursor, filter_reg,
					projector, projector_ctx);
				if (filter_breaks[i] < 0)
					goto error;
			}
			int offset_skip = -1;
			if (has_offset) {
				offset_skip = sqlVdbeAddOp2(vdbe, OP_IfNotZero,
							   offset_reg, 0);
				if (offset_skip != vdbe->nOp - 1 || parse->is_aborted ||
				    diag_last_error(diag_get()) != checkpoint.diag_error)
					goto error;
			}
			for (size_t i = 0; i < input->projection_column_count; ++i) {
				uint32_t expr_ref = input->projection_expr_refs == NULL ? 0 :
					input->projection_expr_refs[i];
				if ((expr_ref == 0 && input->projection_columns[i] > INT_MAX) ||
				    (expr_ref != 0 && projector == NULL) ||
				    sql_plan_emit_projection(input, vdbe, parse, &checkpoint,
					cursor, i, result_first_reg + (int)i, projector,
					projector_ctx) != 0)
					goto error;
			}
			int result = sqlVdbeAddOp2(vdbe, OP_ResultRow, result_first_reg,
						    (int)input->projection_column_count);
			if (result != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto error;
			if (has_limit) {
				int limit_break = sqlVdbeAddOp2(vdbe, OP_DecrJumpZero,
								limit_reg, 0);
				if (limit_break != vdbe->nOp - 1 || parse->is_aborted ||
				    diag_last_error(diag_get()) != checkpoint.diag_error)
					goto error;
				limit_breaks[limit_break_count++] = limit_break;
			}
			if (offset_skip >= 0)
				sqlVdbeJumpHere(vdbe, offset_skip);
			for (size_t i = 0; i < input->filter_count; ++i)
				sqlVdbeJumpHere(vdbe, filter_breaks[i]);
			sqlVdbeJumpHere(vdbe, miss);
		}
		for (size_t i = 0; i < limit_break_count; ++i)
			sqlVdbeJumpHere(vdbe, limit_breaks[i]);
		vdbe_codegen_checkpoint_commit(&checkpoint);
		return 0;
	}
	size_t point_key_count = composite_point ?
		input->access.point_key_part_count : 1;
	if (point_key_count > INT_MAX ||
	    parse->nMem > INT_MAX - (int)point_key_count)
		goto error;
	int key_reg = parse->nMem + 1;
	parse->nMem += (int)point_key_count;
	for (size_t i = 0; i < point_key_count; ++i) {
		struct sql_plan_point_key_part scalar_part = {
			.integer_value = input->access.integer_point_key,
			.unsigned_value = input->access.unsigned_point_key,
			.is_unsigned = input->access.has_unsigned_point_key,
		};
		const struct sql_plan_point_key_part *part = composite_point ?
			&input->access.point_key_parts[i] : &scalar_part;
		int reg = key_reg + (int)i;
		int key_op;
		if (!composite_point && input->access.point_key_variable != 0) {
			int variable = sqlVdbeAddOp2(vdbe, OP_Variable,
						(int)input->access.point_key_variable,
						reg);
			if (variable != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto error;
			key_op = sqlVdbeAddOp2(vdbe, OP_MustBeInt, reg, 0);
		} else if (part->is_unsigned) {
			uint64_t key = part->unsigned_value;
			if (key <= INT_MAX) {
				key_op = sqlVdbeAddOp2(vdbe, OP_Integer, (int)key, reg);
			} else {
				key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
						   (const u8 *)&key, P4_UINT64);
			}
		} else {
			int64_t key = part->integer_value;
			if (key >= INT_MIN && key <= INT_MAX) {
				key_op = sqlVdbeAddOp2(vdbe, OP_Integer, (int)key, reg);
			} else if (key < 0) {
				key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
						   (const u8 *)&key, P4_INT64);
			} else {
				uint64_t value = (uint64_t)key;
				key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
						   (const u8 *)&value, P4_UINT64);
			}
		}
		if (key_op != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	int invalid_key = -1;
	int null_key = -1;
	if (!composite_point && input->access.point_key_variable != 0) {
		invalid_key = vdbe->nOp - 1;
		null_key = sqlVdbeAddOp2(vdbe, OP_IsNull, key_reg, 0);
		if (null_key != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	int miss = sqlVdbeAddOp4Int(vdbe, OP_NotFound, cursor, 0, key_reg,
				     (int)point_key_count);
	if (miss != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	int filter_breaks[SQL_PLAN_FILTER_MAX];
	if (input->filter_count != 0) {
		if (parse->nMem == INT_MAX)
			goto error;
		int filter_reg = ++parse->nMem;
		for (size_t i = 0; i < input->filter_count; ++i) {
			filter_breaks[i] = sql_plan_emit_filter(&input->filters[i],
				vdbe, parse, &checkpoint, cursor, filter_reg,
				projector, projector_ctx);
			if (filter_breaks[i] < 0)
				goto error;
		}
	}
	for (size_t i = 0; i < input->projection_column_count; ++i) {
		uint32_t expr_ref = input->projection_expr_refs == NULL ? 0 :
			input->projection_expr_refs[i];
		if ((expr_ref == 0 && input->projection_columns[i] > INT_MAX) ||
		    (expr_ref != 0 && projector == NULL))
			goto error;
		if (sql_plan_emit_projection(input, vdbe, parse, &checkpoint, cursor,
					     i, result_first_reg + (int)i,
					     projector, projector_ctx) != 0)
			goto error;
	}
	int result = sqlVdbeAddOp2(vdbe, OP_ResultRow, result_first_reg,
				    (int)input->projection_column_count);
	if (result != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	sqlVdbeJumpHere(vdbe, miss);
	if (invalid_key >= 0)
		sqlVdbeJumpHere(vdbe, invalid_key);
	if (null_key >= 0)
		sqlVdbeJumpHere(vdbe, null_key);
	for (size_t i = 0; i < input->filter_count; ++i)
		sqlVdbeJumpHere(vdbe, filter_breaks[i]);
	vdbe_codegen_checkpoint_commit(&checkpoint);
	return 0;
error:
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	return -1;
}

static int
sql_plan_emit_integer_constant(struct Vdbe *vdbe, int reg, bool is_unsigned,
			       int64_t signed_value, uint64_t unsigned_value)
{
	if (is_unsigned) {
		if (unsigned_value <= INT_MAX)
			return sqlVdbeAddOp2(vdbe, OP_Integer,
					     (int)unsigned_value, reg);
		return sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
					  (const u8 *)&unsigned_value, P4_UINT64);
	}
	if (signed_value >= INT_MIN && signed_value <= INT_MAX)
		return sqlVdbeAddOp2(vdbe, OP_Integer, (int)signed_value, reg);
	if (signed_value < 0)
		return sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
					  (const u8 *)&signed_value, P4_INT64);
	uint64_t positive = (uint64_t)signed_value;
	return sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
				  (const u8 *)&positive, P4_UINT64);
}

int
sql_plan_lower_vdbe_pk_prefix_scan_with_projector(
				   const struct sql_plan_descriptor *plan,
				   struct Vdbe *vdbe, int cursor,
				   int result_first_reg,
				   sql_plan_projection_projector_f projector,
				   void *projector_ctx)
{
	if (plan == NULL || vdbe == NULL || vdbe->pParse == NULL || cursor < 0 ||
	    result_first_reg < 1 || vdbe->magic != VDBE_MAGIC_INIT)
		return -1;
	const struct sql_plan_descriptor_input *in =
		sql_plan_descriptor_get_input(plan);
	bool has_range_key = in != NULL && (in->access.has_integer_range_key ||
		in->access.has_unsigned_range_key);
	bool has_range_end = in != NULL &&
		(in->access.has_integer_range_end_key ||
		 in->access.has_unsigned_range_end_key);
	bool has_lower = has_range_key &&
		(in->access.integer_range_op == SQL_PLAN_GT ||
		 in->access.integer_range_op == SQL_PLAN_GE);
	bool has_upper = has_range_end || (has_range_key &&
		(in->access.integer_range_op == SQL_PLAN_LT ||
		 in->access.integer_range_op == SQL_PLAN_LE));
	bool descending = in != NULL &&
		in->access.direction == SQL_PLAN_DESC;
	size_t range_bounds = has_range_key ? (has_range_end ? 2 : 1) : 0;
	if (in == NULL || in->path_class != SQL_PLAN_NEW_PLANNER ||
	    in->access.kind != SQL_PLAN_PK_PREFIX_SCAN ||
	    (in->access.direction != SQL_PLAN_ASC &&
	     in->access.direction != SQL_PLAN_DESC) ||
	    in->access.prefix_key_parts == NULL ||
	    in->access.prefix_key_part_count == 0 ||
	    in->access.prefix_key_part_count > SQL_PLAN_POINT_KEY_PART_MAX ||
	    in->access.prefix_key_part_count > INT_MAX || in->access.bounds == NULL ||
	    in->filter_count > SQL_PLAN_FILTER_MAX ||
	    (in->filter_count != 0 && in->filters == NULL) ||
	    in->finalize_count > 1 ||
	    (in->finalize_count == 1 &&
	     (in->finalize == NULL ||
	      in->finalize[0].kind != SQL_PLAN_LIMIT ||
	      in->finalize[0].limit > INT64_MAX ||
	      in->finalize[0].offset > INT64_MAX)) ||
	    in->access.produced_order_count > SQL_PLAN_POINT_KEY_PART_MAX ||
	    in->projection_columns == NULL || in->projection_column_count == 0 ||
	    in->projection_column_count > INT_MAX ||
	    result_first_reg > INT_MAX - (int)in->projection_column_count + 1 ||
	    in->access.bound_count != in->access.prefix_key_part_count +
		range_bounds ||
	    (has_range_key &&
	     (in->access.has_integer_range_key ==
	      in->access.has_unsigned_range_key ||
	      (in->access.range_key_column > INT_MAX) ||
	      (!has_lower && !has_upper) ||
	      (has_range_end && (in->access.has_integer_range_key !=
			 in->access.has_integer_range_end_key ||
			 in->access.has_unsigned_range_key !=
			 in->access.has_unsigned_range_end_key)))))
		return -1;
	for (size_t i = 0; i < in->filter_count; ++i)
		if (!sql_plan_filter_is_valid(&in->filters[i], projector))
			return -1;
	for (size_t i = 0; i < in->access.prefix_key_part_count; ++i)
		if (in->access.bounds[i].op != SQL_PLAN_EQ ||
		    in->access.prefix_key_parts[i].column > INT_MAX)
			return -1;
	for (size_t i = 0; i < in->access.produced_order_count; ++i)
		if (in->access.produced_order[i].direction !=
			    in->access.direction ||
		    in->access.produced_order[i].column > INT_MAX)
			return -1;
	for (size_t i = 0; i < in->projection_column_count; ++i) {
		uint32_t expr_ref = in->projection_expr_refs == NULL ? 0 :
			in->projection_expr_refs[i];
		if ((expr_ref == 0 && in->projection_columns[i] > INT_MAX) ||
		    (expr_ref != 0 && projector == NULL))
			return -1;
	}
	Parse *parse = vdbe->pParse;
	bool has_limit = in->finalize_count == 1;
	bool has_offset = has_limit && in->finalize[0].offset != 0;
	if (has_limit && in->finalize[0].limit == 0) {
		struct vdbe_codegen_checkpoint checkpoint;
		if (vdbe_codegen_checkpoint_init(&checkpoint, vdbe) != 0)
			return -1;
		int skip = sqlVdbeAddOp2(vdbe, OP_Goto, 0, 0);
		if (skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error) {
			vdbe_codegen_checkpoint_rollback(&checkpoint);
			return -1;
		}
		sqlVdbeJumpHere(vdbe, skip);
		vdbe_codegen_checkpoint_commit(&checkpoint);
		return 0;
	}
	int key_count = (int)in->access.prefix_key_part_count;
	int extra_regs = key_count + 1 + (has_lower ? 1 : 0) +
		(has_upper ? 1 : 0) + (has_limit ? 1 : 0) +
		(has_offset ? 1 : 0) + (in->filter_count != 0 ? 1 : 0);
	if (parse->nMem > INT_MAX - extra_regs)
		return -1;
	struct vdbe_codegen_checkpoint checkpoint;
	if (vdbe_codegen_checkpoint_init(&checkpoint, vdbe) != 0)
		return -1;
	int key_reg = parse->nMem + 1;
	parse->nMem += key_count;
	int range_key_reg;
	int range_end_reg;
	if (descending && has_range_end) {
		/* The upper endpoint is the seek key for a descending bounded scan,
		 * so keep it adjacent to the equality-prefix registers. */
		range_end_reg = ++parse->nMem;
		range_key_reg = ++parse->nMem;
	} else {
		range_key_reg = has_lower ? ++parse->nMem : 0;
		range_end_reg = has_upper ? ++parse->nMem : 0;
	}
	int current_reg = ++parse->nMem;
	int filter_reg = in->filter_count == 0 ? 0 : ++parse->nMem;
	int limit_reg = has_limit ? ++parse->nMem : 0;
	int offset_reg = has_offset ? ++parse->nMem : 0;
	for (int i = 0; i < key_count; ++i) {
		const struct sql_plan_point_key_part *part =
			&in->access.prefix_key_parts[i];
		int reg = key_reg + i;
		int addr;
		if (part->is_unsigned) {
			uint64_t value = part->unsigned_value;
			addr = value <= INT_MAX ?
				sqlVdbeAddOp2(vdbe, OP_Integer, (int)value, reg) :
				sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
						  (const u8 *)&value, P4_UINT64);
		} else {
			int64_t value = part->integer_value;
			if (value >= INT_MIN && value <= INT_MAX) {
				addr = sqlVdbeAddOp2(vdbe, OP_Integer, (int)value, reg);
			} else if (value < 0) {
				addr = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
						 (const u8 *)&value, P4_INT64);
			} else {
				uint64_t positive = (uint64_t)value;
				addr = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, reg, 0,
						 (const u8 *)&positive, P4_UINT64);
			}
		}
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto prefix_error;
	}
	if (has_lower || has_upper) {
		bool start_unsigned = in->access.has_unsigned_range_key;
		int64_t start_signed = in->access.integer_range_key;
		uint64_t start_unsigned_value = in->access.unsigned_range_key;
		if (has_lower) {
			int addr = sql_plan_emit_integer_constant(vdbe, range_key_reg,
				start_unsigned, start_signed,
				start_unsigned_value);
			if (addr != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto prefix_error;
		}
		if (has_upper) {
			bool end_unsigned = has_range_end ?
				in->access.has_unsigned_range_end_key :
				in->access.has_unsigned_range_key;
			int64_t end_signed = has_range_end ?
				in->access.integer_range_end_key :
				in->access.integer_range_key;
			uint64_t end_unsigned_value = has_range_end ?
				in->access.unsigned_range_end_key :
				in->access.unsigned_range_key;
			int addr = sql_plan_emit_integer_constant(vdbe, range_end_reg,
				end_unsigned, end_signed, end_unsigned_value);
			if (addr != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto prefix_error;
		}
	}
	if (has_limit) {
		uint64_t value = in->finalize[0].limit;
		int addr = value <= INT_MAX ?
			sqlVdbeAddOp2(vdbe, OP_Integer, (int)value, limit_reg) :
			sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, limit_reg, 0,
					  (const u8 *)&value, P4_UINT64);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto prefix_error;
	}
	if (has_offset) {
		uint64_t value = in->finalize[0].offset;
		int addr = value <= INT_MAX ?
			sqlVdbeAddOp2(vdbe, OP_Integer, (int)value, offset_reg) :
			sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, offset_reg, 0,
					  (const u8 *)&value, P4_UINT64);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto prefix_error;
	}
	int seek_op;
	int seek_key_reg = key_reg;
	int seek_key_count = key_count;
	if (descending) {
		/* Bounded/upper-only descending ranges start at their upper
		 * endpoint. For a lower-only range, seek to the end of the
		 * equality-prefix and let the prefix and lower-bound guards stop
		 * the reverse walk. */
		seek_op = !has_upper ? OP_SeekLE : has_range_end ?
			(in->access.integer_range_end_op == SQL_PLAN_LT ?
			 OP_SeekLT : OP_SeekLE) :
			(in->access.integer_range_op == SQL_PLAN_LT ?
			 OP_SeekLT : OP_SeekLE);
		if (has_upper)
			seek_key_count++;
	} else {
		seek_op = has_lower ?
			(in->access.integer_range_op == SQL_PLAN_GT ? OP_SeekGT :
			 OP_SeekGE) : OP_SeekGE;
		if (has_lower)
			seek_key_count++;
	}
	int seek = sqlVdbeAddOp4Int(vdbe, seek_op, cursor, 0, seek_key_reg,
				    seek_key_count);
	if (seek != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto prefix_error;
	int body = sqlVdbeCurrentAddr(vdbe);
	int mismatch[SQL_PLAN_POINT_KEY_PART_MAX];
	for (int i = 0; i < key_count; ++i) {
		int addr = sqlVdbeAddOp3(vdbe, OP_Column, cursor,
					 in->access.prefix_key_parts[i].column,
					 current_reg);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto prefix_error;
		mismatch[i] = sqlVdbeAddOp3(vdbe, OP_Ne, current_reg, 0,
					    key_reg + i);
		if (mismatch[i] != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto prefix_error;
	}
	int range_break = -1;
	if ((!descending && has_upper) || (descending && has_lower)) {
		int addr = sqlVdbeAddOp3(vdbe, OP_Column, cursor,
					 in->access.range_key_column, current_reg);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto prefix_error;
		enum sql_plan_bound_op range_stop_op = descending ?
			in->access.integer_range_op : (has_range_end ?
			in->access.integer_range_end_op :
			in->access.integer_range_op);
		int stop_reg = descending ? range_key_reg : range_end_reg;
		/* VDBE comparisons compare P3 against P1. For ascending walks an
		 * upper-exclusive bound exits at current >= upper; for descending
		 * walks a lower-exclusive bound exits at current <= lower.
		 */
		int check_op = descending ?
			(range_stop_op == SQL_PLAN_GT ? OP_Ge : OP_Gt) :
			(range_stop_op == SQL_PLAN_LT ? OP_Le : OP_Lt);
		range_break = sqlVdbeAddOp3(vdbe, check_op, current_reg, 0,
					    stop_reg);
		if (range_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto prefix_error;
	}
	int filter_breaks[SQL_PLAN_FILTER_MAX];
	for (size_t i = 0; i < in->filter_count; ++i) {
		filter_breaks[i] = sql_plan_emit_filter(&in->filters[i], vdbe,
			parse, &checkpoint, cursor, filter_reg, projector,
			projector_ctx);
		if (filter_breaks[i] < 0)
			goto prefix_error;
	}
	int offset_skip = has_offset ?
		sqlVdbeAddOp2(vdbe, OP_IfNotZero, offset_reg, 0) : -1;
	if (has_offset && (offset_skip != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error))
		goto prefix_error;
	for (size_t i = 0; i < in->projection_column_count; ++i) {
		if (sql_plan_emit_projection(in, vdbe, parse, &checkpoint, cursor,
					     i, result_first_reg + (int)i,
					     projector, projector_ctx) != 0)
			goto prefix_error;
	}
	int result = sqlVdbeAddOp2(vdbe, OP_ResultRow, result_first_reg,
				    (int)in->projection_column_count);
	if (result != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto prefix_error;
	int limit_break = has_limit ?
		sqlVdbeAddOp2(vdbe, OP_DecrJumpZero, limit_reg, 0) : -1;
	if (has_limit && (limit_break != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error))
		goto prefix_error;
	if (offset_skip >= 0)
		sqlVdbeJumpHere(vdbe, offset_skip);
	/* Residual rejection keeps scanning inside the matching prefix range. */
	for (size_t i = 0; i < in->filter_count; ++i)
		sqlVdbeJumpHere(vdbe, filter_breaks[i]);
	int next = sqlVdbeAddOp2(vdbe, descending ? OP_Prev : OP_Next,
				  cursor, body);
	if (next != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto prefix_error;
	sqlVdbeJumpHere(vdbe, seek);
	for (int i = 0; i < key_count; ++i)
		sqlVdbeJumpHere(vdbe, mismatch[i]);
	if (range_break >= 0)
		sqlVdbeJumpHere(vdbe, range_break);
	if (limit_break >= 0)
		sqlVdbeJumpHere(vdbe, limit_break);
	vdbe_codegen_checkpoint_commit(&checkpoint);
	return 0;
prefix_error:
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	return -1;
}

static int
sql_plan_lower_vdbe_scan(const struct sql_plan_descriptor *plan,
			 struct Vdbe *vdbe, int cursor,
			 int result_first_reg, bool range,
			 sql_plan_projection_projector_f projector,
			 void *projector_ctx)
{
	if (plan == NULL || vdbe == NULL || vdbe->pParse == NULL || cursor < 0 ||
	    result_first_reg < 1 || vdbe->magic != VDBE_MAGIC_INIT)
		return -1;
	const struct sql_plan_descriptor_input *input =
		sql_plan_descriptor_get_input(plan);
	bool has_null_filter = input != NULL && input->filter_count != 0;
	bool has_range_end = input != NULL &&
		(input->access.has_integer_range_end_key ||
		 input->access.has_unsigned_range_end_key);
	bool invalid_range = range && input != NULL &&
		(input->access.has_integer_range_key ==
		 input->access.has_unsigned_range_key ||
		 (input->access.integer_range_op != SQL_PLAN_GT &&
		  input->access.integer_range_op != SQL_PLAN_GE &&
		  input->access.integer_range_op != SQL_PLAN_LT &&
		  input->access.integer_range_op != SQL_PLAN_LE) ||
		 (input->access.has_integer_range_end_key &&
		  input->access.has_unsigned_range_end_key) ||
		 (has_range_end &&
		  (input->access.has_integer_range_key !=
		   input->access.has_integer_range_end_key ||
		   input->access.has_unsigned_range_key !=
		   input->access.has_unsigned_range_end_key ||
		   (input->access.integer_range_end_op != SQL_PLAN_LT &&
		    input->access.integer_range_end_op != SQL_PLAN_LE))) ||
		 (has_range_end && input->access.range_key_column > INT_MAX));
	if (input == NULL || input->path_class != SQL_PLAN_NEW_PLANNER ||
	    input->access.kind != (range ? SQL_PLAN_INDEX_RANGE_SCAN :
				   SQL_PLAN_TABLE_FULL_SCAN) ||
	    invalid_range ||
	    (input->filter_count > SQL_PLAN_FILTER_MAX) ||
	    (input->filter_count != 0 && input->filters == NULL) ||
	    input->finalize_count > 1 ||
	    (input->finalize_count == 1 &&
	     (input->finalize == NULL ||
	      input->finalize[0].kind != SQL_PLAN_LIMIT ||
	      input->finalize[0].offset > INT64_MAX ||
	      input->finalize[0].limit > INT64_MAX)) ||
	    input->projection_columns == NULL ||
	    input->projection_column_count == 0 ||
	    input->projection_column_count > INT_MAX ||
	    result_first_reg > INT_MAX - (int)input->projection_column_count + 1)
		return -1;
	for (size_t i = 0; i < input->filter_count; ++i) {
		if (!sql_plan_filter_is_valid(&input->filters[i], projector))
			return -1;
	}
	for (size_t i = 0; i < input->projection_column_count; i++) {
		uint32_t expr_ref = input->projection_expr_refs == NULL ? 0 :
			input->projection_expr_refs[i];
		if ((expr_ref == 0 && input->projection_columns[i] > INT_MAX) ||
		    (expr_ref != 0 && projector == NULL))
			return -1;
	}
	Parse *parse = vdbe->pParse;
	int registers_needed = 0;
	if (input->finalize_count == 1 && input->finalize[0].limit != 0) {
		registers_needed = 1;
		if (input->finalize[0].offset != 0)
			++registers_needed;
	}
	bool bounded_range = range &&
		(input->access.has_integer_range_end_key ||
		 input->access.has_unsigned_range_end_key);
	bool upper_only_ascending = range && !bounded_range &&
		input->access.direction == SQL_PLAN_ASC &&
		(input->access.integer_range_op == SQL_PLAN_LT ||
		 input->access.integer_range_op == SQL_PLAN_LE);
	bool lower_only_descending = range && !bounded_range &&
		input->access.direction == SQL_PLAN_DESC &&
		(input->access.integer_range_op == SQL_PLAN_GT ||
		 input->access.integer_range_op == SQL_PLAN_GE);
	bool has_range_guard = bounded_range || upper_only_ascending ||
		lower_only_descending;
	if (has_range_guard)
		registers_needed += 2;
	if (has_null_filter)
		++registers_needed;
	if (parse->nMem > INT_MAX - registers_needed)
		return -1;
	struct vdbe_codegen_checkpoint checkpoint;
	if (vdbe_codegen_checkpoint_init(&checkpoint, vdbe) != 0)
		return -1;
	bool has_limit = input->finalize_count == 1;
	int limit_reg = 0;
	int offset_reg = 0;
	if (has_limit && input->finalize[0].limit == 0) {
		int skip = sqlVdbeAddOp2(vdbe, OP_Goto, 0, 0);
		if (skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
		sqlVdbeJumpHere(vdbe, skip);
		vdbe_codegen_checkpoint_commit(&checkpoint);
		return 0;
	}
	if (has_limit) {
		limit_reg = ++parse->nMem;
		int addr;
		if (input->finalize[0].limit <= INT_MAX) {
			addr = sqlVdbeAddOp2(vdbe, OP_Integer,
					     (int)input->finalize[0].limit,
					     limit_reg);
		} else {
			uint64_t value = input->finalize[0].limit;
			addr = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, limit_reg, 0,
						 (const u8 *)&value, P4_UINT64);
		}
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
		if (input->finalize[0].offset != 0) {
			offset_reg = ++parse->nMem;
			if (input->finalize[0].offset <= INT_MAX) {
				addr = sqlVdbeAddOp2(vdbe, OP_Integer,
						     (int)input->finalize[0].offset,
						     offset_reg);
			} else {
				uint64_t value = input->finalize[0].offset;
				addr = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0,
						offset_reg, 0, (const u8 *)&value,
						P4_UINT64);
			}
			if (addr != vdbe->nOp - 1 || parse->is_aborted ||
			    diag_last_error(diag_get()) != checkpoint.diag_error)
				goto error;
		}
	}
	int filter_reg = has_null_filter ? ++parse->nMem : 0;
	int end_reg = 0;
	int current_reg = 0;
	if (has_range_guard) {
		if (parse->nMem > INT_MAX - 2)
			goto error;
		end_reg = ++parse->nMem;
		current_reg = ++parse->nMem;
		int end_op;
		bool use_lower_bound = input->access.direction == SQL_PLAN_DESC &&
			!upper_only_ascending;
		bool use_unsigned = upper_only_ascending ?
			input->access.has_unsigned_range_key : use_lower_bound ?
			input->access.has_unsigned_range_key :
			input->access.has_unsigned_range_end_key;
		if (use_unsigned) {
			uint64_t key = upper_only_ascending || use_lower_bound ?
				input->access.unsigned_range_key :
				input->access.unsigned_range_end_key;
			end_op = key <= INT_MAX ?
				sqlVdbeAddOp2(vdbe, OP_Integer, (int)key, end_reg) :
				sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, end_reg, 0,
						  (const u8 *)&key, P4_UINT64);
		} else {
			int64_t key = upper_only_ascending || use_lower_bound ?
				input->access.integer_range_key :
				input->access.integer_range_end_key;
			if (key >= INT_MIN && key <= INT_MAX) {
				end_op = sqlVdbeAddOp2(vdbe, OP_Integer, (int)key,
						       end_reg);
			} else if (key < 0) {
				end_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0,
						   end_reg, 0, (const u8 *)&key,
						   P4_INT64);
			} else {
				uint64_t value = (uint64_t)key;
				end_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0,
						   end_reg, 0,
						   (const u8 *)&value, P4_UINT64);
			}
		}
		if (end_op != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	int rewind_op = input->access.direction == SQL_PLAN_DESC ? OP_Last :
		OP_Rewind;
	int step_op = input->access.direction == SQL_PLAN_DESC ? OP_Prev :
		OP_Next;
	int rewind;
	if (!range || upper_only_ascending || lower_only_descending) {
		rewind = sqlVdbeAddOp2(vdbe, rewind_op, cursor, 0);
	} else {
		if (parse->nMem == INT_MAX)
			goto error;
		int key_reg = ++parse->nMem;
		int key_op;
		bool start_at_end = bounded_range &&
			input->access.direction == SQL_PLAN_DESC;
		bool use_unsigned = start_at_end ?
			input->access.has_unsigned_range_end_key :
			input->access.has_unsigned_range_key;
		if (use_unsigned) {
			uint64_t key = start_at_end ?
				input->access.unsigned_range_end_key :
				input->access.unsigned_range_key;
			key_op = key <= INT_MAX ?
				sqlVdbeAddOp2(vdbe, OP_Integer, (int)key, key_reg) :
				sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, key_reg, 0,
						  (const u8 *)&key, P4_UINT64);
		} else {
			int64_t key = start_at_end ?
				input->access.integer_range_end_key :
				input->access.integer_range_key;
			if (key >= INT_MIN && key <= INT_MAX) {
				key_op = sqlVdbeAddOp2(vdbe, OP_Integer, (int)key,
						       key_reg);
			} else if (key < 0) {
				key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, key_reg,
						   0, (const u8 *)&key, P4_INT64);
			} else {
				uint64_t value = (uint64_t)key;
				key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, key_reg,
						   0, (const u8 *)&value,
						   P4_UINT64);
			}
		}
		if (key_op != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
		int seek_op;
		enum sql_plan_bound_op start_bound = start_at_end ?
			input->access.integer_range_end_op :
			input->access.integer_range_op;
		switch (start_bound) {
		case SQL_PLAN_GT: seek_op = OP_SeekGT; break;
		case SQL_PLAN_GE: seek_op = OP_SeekGE; break;
		case SQL_PLAN_LT: seek_op = OP_SeekLT; break;
		case SQL_PLAN_LE: seek_op = OP_SeekLE; break;
		default: goto error;
		}
		rewind = sqlVdbeAddOp4Int(vdbe, seek_op, cursor, 0, key_reg, 1);
	}
	if (rewind != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	int body = sqlVdbeCurrentAddr(vdbe);
	int range_break = -1;
	if (has_range_guard) {
		int column = sqlVdbeAddOp3(vdbe, OP_Column, cursor,
					   input->access.range_key_column,
					   current_reg);
		if (column != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
		int check_op;
		if (upper_only_ascending)
			check_op = input->access.integer_range_op == SQL_PLAN_LT ?
				OP_Le : OP_Lt;
		else if (input->access.direction == SQL_PLAN_ASC)
			check_op = (bounded_range ?
				input->access.integer_range_end_op :
				input->access.integer_range_op) == SQL_PLAN_LT ?
				OP_Le : OP_Lt;
		else
			check_op = input->access.integer_range_op == SQL_PLAN_GT ?
				OP_Ge : OP_Gt;
		range_break = sqlVdbeAddOp3(vdbe, check_op, current_reg, 0,
					    end_reg);
		if (range_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	int filter_breaks[SQL_PLAN_FILTER_MAX];
	for (size_t i = 0; i < input->filter_count; ++i) {
		filter_breaks[i] = sql_plan_emit_filter(&input->filters[i], vdbe,
			parse, &checkpoint, cursor, filter_reg, projector,
			projector_ctx);
		if (filter_breaks[i] < 0)
			goto error;
	}
	int offset_skip = -1;
	if (offset_reg != 0) {
		offset_skip = sqlVdbeAddOp2(vdbe, OP_IfNotZero, offset_reg, 0);
		if (offset_skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	for (size_t i = 0; i < input->projection_column_count; i++) {
		if (sql_plan_emit_projection(input, vdbe, parse, &checkpoint, cursor,
					     i, result_first_reg + (int)i,
					     projector, projector_ctx) != 0)
			goto error;
	}
	int result = sqlVdbeAddOp2(vdbe, OP_ResultRow, result_first_reg,
				    (int)input->projection_column_count);
	if (result != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	int limit_break = -1;
	if (has_limit) {
		limit_break = sqlVdbeAddOp2(vdbe, OP_DecrJumpZero, limit_reg, 0);
		if (limit_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	if (offset_skip >= 0)
		sqlVdbeJumpHere(vdbe, offset_skip);
	/* A rejected row must continue the cursor loop, not terminate it. */
	for (size_t i = 0; i < input->filter_count; ++i)
		sqlVdbeJumpHere(vdbe, filter_breaks[i]);
	int next = sqlVdbeAddOp2(vdbe, step_op, cursor, body);
	if (next != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	sqlVdbeJumpHere(vdbe, rewind);
	if (range_break >= 0)
		sqlVdbeJumpHere(vdbe, range_break);
	if (limit_break >= 0)
		sqlVdbeJumpHere(vdbe, limit_break);
	vdbe_codegen_checkpoint_commit(&checkpoint);
	return 0;
error:
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	return -1;
}

int
sql_plan_lower_vdbe_table_scan(const struct sql_plan_descriptor *plan,
			       struct Vdbe *vdbe, int cursor,
			       int result_first_reg)
{
	return sql_plan_lower_vdbe_table_scan_with_projector(plan, vdbe,
		cursor, result_first_reg, NULL, NULL);
}

int
sql_plan_lower_vdbe_table_scan_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe, int cursor,
	int result_first_reg, sql_plan_projection_projector_f projector,
	void *projector_ctx)
{
	return sql_plan_lower_vdbe_scan(plan, vdbe, cursor, result_first_reg,
					false, projector, projector_ctx);
}

int
sql_plan_lower_vdbe_pk_range(const struct sql_plan_descriptor *plan,
			     struct Vdbe *vdbe, int cursor,
			     int result_first_reg)
{
	return sql_plan_lower_vdbe_pk_range_with_projector(plan, vdbe,
		cursor, result_first_reg, NULL, NULL);
}

int
sql_plan_lower_vdbe_pk_range_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe, int cursor,
	int result_first_reg, sql_plan_projection_projector_f projector,
	void *projector_ctx)
{
	return sql_plan_lower_vdbe_scan(plan, vdbe, cursor, result_first_reg,
					true, projector, projector_ctx);
}

int
sql_plan_lower_vdbe_secondary_scan_with_projector(
	const struct sql_plan_descriptor *plan, struct Vdbe *vdbe,
	int table_cursor, int index_cursor,
	const struct sql_plan_secondary_index *index, int result_first_reg,
	sql_plan_projection_projector_f projector, void *projector_ctx)
{
	if (plan == NULL || vdbe == NULL || vdbe->pParse == NULL ||
	    table_cursor < 0 || index_cursor < 0 || table_cursor == index_cursor ||
	    index == NULL || index->index_id == 0 ||
	    index->primary_key_columns == NULL || index->primary_key_count == 0 ||
	    index->primary_key_count > SQL_PLAN_POINT_KEY_PART_MAX ||
	    result_first_reg < 1 ||
	    vdbe->magic != VDBE_MAGIC_INIT)
		return -1;
	const struct sql_plan_descriptor_input *input =
		sql_plan_descriptor_get_input(plan);
	bool full = input != NULL &&
		input->access.kind == SQL_PLAN_INDEX_FULL_SCAN &&
		input->access.index_id != 0;
	bool prefix_scan = input != NULL &&
		input->access.kind == SQL_PLAN_INDEX_PREFIX_SCAN;
	bool range = input != NULL &&
		input->access.kind == SQL_PLAN_INDEX_RANGE_SCAN;
	bool bounded_range = range &&
		(input->access.has_integer_range_end_key ||
		 input->access.has_unsigned_range_end_key);
	bool upper_only = range && !bounded_range &&
		(input->access.integer_range_op == SQL_PLAN_LT ||
		 input->access.integer_range_op == SQL_PLAN_LE);
	bool lower_only = range && !bounded_range &&
		(input->access.integer_range_op == SQL_PLAN_GT ||
		 input->access.integer_range_op == SQL_PLAN_GE);
	size_t prefix_count = range || prefix_scan ?
		input->access.prefix_key_part_count : 0;
	size_t index_part_count = index->key_part_count;
	size_t range_part = prefix_count;
	bool index_descending = (range || prefix_scan) &&
		index->key_parts_descending != NULL &&
		range_part < index_part_count &&
		index->key_parts_descending[range_part];
	bool logical_descending = index_descending !=
		(input->access.direction == SQL_PLAN_DESC);
	bool upper_only_ascending = upper_only && !logical_descending;
	bool lower_only_descending = lower_only && logical_descending;
	bool one_sided_guarded_walk = upper_only_ascending ||
		lower_only_descending;
	bool bounded_reverse = bounded_range && logical_descending;
	size_t key_part_count = prefix_scan ? prefix_count : range ?
		prefix_count + (one_sided_guarded_walk ? 0 : 1) :
		(index_part_count == 0 ? 1 : index_part_count);
	if (key_part_count > SQL_PLAN_POINT_KEY_PART_MAX ||
	    key_part_count > INT_MAX ||
	    (index_part_count > 0 &&
		     (index->key_columns == NULL ||
		      index->key_parts_unsigned == NULL)))
		return -1;
	bool invalid_range_order = range &&
		input->access.produced_order_count != 0 &&
		(input->access.produced_order == NULL ||
		 input->access.produced_order[0].column !=
			input->access.range_key_column ||
		 (input->access.produced_order_count > 1 &&
		  (index_part_count == 0 ||
		   index->key_parts_descending == NULL ||
		   input->access.produced_order_count >
			index_part_count - range_part)));
	bool invalid_range = range &&
		((prefix_count != 0 &&
		  (index_part_count <= prefix_count ||
		   index->key_columns == NULL ||
		   index->key_parts_unsigned == NULL)) ||
		 input->access.point_key_part_count != 0 ||
		 (prefix_count != 0 && input->access.prefix_key_parts == NULL) ||
		 input->access.has_integer_point_key ||
		 input->access.has_unsigned_point_key ||
		 input->access.range_key_column !=
			(prefix_count == 0 ? index->key_column :
			 index->key_columns[prefix_count]) ||
		 input->access.has_integer_range_key ==
		 input->access.has_unsigned_range_key ||
		 input->access.has_unsigned_range_key !=
			(prefix_count == 0 ? index->key_unsigned :
			 index->key_parts_unsigned[prefix_count]) ||
		 input->access.bound_count != prefix_count +
			(bounded_range ? 2 : 1) ||
		 invalid_range_order ||
		 (!bounded_range && !one_sided_guarded_walk &&
		  (index_descending !=
		   (input->access.direction == SQL_PLAN_DESC)) != upper_only));
	if (range && !invalid_range &&
	    input->access.produced_order_count != 0) {
		for (size_t i = 0; i < input->access.produced_order_count; ++i) {
			size_t part = range_part + i;
			bool part_descending = index_part_count != 0 &&
				index->key_parts_descending != NULL &&
				index->key_parts_descending[part];
			bool descending = part_descending !=
				(input->access.direction == SQL_PLAN_DESC);
			uint32_t column = index_part_count == 0 ? index->key_column :
				index->key_columns[part];
			if (input->access.produced_order[i].column != column ||
			    (input->access.produced_order[i].direction == SQL_PLAN_DESC) !=
				descending) {
				invalid_range = true;
				break;
			}
		}
	}
	bool invalid_prefix = prefix_scan &&
		(input->access.index_id != index->index_id || prefix_count == 0 ||
		 prefix_count >= index_part_count ||
		 index->key_columns == NULL || index->key_parts_unsigned == NULL ||
		 input->access.prefix_key_parts == NULL ||
		 input->access.point_key_part_count != 0 ||
		 input->access.has_integer_point_key ||
		 input->access.has_unsigned_point_key ||
		 input->access.has_integer_range_key ||
		 input->access.has_unsigned_range_key ||
		 input->access.has_integer_range_end_key ||
		 input->access.has_unsigned_range_end_key ||
		 input->access.bound_count != prefix_count ||
		 input->access.bounds == NULL ||
		 input->access.produced_order_count >
			index_part_count - prefix_count ||
		 (input->access.produced_order_count != 0 &&
		  (input->access.produced_order == NULL ||
		   index->key_parts_descending == NULL ||
		   input->access.range_key_column !=
			index->key_columns[prefix_count])));
	if (prefix_scan && !invalid_prefix &&
	    input->access.produced_order_count != 0) {
		for (size_t i = 0; i < input->access.produced_order_count; ++i) {
			size_t part = prefix_count + i;
			bool descending = index->key_parts_descending[part] !=
				(input->access.direction == SQL_PLAN_DESC);
			if (input->access.produced_order[i].column !=
				index->key_columns[part] ||
			    (input->access.produced_order[i].direction == SQL_PLAN_DESC) !=
				descending) {
				invalid_prefix = true;
				break;
			}
		}
	}
	if (prefix_scan && !invalid_prefix) {
		for (size_t i = 0; i < prefix_count; ++i) {
			if (input->access.prefix_key_parts[i].column !=
				index->key_columns[i] ||
			    input->access.prefix_key_parts[i].is_unsigned !=
				index->key_parts_unsigned[i] ||
			    input->access.bounds[i].op != SQL_PLAN_EQ ||
			    input->access.bounds[i].side != SQL_PLAN_LOWER) {
				invalid_prefix = true;
				break;
			}
		}
	}
	if (range && prefix_count != 0 && !invalid_range) {
		for (size_t i = 0; i < prefix_count; ++i) {
			if (input->access.prefix_key_parts[i].column !=
				index->key_columns[i] ||
			    input->access.prefix_key_parts[i].is_unsigned !=
				index->key_parts_unsigned[i]) {
				invalid_range = true;
				break;
			}
		}
	}
	bool invalid_full = full &&
		(index->key_part_count == 0 || index->key_columns == NULL ||
		 index->key_parts_descending == NULL ||
		 index->key_columns[0] != index->key_column ||
		 input->access.bound_count != 0 ||
		 input->access.point_key_part_count != 0 ||
		 input->access.has_integer_point_key ||
		 input->access.has_unsigned_point_key ||
		 input->access.has_integer_range_key ||
		 input->access.has_unsigned_range_key ||
		 input->access.has_integer_range_end_key ||
		 input->access.has_unsigned_range_end_key ||
		 input->access.produced_order_count == 0 ||
		 input->access.produced_order_count > index->key_part_count ||
		 input->access.produced_order == NULL ||
		 input->access.direction > SQL_PLAN_DESC);
	bool invalid_equality = input == NULL ||
		input->access.kind != SQL_PLAN_INDEX_EQUALITY_SCAN ||
		input->access.range_key_column != (index->key_part_count == 0 ?
			index->key_column : index->key_columns[0]) ||
		(input->access.point_key_part_count == 0 &&
		 (input->access.has_integer_point_key ==
		  input->access.has_unsigned_point_key ||
		  input->access.has_unsigned_point_key != index->key_unsigned)) ||
		input->access.point_key_part_count !=
			(index->key_part_count == 0 ? 0 : index->key_part_count) ||
		input->access.bound_count != key_part_count;
	if (input == NULL || input->path_class != SQL_PLAN_NEW_PLANNER ||
	    (full ? invalid_full : prefix_scan ? invalid_prefix :
	     range ? invalid_range : invalid_equality) ||
	    input->access.index_id != index->index_id ||
	    (full ? input->access.bound_count != 0 :
	     input->access.bounds == NULL) ||
	    input->filter_count > SQL_PLAN_FILTER_MAX ||
	    (input->filter_count != 0 && input->filters == NULL) ||
	    input->finalize_count > 1 ||
	    (input->finalize_count == 1 &&
	     (input->finalize == NULL ||
	      input->finalize[0].kind != SQL_PLAN_LIMIT ||
	      input->finalize[0].limit > INT64_MAX ||
	      input->finalize[0].offset > INT64_MAX)) ||
	    input->projection_columns == NULL ||
	    input->projection_column_count == 0 ||
	    input->projection_column_count > INT_MAX ||
	    result_first_reg > INT_MAX - (int)input->projection_column_count + 1)
		return -1;
	if (full) {
		for (size_t i = 0; i < input->access.produced_order_count; ++i) {
			bool effective_descending =
				index->key_parts_descending[i] !=
				(input->access.direction == SQL_PLAN_DESC);
			if (input->access.produced_order[i].column !=
				index->key_columns[i] ||
			    (input->access.produced_order[i].direction ==
				SQL_PLAN_DESC) != effective_descending)
				return -1;
		}
	}
	for (size_t i = 0; i < input->access.bound_count; ++i) {
		if (range || full || prefix_scan)
			continue;
		if (input->access.bounds[i].op != SQL_PLAN_EQ ||
		    input->access.bounds[i].side != SQL_PLAN_LOWER)
			return -1;
		if (index->key_part_count != 0 &&
		    (input->access.point_key_parts[i].column !=
		     index->key_columns[i] ||
		     input->access.point_key_parts[i].is_unsigned !=
		     index->key_parts_unsigned[i]))
			return -1;
	}
	for (size_t i = 0; i < index->primary_key_count; ++i)
		if (index->primary_key_columns[i] > INT_MAX)
			return -1;
	for (size_t i = 0; i < input->filter_count; ++i)
		if (!sql_plan_filter_is_valid(&input->filters[i], projector))
			return -1;
	for (size_t i = 0; i < input->projection_column_count; ++i) {
		uint32_t expr_ref = input->projection_expr_refs == NULL ? 0 :
			input->projection_expr_refs[i];
		if ((expr_ref == 0 && input->projection_columns[i] > INT_MAX) ||
		    (expr_ref != 0 && projector == NULL))
			return -1;
	}
	Parse *parse = vdbe->pParse;
	bool has_limit = input->finalize_count == 1;
	bool has_offset = has_limit && input->finalize[0].offset != 0;
	if (has_limit && input->finalize[0].limit == 0) {
		struct vdbe_codegen_checkpoint checkpoint;
		if (vdbe_codegen_checkpoint_init(&checkpoint, vdbe) != 0)
			return -1;
		int skip = sqlVdbeAddOp2(vdbe, OP_Goto, 0, 0);
		if (skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error) {
			vdbe_codegen_checkpoint_rollback(&checkpoint);
			return -1;
		}
		sqlVdbeJumpHere(vdbe, skip);
		vdbe_codegen_checkpoint_commit(&checkpoint);
		return 0;
	}
	int key_register_count = full ? 0 : (int)key_part_count;
	int extra_regs = key_register_count +
		(bounded_range ? 1 : 0) +
		(one_sided_guarded_walk ? 1 : 0) +
		((bounded_range || upper_only || lower_only_descending) ? 1 : 0) +
		(int)index->primary_key_count +
		(int)input->filter_count + (has_limit ? 1 : 0) +
		(has_offset ? 1 : 0);
	if (parse->nMem > INT_MAX - extra_regs)
		return -1;
	struct vdbe_codegen_checkpoint checkpoint;
	if (vdbe_codegen_checkpoint_init(&checkpoint, vdbe) != 0)
		return -1;
	int key_reg = full ? 0 : parse->nMem + 1;
	parse->nMem += key_register_count;
	int range_end_reg = bounded_range ? ++parse->nMem : 0;
	int range_bound_reg = one_sided_guarded_walk ? ++parse->nMem : 0;
	int range_current_reg = bounded_range || upper_only ||
		lower_only_descending ?
		++parse->nMem : 0;
	int pk_reg = parse->nMem + 1;
	parse->nMem += (int)index->primary_key_count;
	int filter_reg = input->filter_count == 0 ? 0 : parse->nMem + 1;
	parse->nMem += (int)input->filter_count;
	int limit_reg = has_limit ? ++parse->nMem : 0;
	int offset_reg = has_offset ? ++parse->nMem : 0;
	int rc = 0;
	for (size_t i = 0; !full && i < key_part_count; ++i) {
		bool is_prefix_part = (range || prefix_scan) && prefix_count != 0 &&
			i < prefix_count;
		bool is_unsigned = is_prefix_part ?
			input->access.prefix_key_parts[i].is_unsigned :
			input->access.point_key_part_count != 0 ?
			input->access.point_key_parts[i].is_unsigned :
			range ? input->access.has_unsigned_range_key :
			input->access.has_unsigned_point_key;
		int64_t signed_key = is_prefix_part ?
			input->access.prefix_key_parts[i].integer_value :
			input->access.point_key_part_count != 0 ?
			input->access.point_key_parts[i].integer_value :
			range ? bounded_reverse ?
				input->access.integer_range_end_key :
				input->access.integer_range_key :
			input->access.integer_point_key;
		uint64_t unsigned_key = is_prefix_part ?
			input->access.prefix_key_parts[i].unsigned_value :
			input->access.point_key_part_count != 0 ?
			input->access.point_key_parts[i].unsigned_value :
			range ? bounded_reverse ?
				input->access.unsigned_range_end_key :
				input->access.unsigned_range_key :
			input->access.unsigned_point_key;
		rc = sql_plan_emit_integer_constant(vdbe, key_reg + (int)i,
			is_unsigned, signed_key, unsigned_key);
		if (rc != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	if (bounded_range) {
		bool is_unsigned = bounded_reverse ?
			input->access.has_unsigned_range_key :
			input->access.has_unsigned_range_end_key;
		int64_t signed_key = bounded_reverse ?
			input->access.integer_range_key :
			input->access.integer_range_end_key;
		uint64_t unsigned_key = bounded_reverse ?
			input->access.unsigned_range_key :
			input->access.unsigned_range_end_key;
		rc = sql_plan_emit_integer_constant(vdbe, range_end_reg,
			is_unsigned, signed_key, unsigned_key);
		if (rc != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	if (one_sided_guarded_walk) {
		int rc = sql_plan_emit_integer_constant(vdbe, range_bound_reg,
			input->access.has_unsigned_range_key,
			input->access.integer_range_key,
			input->access.unsigned_range_key);
		if (rc != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	if (has_limit) {
		uint64_t value = input->finalize[0].limit;
		rc = value <= INT_MAX ?
			sqlVdbeAddOp2(vdbe, OP_Integer, (int)value, limit_reg) :
			sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, limit_reg, 0,
					  (const u8 *)&value, P4_UINT64);
		if (rc != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	if (has_offset) {
		uint64_t value = input->finalize[0].offset;
		rc = value <= INT_MAX ?
			sqlVdbeAddOp2(vdbe, OP_Integer, (int)value, offset_reg) :
			sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, offset_reg, 0,
					  (const u8 *)&value, P4_UINT64);
		if (rc != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	int seek_op = full ?
		(input->access.direction == SQL_PLAN_DESC ? OP_Last : OP_Rewind) :
		(prefix_scan && input->access.direction == SQL_PLAN_DESC ?
		 OP_SeekLE : OP_SeekGE);
	if (range && one_sided_guarded_walk) {
		if (input->access.direction == SQL_PLAN_DESC)
			seek_op = prefix_count == 0 ? OP_Last : OP_SeekLE;
		else
			seek_op = prefix_count == 0 ? OP_Rewind : OP_SeekGE;
	} else if (range) {
		switch (input->access.integer_range_op) {
		case SQL_PLAN_GT: seek_op = OP_SeekGT; break;
		case SQL_PLAN_GE: seek_op = OP_SeekGE; break;
		case SQL_PLAN_LT: seek_op = OP_SeekLT; break;
		case SQL_PLAN_LE: seek_op = OP_SeekLE; break;
		default: goto secondary_error;
		}
		if (bounded_reverse) {
			switch (input->access.integer_range_end_op) {
			case SQL_PLAN_LT: seek_op = OP_SeekLT; break;
			case SQL_PLAN_LE: seek_op = OP_SeekLE; break;
			default: goto secondary_error;
			}
		}
		if (index_descending) {
			switch (seek_op) {
			case OP_SeekGT: seek_op = OP_SeekLT; break;
			case OP_SeekGE: seek_op = OP_SeekLE; break;
			case OP_SeekLT: seek_op = OP_SeekGT; break;
			case OP_SeekLE: seek_op = OP_SeekGE; break;
			default: goto secondary_error;
			}
		}
	}
	int seek = full || (range && one_sided_guarded_walk &&
			     prefix_count == 0) ?
		sqlVdbeAddOp2(vdbe, seek_op, index_cursor, 0) :
		sqlVdbeAddOp4Int(vdbe, seek_op, index_cursor, 0, key_reg,
				 (int)key_part_count);
	if (seek != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto secondary_error;
	int body = sqlVdbeCurrentAddr(vdbe);
	int end = -1;
	int prefix_end = -1;
	int range_break = -1;
	int range_null_break = -1;
	int next_label = sqlVdbeMakeLabel(vdbe);
	if (full) {
		/* An index full scan has no key guard. */
	} else if (!range && !prefix_scan) {
		end = sqlVdbeAddOp4Int(vdbe, OP_IdxGT, index_cursor, 0,
				       key_reg, (int)key_part_count);
		if (end != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	} else if (prefix_scan || prefix_count != 0) {
		int prefix_op = input->access.direction == SQL_PLAN_DESC ?
			OP_IdxLT : OP_IdxGT;
		prefix_end = sqlVdbeAddOp4Int(vdbe, prefix_op, index_cursor, 0,
					      key_reg, (int)prefix_count);
		if (prefix_end != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	if (range && bounded_range) {
		int column = sqlVdbeAddOp3(vdbe, OP_Column, index_cursor,
					   (int)input->access.range_key_column,
					   range_current_reg);
		if (column != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
		range_null_break = sqlVdbeAddOp2(vdbe, OP_IsNull,
						 range_current_reg, 0);
		if (range_null_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
		int check_op;
		if (bounded_reverse) {
			check_op = input->access.integer_range_op == SQL_PLAN_GT ?
				OP_Ge : OP_Gt;
		} else {
			check_op = input->access.integer_range_end_op ==
				SQL_PLAN_LT ? OP_Le : OP_Lt;
		}
		range_break = sqlVdbeAddOp3(vdbe, check_op, range_current_reg, 0,
					     range_end_reg);
		if (range_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	} else if (range && one_sided_guarded_walk) {
		int column = sqlVdbeAddOp3(vdbe, OP_Column, index_cursor,
					   (int)input->access.range_key_column,
					   range_current_reg);
		if (column != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
		range_null_break = sqlVdbeAddOp2(vdbe, OP_IsNull,
						 range_current_reg,
						 upper_only_ascending ? next_label : 0);
		if (range_null_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
		int check_op;
		if (upper_only_ascending) {
			check_op = input->access.integer_range_op == SQL_PLAN_LT ?
				OP_Ge : OP_Gt;
		} else {
			check_op = input->access.integer_range_op == SQL_PLAN_GT ?
				OP_Le : OP_Lt;
		}
		range_break = sqlVdbeAddOp3(vdbe, check_op, range_bound_reg, 0,
					      range_current_reg);
		if (range_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	} else if (range && upper_only) {
		int column = sqlVdbeAddOp3(vdbe, OP_Column, index_cursor,
					   (int)input->access.range_key_column,
					   range_current_reg);
		if (column != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
		range_break = sqlVdbeAddOp2(vdbe, OP_IsNull,
					     range_current_reg, 0);
		if (range_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	for (size_t i = 0; i < index->primary_key_count; ++i) {
		rc = sqlVdbeAddOp3(vdbe, OP_Column, index_cursor,
				   (int)index->primary_key_columns[i],
				   pk_reg + (int)i);
		if (rc != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	int table_miss = sqlVdbeAddOp4Int(vdbe, OP_NotFound, table_cursor, 0,
					   pk_reg,
					   (int)index->primary_key_count);
	if (table_miss != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto secondary_error;
	int filter_breaks[SQL_PLAN_FILTER_MAX];
	for (size_t i = 0; i < input->filter_count; ++i) {
		filter_breaks[i] = sql_plan_emit_filter(&input->filters[i], vdbe,
			parse, &checkpoint, table_cursor, filter_reg + (int)i, projector,
			projector_ctx);
		if (filter_breaks[i] < 0)
			goto secondary_error;
	}
	int offset_skip = -1;
	if (has_offset) {
		offset_skip = sqlVdbeAddOp2(vdbe, OP_IfNotZero, offset_reg, 0);
		if (offset_skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	for (size_t i = 0; i < input->projection_column_count; ++i) {
		if (sql_plan_emit_projection(input, vdbe, parse, &checkpoint,
					     table_cursor, i,
					     result_first_reg + (int)i,
					     projector, projector_ctx) != 0)
			goto secondary_error;
	}
	rc = sqlVdbeAddOp2(vdbe, OP_ResultRow, result_first_reg,
			   (int)input->projection_column_count);
	if (rc != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto secondary_error;
	int limit_break = -1;
	if (has_limit) {
		limit_break = sqlVdbeAddOp2(vdbe, OP_DecrJumpZero, limit_reg, 0);
		if (limit_break != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto secondary_error;
	}
	if (offset_skip >= 0)
		sqlVdbeJumpHere(vdbe, offset_skip);
	sqlVdbeResolveLabel(vdbe, next_label);
	int step = input->access.direction == SQL_PLAN_DESC ? OP_Prev : OP_Next;
	int next = sqlVdbeAddOp2(vdbe, step, index_cursor, body);
	if (next != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto secondary_error;
	sqlVdbeJumpHere(vdbe, seek);
	if (end >= 0)
		sqlVdbeJumpHere(vdbe, end);
	if (prefix_end >= 0)
		sqlVdbeJumpHere(vdbe, prefix_end);
	if (range_break >= 0)
		sqlVdbeJumpHere(vdbe, range_break);
	if (range_null_break >= 0 && !upper_only_ascending)
		sqlVdbeJumpHere(vdbe, range_null_break);
	sqlVdbeChangeP2(vdbe, table_miss, next_label);
	for (size_t i = 0; i < input->filter_count; ++i)
		sqlVdbeChangeP2(vdbe, filter_breaks[i], next_label);
	if (limit_break >= 0)
		sqlVdbeJumpHere(vdbe, limit_break);
	vdbe_codegen_checkpoint_commit(&checkpoint);
	return 0;
secondary_error:
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	return -1;
}

int
sql_plan_lower_vdbe_pk_point(const struct sql_plan_descriptor *plan,
			     struct Vdbe *vdbe, int cursor, int result_first_reg)
{
	return sql_plan_lower_vdbe_pk_point_with_projector(plan, vdbe, cursor,
							result_first_reg,
							NULL, NULL);
}

int
sql_plan_lower_vdbe_pk_prefix_scan(const struct sql_plan_descriptor *plan,
				   struct Vdbe *vdbe, int cursor,
				   int result_first_reg)
{
	return sql_plan_lower_vdbe_pk_prefix_scan_with_projector(plan, vdbe,
		cursor, result_first_reg, NULL, NULL);
}
