/* Comparison opcode handlers extracted from vdbe.c
 *
 * These handlers implement the SQL comparison operators (Eq, Ne, Lt, Le, Gt, Ge).
 * They were successfully extracted after moving iCompare from a local variable
 * in sqlVdbeExec() to a member of struct Vdbe.
 *
 * Jump handling: These handlers don't perform jumps directly. Instead, they
 * return a special value (1) to indicate a jump should be taken. The main
 * execution loop checks the return value and performs the jump if needed.
 */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"

/* Return values for comparison handlers */
#define VDBE_CMP_CONTINUE  0   /* Continue to next instruction */
#define VDBE_CMP_JUMP      1   /* Jump to P2 */
#define VDBE_CMP_ERROR    -1   /* Error occurred */

/* OP_Eq: IF r[P3]==r[P1]
 * OP_Ne: IF r[P3]!=r[P1]
 *
 * Compare the values in register P1 and P3. If the comparison matches,
 * then jump to address P2 or store the comparison result in register P2
 * if the SQL_STOREP2 flag is set in P5.
 *
 * P5 flags:
 * - SQL_STOREP2: Store comparison result in r[P2] instead of jumping
 * - SQL_NULLEQ: NULL == NULL returns TRUE (otherwise NULL)
 * - SQL_JUMPIFNULL: Jump if any operand is NULL
 */
int vdbe_op_eq(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];

	if (mem_is_any_null(pIn1, pIn3) && (pOp->p5 & SQL_NULLEQ) == 0) {
		/* NULL comparison without NULLEQ */
		if ((pOp->p5 & SQL_STOREP2) != 0) {
			Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
			p->iCompare = 1;
			REGISTER_TRACE(p, pOp->p2, pOut);
			return VDBE_CMP_CONTINUE;
		}
		if ((pOp->p5 & SQL_JUMPIFNULL) != 0)
			return VDBE_CMP_JUMP;
		return VDBE_CMP_CONTINUE;
	}

	int cmp_res;
	if (mem_cmp(pIn3, pIn1, &cmp_res, pOp->p4.pColl) != 0)
		return VDBE_CMP_ERROR;

	bool result = (cmp_res == 0);

	if ((pOp->p5 & SQL_STOREP2) != 0) {
		p->iCompare = cmp_res;
		Mem *pOut = &aMem[pOp->p2];
		mem_set_bool(pOut, result);
		REGISTER_TRACE(p, pOp->p2, pOut);
		return VDBE_CMP_CONTINUE;
	}

	return result ? VDBE_CMP_JUMP : VDBE_CMP_CONTINUE;
}

int vdbe_op_ne(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];

	if (mem_is_any_null(pIn1, pIn3) && (pOp->p5 & SQL_NULLEQ) == 0) {
		/* NULL comparison without NULLEQ */
		if ((pOp->p5 & SQL_STOREP2) != 0) {
			Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
			p->iCompare = 1;
			REGISTER_TRACE(p, pOp->p2, pOut);
			return VDBE_CMP_CONTINUE;
		}
		if ((pOp->p5 & SQL_JUMPIFNULL) != 0)
			return VDBE_CMP_JUMP;
		return VDBE_CMP_CONTINUE;
	}

	int cmp_res;
	if (mem_cmp(pIn3, pIn1, &cmp_res, pOp->p4.pColl) != 0)
		return VDBE_CMP_ERROR;

	bool result = (cmp_res != 0);

	if ((pOp->p5 & SQL_STOREP2) != 0) {
		p->iCompare = cmp_res;
		Mem *pOut = &aMem[pOp->p2];
		mem_set_bool(pOut, result);
		REGISTER_TRACE(p, pOp->p2, pOut);
		return VDBE_CMP_CONTINUE;
	}

	return result ? VDBE_CMP_JUMP : VDBE_CMP_CONTINUE;
}

/* OP_Lt: IF r[P3]<r[P1]
 * OP_Le: IF r[P3]<=r[P1]
 * OP_Gt: IF r[P3]>r[P1]
 * OP_Ge: IF r[P3]>=r[P1]
 *
 * Compare the values in register P1 and P3. If the comparison matches,
 * then jump to address P2 or store the comparison result in register P2
 * if the SQL_STOREP2 flag is set in P5.
 */
int vdbe_op_lt(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];

	if (mem_is_any_null(pIn1, pIn3)) {
		if ((pOp->p5 & SQL_STOREP2) != 0) {
			Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
			p->iCompare = 1;
			REGISTER_TRACE(p, pOp->p2, pOut);
			return VDBE_CMP_CONTINUE;
		}
		if ((pOp->p5 & SQL_JUMPIFNULL) != 0)
			return VDBE_CMP_JUMP;
		return VDBE_CMP_CONTINUE;
	}

	int cmp_res;
	if (mem_cmp(pIn3, pIn1, &cmp_res, pOp->p4.pColl) != 0)
		return VDBE_CMP_ERROR;

	bool result = (cmp_res < 0);

	if ((pOp->p5 & SQL_STOREP2) != 0) {
		p->iCompare = cmp_res;
		Mem *pOut = &aMem[pOp->p2];
		mem_set_bool(pOut, result);
		REGISTER_TRACE(p, pOp->p2, pOut);
		return VDBE_CMP_CONTINUE;
	}

	return result ? VDBE_CMP_JUMP : VDBE_CMP_CONTINUE;
}

int vdbe_op_le(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];

	if (mem_is_any_null(pIn1, pIn3)) {
		if ((pOp->p5 & SQL_STOREP2) != 0) {
			Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
			p->iCompare = 1;
			REGISTER_TRACE(p, pOp->p2, pOut);
			return VDBE_CMP_CONTINUE;
		}
		if ((pOp->p5 & SQL_JUMPIFNULL) != 0)
			return VDBE_CMP_JUMP;
		return VDBE_CMP_CONTINUE;
	}

	int cmp_res;
	if (mem_cmp(pIn3, pIn1, &cmp_res, pOp->p4.pColl) != 0)
		return VDBE_CMP_ERROR;

	bool result = (cmp_res <= 0);

	if ((pOp->p5 & SQL_STOREP2) != 0) {
		p->iCompare = cmp_res;
		Mem *pOut = &aMem[pOp->p2];
		mem_set_bool(pOut, result);
		REGISTER_TRACE(p, pOp->p2, pOut);
		return VDBE_CMP_CONTINUE;
	}

	return result ? VDBE_CMP_JUMP : VDBE_CMP_CONTINUE;
}

int vdbe_op_gt(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];

	if (mem_is_any_null(pIn1, pIn3)) {
		if ((pOp->p5 & SQL_STOREP2) != 0) {
			Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
			p->iCompare = 1;
			REGISTER_TRACE(p, pOp->p2, pOut);
			return VDBE_CMP_CONTINUE;
		}
		if ((pOp->p5 & SQL_JUMPIFNULL) != 0)
			return VDBE_CMP_JUMP;
		return VDBE_CMP_CONTINUE;
	}

	int cmp_res;
	if (mem_cmp(pIn3, pIn1, &cmp_res, pOp->p4.pColl) != 0)
		return VDBE_CMP_ERROR;

	bool result = (cmp_res > 0);

	if ((pOp->p5 & SQL_STOREP2) != 0) {
		p->iCompare = cmp_res;
		Mem *pOut = &aMem[pOp->p2];
		mem_set_bool(pOut, result);
		REGISTER_TRACE(p, pOp->p2, pOut);
		return VDBE_CMP_CONTINUE;
	}

	return result ? VDBE_CMP_JUMP : VDBE_CMP_CONTINUE;
}

int vdbe_op_ge(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];

	if (mem_is_any_null(pIn1, pIn3)) {
		if ((pOp->p5 & SQL_STOREP2) != 0) {
			Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
			p->iCompare = 1;
			REGISTER_TRACE(p, pOp->p2, pOut);
			return VDBE_CMP_CONTINUE;
		}
		if ((pOp->p5 & SQL_JUMPIFNULL) != 0)
			return VDBE_CMP_JUMP;
		return VDBE_CMP_CONTINUE;
	}

	int cmp_res;
	if (mem_cmp(pIn3, pIn1, &cmp_res, pOp->p4.pColl) != 0)
		return VDBE_CMP_ERROR;

	bool result = (cmp_res >= 0);

	if ((pOp->p5 & SQL_STOREP2) != 0) {
		p->iCompare = cmp_res;
		Mem *pOut = &aMem[pOp->p2];
		mem_set_bool(pOut, result);
		REGISTER_TRACE(p, pOp->p2, pOut);
		return VDBE_CMP_CONTINUE;
	}

	return result ? VDBE_CMP_JUMP : VDBE_CMP_CONTINUE;
}
