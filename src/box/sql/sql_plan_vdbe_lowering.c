#include "sql_plan_lowering.h"

#include <limits.h>
#include <stdint.h>

#include "sqlInt.h"
#include "core/diag.h"
#include "opcodes.h"
#include "vdbe.h"
#include "vdbeInt.h"

int
sql_plan_lower_vdbe_pk_point(const struct sql_plan_descriptor *plan,
			     struct Vdbe *vdbe, int cursor,
			     int result_first_reg)
{
	if (plan == NULL || vdbe == NULL || vdbe->pParse == NULL || cursor < 0 ||
	    result_first_reg < 1 || vdbe->magic != VDBE_MAGIC_INIT)
		return -1;
	const struct sql_plan_descriptor_input *input =
		sql_plan_descriptor_get_input(plan);
	if (input == NULL || input->path_class != SQL_PLAN_NEW_PLANNER ||
	    input->access.kind != SQL_PLAN_PK_POINT_LOOKUP ||
	    (input->access.has_integer_point_key ==
	     input->access.has_unsigned_point_key) || input->filter_count != 0 ||
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
	Parse *parse = vdbe->pParse;
	struct vdbe_codegen_checkpoint checkpoint;
	if (vdbe_codegen_checkpoint_init(&checkpoint, vdbe) != 0)
		return -1;
	if (input->finalize_count == 1 &&
	    (input->finalize[0].limit == 0 || input->finalize[0].offset != 0)) {
		int skip = sqlVdbeAddOp2(vdbe, OP_Goto, 0, 0);
		if (skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
		sqlVdbeJumpHere(vdbe, skip);
		vdbe_codegen_checkpoint_commit(&checkpoint);
		return 0;
	}
	int key_reg = ++parse->nMem;
	int key_op;
	if (input->access.has_unsigned_point_key) {
		uint64_t key = input->access.unsigned_point_key;
		if (key <= INT_MAX) {
			key_op = sqlVdbeAddOp2(vdbe, OP_Integer, (int)key, key_reg);
		} else {
			key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, key_reg, 0,
					   (const u8 *)&key, P4_UINT64);
		}
	} else {
		int64_t key = input->access.integer_point_key;
		if (key >= INT_MIN && key <= INT_MAX) {
			key_op = sqlVdbeAddOp2(vdbe, OP_Integer, (int)key, key_reg);
		} else if (key < 0) {
			key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, key_reg, 0,
					   (const u8 *)&key, P4_INT64);
		} else {
			uint64_t value = (uint64_t)key;
			key_op = sqlVdbeAddOp4Dup8(vdbe, OP_Int64, 0, key_reg, 0,
					   (const u8 *)&value, P4_UINT64);
		}
	}
	if (key_op != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	int miss = sqlVdbeAddOp4Int(vdbe, OP_NotFound, cursor, 0, key_reg, 1);
	if (miss != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	for (size_t i = 0; i < input->projection_column_count; ++i) {
		if (input->projection_columns[i] > INT_MAX)
			goto error;
		int addr = sqlVdbeAddOp3(vdbe, OP_Column, cursor,
					 input->projection_columns[i],
					 result_first_reg + (int)i);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	int result = sqlVdbeAddOp2(vdbe, OP_ResultRow, result_first_reg,
				    (int)input->projection_column_count);
	if (result != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	sqlVdbeJumpHere(vdbe, miss);
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
	if (plan == NULL || vdbe == NULL || vdbe->pParse == NULL || cursor < 0 ||
	    result_first_reg < 1 || vdbe->magic != VDBE_MAGIC_INIT)
		return -1;
	const struct sql_plan_descriptor_input *input =
		sql_plan_descriptor_get_input(plan);
	if (input == NULL || input->path_class != SQL_PLAN_NEW_PLANNER ||
	    input->access.kind != SQL_PLAN_TABLE_FULL_SCAN ||
	    input->filter_count != 0 || input->finalize_count > 1 ||
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
	for (size_t i = 0; i < input->projection_column_count; i++) {
		if (input->projection_columns[i] > INT_MAX)
			return -1;
	}
	Parse *parse = vdbe->pParse;
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
	int rewind_op = input->access.direction == SQL_PLAN_DESC ? OP_Last :
		OP_Rewind;
	int step_op = input->access.direction == SQL_PLAN_DESC ? OP_Prev :
		OP_Next;
	int rewind = sqlVdbeAddOp2(vdbe, rewind_op, cursor, 0);
	if (rewind != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	int body = sqlVdbeCurrentAddr(vdbe);
	int offset_skip = -1;
	if (offset_reg != 0) {
		offset_skip = sqlVdbeAddOp2(vdbe, OP_IfNotZero, offset_reg, 0);
		if (offset_skip != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
			goto error;
	}
	for (size_t i = 0; i < input->projection_column_count; i++) {
		int addr = sqlVdbeAddOp3(vdbe, OP_Column, cursor,
					 input->projection_columns[i],
					 result_first_reg + (int)i);
		if (addr != vdbe->nOp - 1 || parse->is_aborted ||
		    diag_last_error(diag_get()) != checkpoint.diag_error)
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
	int next = sqlVdbeAddOp2(vdbe, step_op, cursor, body);
	if (next != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	sqlVdbeJumpHere(vdbe, rewind);
	if (limit_break >= 0)
		sqlVdbeJumpHere(vdbe, limit_break);
	vdbe_codegen_checkpoint_commit(&checkpoint);
	return 0;
error:
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	return -1;
}
