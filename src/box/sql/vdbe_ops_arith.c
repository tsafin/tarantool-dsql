/* Simple VDBE opcode handler stubs (temporary) */
#include "generated/vdbe_opcodes_generated.h"
#include "mem.h"

/* No-op handler */
void vdbe_op_noop(struct Vdbe *p, struct Op *pOp, struct Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}

/* Arithmetic add: placeholder implementation */
void vdbe_op_add(struct Vdbe *p, struct Op *pOp, struct Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}

/* Arithmetic sub: placeholder implementation */
void vdbe_op_sub(struct Vdbe *p, struct Op *pOp, struct Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}

/* Jump handler placeholder */
void vdbe_op_jump(struct Vdbe *p, struct Op *pOp, struct Mem *aMem)
{
    (void)p; (void)pOp; (void)aMem;
}
