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

#endif /* SRC_BOX_SQL_VDBE_OPS_H */
