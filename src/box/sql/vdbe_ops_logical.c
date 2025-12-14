/* Logical and bitwise opcode handlers extracted from vdbe.c
 *
 * These handlers implement SQL logical operations (AND, OR, NOT) and
 * bitwise operations (BitAnd, BitOr, BitNot).
 *
 * Boolean logic uses three-valued logic (TRUE, FALSE, NULL/UNKNOWN)
 * following SQL standard semantics.
 */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"

/* OP_And: r[P3] = r[P1] AND r[P2]
 * OP_Or:  r[P3] = r[P1] OR r[P2]
 *
 * Take the logical AND/OR of the values in registers P1 and P2 and
 * store the result in register P3.
 *
 * Uses three-valued logic:
 * - 0 = FALSE
 * - 1 = TRUE
 * - 2 = NULL/UNKNOWN
 *
 * Truth tables:
 * AND: F^F=F, F^T=F, F^N=F, T^T=T, T^N=N, N^N=N
 * OR:  F|F=F, F|T=T, F|N=N, T|T=T, T|N=T, N|N=N
 */
int vdbe_op_and(Vdbe *p, Op *pOp, Mem *aMem)
{
	int v1, v2;  /* Operands: 0=FALSE, 1=TRUE, 2=NULL */
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut;

	/* Evaluate left operand */
	if (mem_is_null(pIn1)) {
		v1 = 2;
	} else if (mem_is_bool(pIn1)) {
		v1 = pIn1->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn1), "boolean");
		return -1;
	}

	/* Evaluate right operand */
	if (mem_is_null(pIn2)) {
		v2 = 2;
	} else if (mem_is_bool(pIn2)) {
		v2 = pIn2->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn2), "boolean");
		return -1;
	}

	/* Apply AND logic using lookup table */
	static const unsigned char and_logic[] = { 0, 0, 0, 0, 1, 2, 0, 2, 2 };
	v1 = and_logic[v1 * 3 + v2];

	/* Store result */
	pOut = vdbe_prepare_null_out(p, pOp->p3);
	if (v1 != 2)
		mem_set_bool(pOut, v1);
	return 0;
}

int vdbe_op_or(Vdbe *p, Op *pOp, Mem *aMem)
{
	int v1, v2;  /* Operands: 0=FALSE, 1=TRUE, 2=NULL */
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut;

	/* Evaluate left operand */
	if (mem_is_null(pIn1)) {
		v1 = 2;
	} else if (mem_is_bool(pIn1)) {
		v1 = pIn1->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn1), "boolean");
		return -1;
	}

	/* Evaluate right operand */
	if (mem_is_null(pIn2)) {
		v2 = 2;
	} else if (mem_is_bool(pIn2)) {
		v2 = pIn2->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn2), "boolean");
		return -1;
	}

	/* Apply OR logic using lookup table */
	static const unsigned char or_logic[] = { 0, 1, 2, 1, 1, 1, 2, 1, 2 };
	v1 = or_logic[v1 * 3 + v2];

	/* Store result */
	pOut = vdbe_prepare_null_out(p, pOp->p3);
	if (v1 != 2)
		mem_set_bool(pOut, v1);
	return 0;
}

/* OP_Not: r[P2] = !r[P1]
 *
 * Interpret the value in register P1 as a boolean value. Store the
 * boolean complement in register P2. If the value in register P1 is
 * NULL, then a NULL is stored in P2.
 */
int vdbe_op_not(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);

	if (!mem_is_null(pIn1)) {
		if (!mem_is_bool(pIn1)) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 mem_str(pIn1), "boolean");
			return -1;
		}
		mem_set_bool(pOut, !pIn1->u.b);
	}
	return 0;
}

/* OP_BitAnd: r[P3] = r[P1] & r[P2]
 *
 * Take the bit-wise AND of the values in register P1 and P2 and
 * store the result in register P3.
 * If either input is NULL, the result is NULL.
 */
int vdbe_op_bitand(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];

	if (mem_bit_and(pIn2, pIn1, pOut) != 0)
		return -1;

	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);
	return 0;
}

/* OP_BitOr: r[P3] = r[P1] | r[P2]
 *
 * Take the bit-wise OR of the values in register P1 and P2 and
 * store the result in register P3.
 * If either input is NULL, the result is NULL.
 */
int vdbe_op_bitor(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];

	if (mem_bit_or(pIn2, pIn1, pOut) != 0)
		return -1;

	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);
	return 0;
}

/* OP_BitNot: r[P2] = ~r[P1]
 *
 * Interpret the content of register P1 as an integer. Store the
 * ones-complement of the P1 value into register P2. If P1 holds
 * a NULL then store a NULL in P2.
 */
int vdbe_op_bitnot(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pOut = &aMem[pOp->p2];

	if (mem_bit_not(pIn1, pOut) != 0)
		return -1;

	return 0;
}
