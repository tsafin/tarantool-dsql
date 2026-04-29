/* Arithmetic opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_ops_cnp_impl.h"

/* No-op handler */
int SQL_PRESERVE_NONE vdbe_op_noop(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;
	return 0;
}

/* Opcode: Add P1 P2 P3 * *
 * Synopsis: r[P3]=r[P1]+r[P2]
 *
 * Add the value in register P1 to the value in register P2
 * and store the result in register P3.
 * If either input is NULL, the result is NULL.
 */
int SQL_PRESERVE_NONE vdbe_op_add(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_add_impl(p, pOp, aMem);
}

/* Opcode: Subtract P1 P2 P3 * *
 * Synopsis: r[P3]=r[P2]-r[P1]
 *
 * Subtract the value in register P1 from the value in register P2
 * and store the result in register P3.
 * If either input is NULL, the result is NULL.
 */
int SQL_PRESERVE_NONE vdbe_op_sub(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_sub_impl(p, pOp, aMem);
}

/* Jump handler placeholder */
int SQL_PRESERVE_NONE vdbe_op_jump(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;
	return 0;
}

/* Opcode: Multiply P1 P2 P3 * *
 * Synopsis: r[P3]=r[P1]*r[P2]
 *
 *
 * Multiply the value in register P1 by the value in register P2
 * and store the result in register P3.
 * If either input is NULL, the result is NULL.
 */
int SQL_PRESERVE_NONE vdbe_op_multiply(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_multiply_impl(p, pOp, aMem);
}

/* Opcode: Divide P1 P2 P3 * *
 * Synopsis: r[P3]=r[P2]/r[P1]
 *
 * Divide the value in register P1 by the value in register P2
 * and store the result in register P3 (P3=P2/P1). If the value in
 * register P1 is zero, then the result is NULL. If either input is
 * NULL, the result is NULL.
 */
int SQL_PRESERVE_NONE vdbe_op_divide(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_divide_impl(p, pOp, aMem);
}

/* Opcode: Remainder P1 P2 P3 * *
 * Synopsis: r[P3]=r[P2]%r[P1]
 *
 * Compute the remainder after integer register P2 is divided by
 * register P1 and store the result in register P3.
 * If the value in register P1 is zero the result is NULL.
 * If either operand is NULL, the result is NULL.
 */
int SQL_PRESERVE_NONE vdbe_op_remainder(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_remainder_impl(p, pOp, aMem);
}
