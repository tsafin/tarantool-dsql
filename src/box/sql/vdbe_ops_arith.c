/* Simple VDBE opcode handler stubs (temporary) */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"

/* No-op handler */
void vdbe_op_noop(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}

/* Arithmetic add: placeholder implementation */
void vdbe_op_add(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}

/* Arithmetic sub: placeholder implementation */
void vdbe_op_sub(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}

/* Jump handler placeholder */
void vdbe_op_jump(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}
