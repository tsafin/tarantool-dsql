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
/* Comparison opcodes (STUBS - see vdbe_ops_compare.c for extraction plan) */
int vdbe_op_eq(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_ne(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_lt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_le(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_gt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_ge(Vdbe *p, Op *pOp, Mem *aMem);

#endif /* SRC_BOX_SQL_VDBE_OPS_H */
