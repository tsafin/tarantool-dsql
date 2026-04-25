/* String opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_ops_cnp_impl.h"
#include "vdbe_debug.h"

/* Opcode: Concat P1 P2 P3 * *
 * Synopsis: r[P3]=r[P2]+r[P1]
 *
 * Add the text in register P1 onto the end of the text in
 * register P2 and store the result in register P3.
 * If either the P1 or P2 text are NULL then store NULL in P3.
 *
 *   P3 = P2 || P1
 *
 * It is illegal for P1 and P3 to be the same register. Sometimes,
 * if P3 is the same register as P2, the implementation is able
 * to avoid a memcpy().
 *
 * Concatenation operator accepts only arguments of string-like
 * types (i.e. TEXT and BLOB).
 */
int vdbe_op_concat(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_concat_impl(p, pOp, aMem);
}
