#include "sql_plan_lowering.h"

#include <limits.h>

#include "sqlInt.h"
#include "core/diag.h"
#include "opcodes.h"
#include "vdbe.h"
#include "vdbeInt.h"

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
	    input->filter_count != 0 || input->finalize_count != 0 ||
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
	int start = vdbe->nOp;
	int rewind_op = input->access.direction == SQL_PLAN_DESC ? OP_Last :
		OP_Rewind;
	int step_op = input->access.direction == SQL_PLAN_DESC ? OP_Prev :
		OP_Next;
	int rewind = sqlVdbeAddOp2(vdbe, rewind_op, cursor, 0);
	if (vdbe->nOp != start + 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	int body = sqlVdbeCurrentAddr(vdbe);
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
	int next = sqlVdbeAddOp2(vdbe, step_op, cursor, body);
	if (next != vdbe->nOp - 1 || parse->is_aborted ||
	    diag_last_error(diag_get()) != checkpoint.diag_error)
		goto error;
	sqlVdbeJumpHere(vdbe, rewind);
	vdbe_codegen_checkpoint_commit(&checkpoint);
	return 0;
error:
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	return -1;
}
