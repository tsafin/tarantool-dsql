/* LIMIT and OFFSET opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"

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
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);

	assert(mem_is_uint(pIn1));
	assert(mem_is_uint(pIn3));
	uint64_t x = pIn1->u.u;
	uint64_t rhs = pIn3->u.u;
	bool unused;
	if (sql_add_int(x, false, rhs, false, (int64_t *) &x, &unused) != 0) {
		diag_set(ClientError, ER_SQL_EXECUTE, "sum of LIMIT and OFFSET "
			"values should not result in integer overflow");
		return -1;
	}
	mem_set_uint(pOut, x);
	return 0;
}
