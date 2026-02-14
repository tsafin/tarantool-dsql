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

/* OP_Compare: r[P1@P3] <-> r[P2@P3]
 *
 * Compare P3 consecutive registers starting at P1 with P3 consecutive
 * registers starting at P2. Store the comparison result in p->iCompare.
 *
 * The comparison is done element by element using the key definition in P4.
 * If OPFLAG_PERMUTE is set in P5, the comparison uses a permutation array
 * that was set by OP_Permutation (stored in p->aPermute).
 *
 * Returns:
 * - 0: comparison complete, continue to next opcode
 * - -1: error occurred
 */
int vdbe_op_compare(Vdbe *p, Op *pOp, Mem *aMem)
{
	if ((pOp->p5 & OPFLAG_PERMUTE) == 0)
		p->aPermute = 0;

	int n = pOp->p3;
	assert(pOp->p4type == P4_KEYINFO);
	assert(n > 0);
	int p1 = pOp->p1;
	int p2 = pOp->p2;

	struct key_def *def = sql_key_info_to_key_def(pOp->p4.key_info);
	if (def == NULL)
		return VDBE_CMP_ERROR;

#if SQL_DEBUG
	if (p->aPermute) {
		int mx = 0;
		for(uint32_t k = 0; k < (uint32_t)n; k++)
			if (p->aPermute[k] > mx)
				mx = p->aPermute[k];
		assert(p1>0 && p1+mx<=(p->nMem+1 - p->nCursor)+1);
		assert(p2>0 && p2+mx<=(p->nMem+1 - p->nCursor)+1);
	} else {
		assert(p1>0 && p1+n<=(p->nMem+1 - p->nCursor)+1);
		assert(p2>0 && p2+n<=(p->nMem+1 - p->nCursor)+1);
	}
#endif /* SQL_DEBUG */

	for(int i = 0; i < n; i++) {
		int idx = p->aPermute ? p->aPermute[i] : i;
		assert(memIsValid(&aMem[p1+idx]));
		assert(memIsValid(&aMem[p2+idx]));
		REGISTER_TRACE(p, p1+idx, &aMem[p1+idx]);
		REGISTER_TRACE(p, p2+idx, &aMem[p2+idx]);
		assert(i < (int)def->part_count);
		struct coll *coll = def->parts[i].coll;
		bool is_rev = def->parts[i].sort_order == SORT_ORDER_DESC;
		struct Mem *a = &aMem[p1+idx];
		struct Mem *b = &aMem[p2+idx];
		if (!mem_is_comparable(a)) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH, mem_str(a),
				 "comparable type");
			return VDBE_CMP_ERROR;
		}
		if (!mem_is_comparable(b)) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH, mem_str(b),
				 "comparable type");
			return VDBE_CMP_ERROR;
		}
		p->iCompare = mem_cmp_scalar(a, b, coll);
		if (p->iCompare) {
			if (is_rev)
				p->iCompare = -p->iCompare;
			break;
		}
	}
	p->aPermute = 0;
	return VDBE_CMP_CONTINUE;
}
