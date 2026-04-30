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
#include "vdbe_ops_cnp_impl.h"
#include "vdbe_debug.h"

static inline bool
mem_is_plain_uint(const struct Mem *mem)
{
	return mem->type == MEM_TYPE_UINT && !mem_is_metatype(mem);
}

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
int SQL_PRESERVE_NONE vdbe_op_and(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_and_impl(p, pOp, aMem);
}

int SQL_PRESERVE_NONE vdbe_op_or(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_or_impl(p, pOp, aMem);
}

/* OP_Not: r[P2] = !r[P1]
 *
 * Interpret the value in register P1 as a boolean value. Store the
 * boolean complement in register P2. If the value in register P1 is
 * NULL, then a NULL is stored in P2.
 */
int SQL_PRESERVE_NONE vdbe_op_not(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_not_impl(p, pOp, aMem);
}

/* OP_BitAnd: r[P3] = r[P1] & r[P2]
 *
 * Take the bit-wise AND of the values in register P1 and P2 and
 * store the result in register P3.
 * If either input is NULL, the result is NULL.
 */
int SQL_PRESERVE_NONE vdbe_op_bitand(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_bitand_impl(p, pOp, aMem);
}

int
vdbe_op_bitand_inline(Vdbe *p, Op *pOp, Mem *aMem)
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

int
vdbe_op_bitand_uint_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pIn1) || !mem_is_plain_uint(pIn2))
		return vdbe_op_bitand_inline(p, pOp, aMem);
	uint64_t lhs = pIn1->u.u;
	uint64_t rhs = pIn2->u.u;
	mem_set_uint(pOut, lhs & rhs);
	return 0;
}

int
vdbe_op_bitand_p1_imm1023_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_null(pIn)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pIn))
		return vdbe_op_bitand_inline(p, pOp, aMem);
	mem_set_uint(pOut, pIn->u.u & 1023);
	return 0;
}

/* OP_BitOr: r[P3] = r[P1] | r[P2]
 *
 * Take the bit-wise OR of the values in register P1 and P2 and
 * store the result in register P3.
 * If either input is NULL, the result is NULL.
 */
int SQL_PRESERVE_NONE vdbe_op_bitor(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_bitor_impl(p, pOp, aMem);
}

int
vdbe_op_bitor_inline(Vdbe *p, Op *pOp, Mem *aMem)
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

int
vdbe_op_bitor_uint_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pIn1) || !mem_is_plain_uint(pIn2))
		return vdbe_op_bitor_inline(p, pOp, aMem);
	uint64_t lhs = pIn1->u.u;
	uint64_t rhs = pIn2->u.u;
	mem_set_uint(pOut, lhs | rhs);
	return 0;
}

int
vdbe_op_bitor_p1_imm255_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_null(pIn)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pIn))
		return vdbe_op_bitor_inline(p, pOp, aMem);
	mem_set_uint(pOut, pIn->u.u | 255);
	return 0;
}

/* OP_BitNot: r[P2] = ~r[P1]
 *
 * Interpret the content of register P1 as an integer. Store the
 * ones-complement of the P1 value into register P2. If P1 holds
 * a NULL then store a NULL in P2.
 */
int SQL_PRESERVE_NONE vdbe_op_bitnot(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_bitnot_impl(p, pOp, aMem);
}

int
vdbe_op_bitnot_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn = &aMem[pOp->p1];
	Mem *pOut = &aMem[pOp->p2];
	return mem_bit_not(pIn, pOut) != 0 ? -1 : 0;
}

int
vdbe_op_bitnot_uint_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn = &aMem[pOp->p1];
	Mem *pOut = &aMem[pOp->p2];
	if (mem_is_null(pIn)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pIn))
		return vdbe_op_bitnot_inline(p, pOp, aMem);
	uint64_t value = pIn->u.u;
	mem_set_uint(pOut, ~value);
	return 0;
}
