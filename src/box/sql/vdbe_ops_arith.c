/* Simple VDBE opcode handler stubs (temporary) */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"

/* No-op handler */
int vdbe_op_noop(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
    return 0;
}

/* Arithmetic add implementation */
int vdbe_op_add(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p;
    Mem *pIn1 = &aMem[pOp->p1];
    Mem *pIn2 = &aMem[pOp->p2];
    Mem *pOut = &aMem[pOp->p3];
    if (mem_add(pIn2, pIn1, pOut) != 0)
        return -1;
    return 0;
}

/* Arithmetic sub implementation */
int vdbe_op_sub(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p;
    Mem *pIn1 = &aMem[pOp->p1];
    Mem *pIn2 = &aMem[pOp->p2];
    Mem *pOut = &aMem[pOp->p3];
    if (mem_sub(pIn2, pIn1, pOut) != 0)
        return -1;
    return 0;
}

/* Jump handler placeholder */
int vdbe_op_jump(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
    return 0;
}
