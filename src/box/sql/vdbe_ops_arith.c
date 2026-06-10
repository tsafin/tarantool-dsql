/* Arithmetic opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_ops_cnp_impl.h"

static inline bool
mem_is_plain_int(const struct Mem *mem)
{
	return mem_is_int(mem) && !mem_is_metatype(mem);
}

#ifdef ENABLE_SQL_CNP
static inline const struct cnp_arith_imm *
cnp_arith_imm(const Vdbe *p, const Op *pOp)
{
	if (p == NULL || p->cnp_arith_imm == NULL || p->aOp == NULL)
		return NULL;
	int pc = (int)(pOp - p->aOp);
	if (pc < 0 || pc >= p->nOp)
		return NULL;
	return &p->cnp_arith_imm[pc];
}

static inline bool
cnp_arith_const_input(const struct cnp_arith_imm *imm, uint8_t mask,
		      int64_t *value, bool *is_signed)
{
	if (imm == NULL || (imm->mask & mask) == 0)
		return false;
	if (mask == CNP_ARITH_IMM_P1) {
		*value = imm->p1_value;
		*is_signed = imm->p1_is_signed;
	} else {
		*value = imm->p2_value;
		*is_signed = imm->p2_is_signed;
	}
	return true;
}

static inline bool
cnp_arith_mem_input(const Mem *mem, int64_t *value, bool *is_signed)
{
	if (mem_is_null(mem))
		return false;
	if (!mem_is_plain_int(mem))
		return false;
	*value = mem->u.i;
	*is_signed = mem->type == MEM_TYPE_INT;
	return true;
}

typedef int (*arith_const_fn)(int64_t lhs, bool lhs_signed, int64_t rhs,
			      bool rhs_signed, int64_t *res, bool *is_signed);

static inline int
vdbe_op_arith_const_fast(Vdbe *p, Op *pOp, Mem *aMem, uint8_t opcode,
			   arith_const_fn arith_fn, int (*fallback)(Vdbe *, Op *,
							    Mem *))
{
	const struct cnp_arith_imm *imm = cnp_arith_imm(p, pOp);
	int64_t lhs, rhs, res;
	bool lhs_signed, rhs_signed, res_signed;
	if (imm == NULL)
		return fallback(p, pOp, aMem);
	Mem *pOut = &aMem[pOp->p3];
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	if ((imm->mask & CNP_ARITH_IMM_P1) == 0) {
		if (mem_is_null(pIn1))
			goto set_null;
		if (!cnp_arith_mem_input(pIn1, &rhs, &rhs_signed))
			return fallback(p, pOp, aMem);
	} else if (!cnp_arith_const_input(imm, CNP_ARITH_IMM_P1, &rhs,
					      &rhs_signed)) {
		return fallback(p, pOp, aMem);
	}
	if ((imm->mask & CNP_ARITH_IMM_P2) == 0) {
		if (mem_is_null(pIn2))
			goto set_null;
		if (!cnp_arith_mem_input(pIn2, &lhs, &lhs_signed))
			return fallback(p, pOp, aMem);
	} else if (!cnp_arith_const_input(imm, CNP_ARITH_IMM_P2, &lhs,
					      &lhs_signed)) {
		return fallback(p, pOp, aMem);
	}
	if ((opcode == OP_Divide || opcode == OP_Remainder) && rhs == 0)
		return fallback(p, pOp, aMem);
	if (arith_fn(lhs, lhs_signed, rhs, rhs_signed, &res, &res_signed) != 0)
		return fallback(p, pOp, aMem);
	mem_set_int(pOut, res, res_signed);
	return 0;
set_null:
	mem_set_null(pOut);
	return 0;
}
#endif /* ENABLE_SQL_CNP */

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

int
vdbe_op_add_sysv_bridge(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_add_impl(p, pOp, aMem);
}

int
vdbe_op_add_int_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_int(pIn1) || !mem_is_plain_int(pIn2))
		return vdbe_op_add_impl(p, pOp, aMem);
	int64_t res;
	bool is_neg;
	if (sql_add_int(pIn2->u.i, pIn2->type == MEM_TYPE_INT, pIn1->u.i,
			pIn1->type == MEM_TYPE_INT, &res, &is_neg) != 0)
		return vdbe_op_add_impl(p, pOp, aMem);
	mem_set_int(pOut, res, is_neg);
	return 0;
}

#ifdef ENABLE_SQL_CNP
int
vdbe_op_add_const_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_arith_const_fast(p, pOp, aMem, OP_Add, sql_add_int,
					vdbe_op_add_impl);
}
#endif

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

int
vdbe_op_sub_sysv_bridge(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_sub_impl(p, pOp, aMem);
}

int
vdbe_op_sub_int_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_int(pIn1) || !mem_is_plain_int(pIn2))
		return vdbe_op_sub_impl(p, pOp, aMem);
	int64_t res;
	bool is_neg;
	if (sql_sub_int(pIn2->u.i, pIn2->type == MEM_TYPE_INT, pIn1->u.i,
			pIn1->type == MEM_TYPE_INT, &res, &is_neg) != 0)
		return vdbe_op_sub_impl(p, pOp, aMem);
	mem_set_int(pOut, res, is_neg);
	return 0;
}

#ifdef ENABLE_SQL_CNP
int
vdbe_op_sub_const_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_arith_const_fast(p, pOp, aMem, OP_Subtract, sql_sub_int,
					vdbe_op_sub_impl);
}
#endif

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

int
vdbe_op_multiply_sysv_bridge(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_multiply_impl(p, pOp, aMem);
}

int
vdbe_op_multiply_int_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_int(pIn1) || !mem_is_plain_int(pIn2))
		return vdbe_op_multiply_impl(p, pOp, aMem);
	int64_t res;
	bool is_neg;
	if (sql_mul_int(pIn2->u.i, pIn2->type == MEM_TYPE_INT, pIn1->u.i,
			pIn1->type == MEM_TYPE_INT, &res, &is_neg) != 0)
		return vdbe_op_multiply_impl(p, pOp, aMem);
	mem_set_int(pOut, res, is_neg);
	return 0;
}

#ifdef ENABLE_SQL_CNP
int
vdbe_op_multiply_const_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_arith_const_fast(p, pOp, aMem, OP_Multiply, sql_mul_int,
					vdbe_op_multiply_impl);
}
#endif

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

int
vdbe_op_divide_sysv_bridge(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_divide_impl(p, pOp, aMem);
}

int
vdbe_op_divide_int_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_int(pIn1) || !mem_is_plain_int(pIn2))
		return vdbe_op_divide_impl(p, pOp, aMem);
	if (pIn1->u.u == 0)
		return vdbe_op_divide_impl(p, pOp, aMem);
	int64_t res;
	bool is_neg;
	if (sql_div_int(pIn2->u.i, pIn2->type == MEM_TYPE_INT, pIn1->u.i,
			pIn1->type == MEM_TYPE_INT, &res, &is_neg) != 0)
		return vdbe_op_divide_impl(p, pOp, aMem);
	mem_set_int(pOut, res, is_neg);
	return 0;
}

#ifdef ENABLE_SQL_CNP
int
vdbe_op_divide_const_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_arith_const_fast(p, pOp, aMem, OP_Divide, sql_div_int,
					vdbe_op_divide_impl);
}
#endif

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

int
vdbe_op_remainder_sysv_bridge(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_remainder_impl(p, pOp, aMem);
}

int
vdbe_op_remainder_int_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_int(pIn1) || !mem_is_plain_int(pIn2))
		return vdbe_op_remainder_impl(p, pOp, aMem);
	if (pIn1->u.u == 0)
		return vdbe_op_remainder_impl(p, pOp, aMem);
	int64_t res;
	bool is_neg;
	if (sql_rem_int(pIn2->u.i, pIn2->type == MEM_TYPE_INT, pIn1->u.i,
			pIn1->type == MEM_TYPE_INT, &res, &is_neg) != 0)
		return vdbe_op_remainder_impl(p, pOp, aMem);
	mem_set_int(pOut, res, is_neg);
	return 0;
}

#ifdef ENABLE_SQL_CNP
int
vdbe_op_remainder_const_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_arith_const_fast(p, pOp, aMem, OP_Remainder, sql_rem_int,
					vdbe_op_remainder_impl);
}
#endif
