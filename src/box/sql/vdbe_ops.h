/* Header declaring extracted VDBE opcode handlers */
#ifndef SRC_BOX_SQL_VDBE_OPS_H
#define SRC_BOX_SQL_VDBE_OPS_H

#include "vdbeInt.h"

/* Handler prototypes return int (0 on success, non-zero on error).
 * Implementations live in vdbe_ops_*.c files. */
int vdbe_op_noop(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_add(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_sub(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_jump(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_multiply(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_divide(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_remainder(Vdbe *p, Op *pOp, Mem *aMem);
/* Data / constant opcodes */
int vdbe_op_integer(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bool(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_int64(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_real(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_string(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_null(Vdbe *p, Op *pOp, Mem *aMem);
/* Blob / variable / copy/move opcodes */
int vdbe_op_blob(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_variable(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_move(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_copy(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_scopy(Vdbe *p, Op *pOp, Mem *aMem);
/* Comparison opcodes (FUNCTIONAL - extracted after adding iCompare to Vdbe struct) */
int vdbe_op_eq(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_ne(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_lt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_le(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_gt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_ge(Vdbe *p, Op *pOp, Mem *aMem);
/* Logical and bitwise opcodes */
int vdbe_op_and(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_or(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_not(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitand(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitor(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitnot(Vdbe *p, Op *pOp, Mem *aMem);
/* String opcodes */
int vdbe_op_concat(Vdbe *p, Op *pOp, Mem *aMem);
/* Control flow opcodes - reserved for dispatcher refactoring
 * See vdbe_ops_control.c for extraction plan.
 * These remain in vdbe.c for now due to PC manipulation complexity. */

#endif /* SRC_BOX_SQL_VDBE_OPS_H */
