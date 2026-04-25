/* LIMIT and OFFSET opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_ops_cnp_impl.h"

/* Opcode: OffsetLimit P1 P2 P3 * *
 * Synopsis: r[P2]=r[P1]+r[P3]
 *
 * This opcode performs a commonly used computation associated with
 * LIMIT and OFFSET process.  r[P1] holds the limit counter.  r[P3]
 * holds the offset counter.  The opcode computes the combined value
 * of the LIMIT and OFFSET and stores that value in r[P2].  The r[P2]
 * value computed is the total number of rows that will need to be
 * visited in order to complete the query.
 *
 * Otherwise, r[P2] is set to the sum of r[P1] and r[P3]. If the
 * sum is larger than 2^63-1 (i.e. overflow takes place) then
 * error is raised.
 */
int vdbe_op_offsetlimit(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_offsetlimit_impl(p, pOp, aMem);
}
