/*
 * Shared tiny opcode implementations for CnP stub inlining.
 *
 * These helpers are intentionally limited to tiny hot opcodes where
 * duplicating the body into the CnP stubs is likely to reduce dispatch
 * overhead without causing code-size explosion.
 */
#ifndef SRC_BOX_SQL_VDBE_OPS_CNP_IMPL_H
#define SRC_BOX_SQL_VDBE_OPS_CNP_IMPL_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef VDBE_CNP_STUB_BUILD
typedef int64_t i64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef struct Vdbe Vdbe;
typedef struct Mem Mem;
typedef struct CnpStubOp Op;

struct CnpStubOp {
	uint8_t opcode;
	int8_t p4type;
	uint16_t p5;
	int p1;
	int p2;
	int p3;
	union {
		int i;
		void *p;
		char *z;
		int64_t *pI64;
		double *pReal;
		bool b;
	} p4;
};

#define P4_INT64 (-10)
#else
#include "sqlInt.h"
#include "vdbeInt.h"
#include "vdbe_debug.h"

int vdbe_cnp_setdiag_handler(struct Vdbe *p, struct VdbeOp *pOp,
			     struct Mem *aMem);
#endif
#include "mem.h"
#include "vdbe_helpers.h"

#if defined(VDBE_CNP_STUB_BUILD) || defined(VDBE_CNP_FRAGMENT_BUILD)
#define VDBE_CNP_INLINE static __attribute__((always_inline)) inline
#else
#define VDBE_CNP_INLINE static inline
#endif

VDBE_CNP_INLINE bool
vdbe_cnp_mem_is_plain_uint(const Mem *mem)
{
	return mem->type == MEM_TYPE_UINT && !mem_is_metatype(mem);
}

VDBE_CNP_INLINE int
vdbe_op_integer_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	mem_set_int(pOut, pOp->p1, pOp->p1 < 0);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_bool_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	assert(pOp->p1 == 0 || pOp->p1 == 1);
	mem_set_bool(pOut, pOp->p1);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_int64_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	assert(pOp->p4.pI64 != NULL);
	mem_set_int(pOut, *pOp->p4.pI64, pOp->p4type == P4_INT64);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_add_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_add(pIn2, pIn1, pOut) != 0)
		return -1;
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_sub_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_sub(pIn2, pIn1, pOut) != 0)
		return -1;
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_multiply_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_mul(pIn2, pIn1, pOut) != 0)
		return -1;
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_divide_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_div(pIn2, pIn1, pOut) != 0)
		return -1;
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_remainder_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_rem(pIn2, pIn1, pOut) != 0)
		return -1;
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_bitand_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (vdbe_cnp_mem_is_plain_uint(pIn1) &&
	    vdbe_cnp_mem_is_plain_uint(pIn2)) {
		uint64_t lhs = pIn1->u.u;
		uint64_t rhs = pIn2->u.u;
		if (lhs == 1023) {
			mem_set_uint(pOut, rhs & 1023);
			return 0;
		}
		if (rhs == 1023) {
			mem_set_uint(pOut, lhs & 1023);
			return 0;
		}
		mem_set_uint(pOut, lhs & rhs);
		return 0;
	}
	if (mem_bit_and(pIn2, pIn1, pOut) != 0)
		return -1;
	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_bitor_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pIn1, pIn2)) {
		mem_set_null(pOut);
		return 0;
	}
	if (vdbe_cnp_mem_is_plain_uint(pIn1) &&
	    vdbe_cnp_mem_is_plain_uint(pIn2)) {
		uint64_t lhs = pIn1->u.u;
		uint64_t rhs = pIn2->u.u;
		if (lhs == 255) {
			mem_set_uint(pOut, rhs | 255);
			return 0;
		}
		if (rhs == 255) {
			mem_set_uint(pOut, lhs | 255);
			return 0;
		}
		mem_set_uint(pOut, lhs | rhs);
		return 0;
	}
	if (mem_bit_or(pIn2, pIn1, pOut) != 0)
		return -1;
	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_bitnot_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pOut = &aMem[pOp->p2];
	if (mem_is_null(pIn1)) {
		mem_set_null(pOut);
		return 0;
	}
	if (vdbe_cnp_mem_is_plain_uint(pIn1)) {
		mem_set_uint(pOut, ~pIn1->u.u);
		return 0;
	}
	return mem_bit_not(pIn1, pOut) != 0 ? -1 : 0;
}

VDBE_CNP_INLINE int
vdbe_op_shiftleft_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pShift = &aMem[pOp->p1];
	Mem *pValue = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pShift, pValue)) {
		mem_set_null(pOut);
		return 0;
	}
	if (vdbe_cnp_mem_is_plain_uint(pShift) &&
	    vdbe_cnp_mem_is_plain_uint(pValue)) {
		uint64_t shift = pShift->u.u;
		uint64_t value = pValue->u.u;
		if (shift == 1) {
			mem_set_uint(pOut, value << 1);
			return 0;
		}
		if (shift == 2) {
			mem_set_uint(pOut, value << 2);
			return 0;
		}
		mem_set_uint(pOut, shift >= 64 ? 0 : value << shift);
		return 0;
	}
	if (mem_shift_left(pValue, pShift, pOut) != 0)
		return -1;
	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_shiftright_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pShift = &aMem[pOp->p1];
	Mem *pValue = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pShift, pValue)) {
		mem_set_null(pOut);
		return 0;
	}
	if (vdbe_cnp_mem_is_plain_uint(pShift) &&
	    vdbe_cnp_mem_is_plain_uint(pValue)) {
		uint64_t shift = pShift->u.u;
		uint64_t value = pValue->u.u;
		if (shift == 1) {
			mem_set_uint(pOut, value >> 1);
			return 0;
		}
		mem_set_uint(pOut, shift >= 64 ? 0 : value >> shift);
		return 0;
	}
	if (mem_shift_right(pValue, pShift, pOut) != 0)
		return -1;
	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);
	return 0;
}

#if !defined(VDBE_CNP_STUB_BUILD)

VDBE_CNP_INLINE int
vdbe_op_and_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	int v1, v2;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut;

	if (mem_is_null(pIn1)) {
		v1 = 2;
	} else if (mem_is_bool(pIn1)) {
		v1 = pIn1->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn1), "boolean");
		return -1;
	}

	if (mem_is_null(pIn2)) {
		v2 = 2;
	} else if (mem_is_bool(pIn2)) {
		v2 = pIn2->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn2), "boolean");
		return -1;
	}

	static const unsigned char and_logic[] = {0, 0, 0, 0, 1, 2, 0, 2, 2};
	v1 = and_logic[v1 * 3 + v2];
	pOut = vdbe_prepare_null_out(p, pOp->p3);
	if (v1 != 2)
		mem_set_bool(pOut, v1);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_or_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	int v1, v2;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut;

	if (mem_is_null(pIn1)) {
		v1 = 2;
	} else if (mem_is_bool(pIn1)) {
		v1 = pIn1->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn1), "boolean");
		return -1;
	}

	if (mem_is_null(pIn2)) {
		v2 = 2;
	} else if (mem_is_bool(pIn2)) {
		v2 = pIn2->u.b;
	} else {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn2), "boolean");
		return -1;
	}

	static const unsigned char or_logic[] = {0, 1, 2, 1, 1, 1, 2, 1, 2};
	v1 = or_logic[v1 * 3 + v2];
	pOut = vdbe_prepare_null_out(p, pOp->p3);
	if (v1 != 2)
		mem_set_bool(pOut, v1);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_not_impl(Vdbe *p, Op *pOp, Mem *aMem)
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

VDBE_CNP_INLINE int
vdbe_op_concat_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn2 = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_concat(pIn2, pIn1, pOut) != 0)
		return -1;
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_offsetlimit_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);

	assert(mem_is_uint(pIn1));
	assert(mem_is_uint(pIn3));
	uint64_t x = pIn1->u.u;
	uint64_t rhs = pIn3->u.u;
	bool unused;
	if (sql_add_int(x, false, rhs, false, (int64_t *)&x, &unused) != 0) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			 "sum of LIMIT and OFFSET values should not result "
			 "in integer overflow");
		return -1;
	}
	mem_set_uint(pOut, x);
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_mustbeint_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	if (mem_to_int_precise(pIn1) != 0) {
		if (pOp->p2 != 0)
			return 1;
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn1), "integer");
		return -1;
	}
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_cast_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	int rc = mem_cast_explicit(pIn1, pOp->p2);
	UPDATE_MAX_BLOBSIZE(pIn1);
	if (rc == 0)
		return 0;
	diag_set(ClientError, ER_SQL_TYPE_MISMATCH, mem_str(pIn1),
		 field_type_strs[pOp->p2]);
	return -1;
}

VDBE_CNP_INLINE int
vdbe_op_applytype_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	enum field_type *types = pOp->p4.types;
	assert(types != NULL);
	Mem *pIn1 = &aMem[pOp->p1];
	for (int i = 0; i < pOp->p2; ++i, ++pIn1) {
		enum field_type type = types[i];
		assert(pIn1 <= &p->aMem[(p->nMem + 1 - p->nCursor)]);
		assert(pIn1->type != MEM_TYPE_INVALID);
		if (mem_cast_implicit(pIn1, type) != 0) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 mem_str(pIn1), field_type_strs[type]);
			return -1;
		}
	}
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_makerecord_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pData0;           /* First field to be combined into the record */
	int nField;            /* Number of fields in the record */
	u8 bIsEphemeral;

	nField = pOp->p2;
	bIsEphemeral = pOp->p5;
	assert(nField > 0 && pOp->p1 >= 0 &&
	       pOp->p1 + nField <= (p->nMem + 1 - p->nCursor) + 1);
	pData0 = &aMem[pOp->p1];

	assert(pOp->p3 < pOp->p1 || pOp->p3 >= pOp->p1 + pOp->p2);
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p3);

	for (int i = 0; i < nField; i++) {
		if (pData0[i].type == MEM_TYPE_INVALID)
			mem_set_null(&pData0[i]);
	}

	struct region *region = &fiber()->gc;
	size_t used = region_used(region);
	uint32_t tuple_size;
	char *tuple = mem_encode_array(pData0, nField, &tuple_size, region);
	if (tuple == NULL)
		return -1;
	if (tuple_size > SQL_MAX_LENGTH) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob too big");
		return -1;
	}

	if (bIsEphemeral) {
		if (mem_copy_bin(pOut, tuple, tuple_size) != 0)
			return -1;
		region_truncate(region, used);
	} else {
		mem_destroy(pOut);
		mem_set_bin_ephemeral(pOut, tuple, tuple_size);
	}
#ifdef SQL_DEBUG
	assert(sqlVdbeCheckMemInvariants(pOut));
#endif
	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	REGISTER_TRACE(p, pOp->p3, pOut);
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}

typedef int (*vdbe_cnp_sysv_op3_f)(Vdbe *, Op *, Mem *);

VDBE_CNP_INLINE int
vdbe_op_makerecord_sysv_bridge(Vdbe *p, Op *pOp, Mem *aMem)
{
	vdbe_cnp_sysv_op3_f fn = (vdbe_cnp_sysv_op3_f)(uintptr_t)vdbe_op_makerecord;
	return fn(p, pOp, aMem);
}

VDBE_CNP_INLINE int
vdbe_op_mustbeint_sysv_bridge(Vdbe *p, Op *pOp, Mem *aMem)
{
	vdbe_cnp_sysv_op3_f fn = (vdbe_cnp_sysv_op3_f)(uintptr_t)vdbe_op_mustbeint;
	return fn(p, pOp, aMem);
}

VDBE_CNP_INLINE int
vdbe_op_aggstep_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	int argc = pOp->p1;
	sql_context *pCtx;
	Mem *pMem;

	assert(pOp->p4type == P4_FUNCCTX);
	pCtx = pOp->p4.pCtx;
	pMem = &aMem[pOp->p3];

	if (pCtx->pOut != pMem)
		pCtx->pOut = pMem;

#ifdef SQL_DEBUG
	for (int i = 0; i < argc; i++) {
		assert(aMem[pOp->p2 + i].type != MEM_TYPE_INVALID);
		REGISTER_TRACE(p, pOp->p2 + i, &aMem[pOp->p2 + i]);
	}
#endif

	pCtx->skipFlag = 0;
	assert(pCtx->func->def->language == FUNC_LANGUAGE_SQL_BUILTIN);
	struct func_sql_builtin *func = (struct func_sql_builtin *)pCtx->func;
	func->call(pCtx, argc, &aMem[pOp->p2]);
	if (pCtx->is_aborted)
		return -1;
	if (pCtx->skipFlag) {
		assert(pOp[-1].opcode == OP_SkipLoad);
		int i = pOp[-1].p1;
		if (i)
			mem_set_bool(&aMem[i], true);
	}
	return 0;
}

VDBE_CNP_INLINE int
vdbe_op_aggfinal_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	assert(pOp->p1 > 0 && pOp->p1 <= (p->nMem + 1 - p->nCursor));
	struct func_sql_builtin *func = (struct func_sql_builtin *)pOp->p4.func;
	struct Mem *pIn1 = &aMem[pOp->p1];

	if (func->finalize != NULL && func->finalize(pIn1) != 0)
		return -1;
	UPDATE_MAX_BLOBSIZE(pIn1);
	if (sqlVdbeMemTooBig(pIn1) != 0) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob too big");
		return -1;
	}
	return 0;
}

enum vdbe_cmp_predicate {
	VDBE_CMP_EQ,
	VDBE_CMP_NE,
	VDBE_CMP_LT,
	VDBE_CMP_LE,
	VDBE_CMP_GT,
	VDBE_CMP_GE,
};

VDBE_CNP_INLINE int
vdbe_op_cmp_impl(Vdbe *p, Op *pOp, Mem *aMem, enum vdbe_cmp_predicate pred)
{
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pIn3 = &aMem[pOp->p3];
	bool treat_null_as_equal = pred == VDBE_CMP_EQ || pred == VDBE_CMP_NE;

	if (mem_is_any_null(pIn1, pIn3) &&
	    (!treat_null_as_equal || (pOp->p5 & SQL_NULLEQ) == 0)) {
		if ((pOp->p5 & SQL_STOREP2) != 0) {
			Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
			p->iCompare = 1;
			REGISTER_TRACE(p, pOp->p2, pOut);
			return 0;
		}
		if ((pOp->p5 & SQL_JUMPIFNULL) != 0)
			return 1;
		return 0;
	}

	int cmp_res;
	if (mem_cmp(pIn3, pIn1, &cmp_res, pOp->p4.pColl) != 0)
		return -1;

	bool result = false;
	switch (pred) {
	case VDBE_CMP_EQ:
		result = cmp_res == 0;
		break;
	case VDBE_CMP_NE:
		result = cmp_res != 0;
		break;
	case VDBE_CMP_LT:
		result = cmp_res < 0;
		break;
	case VDBE_CMP_LE:
		result = cmp_res <= 0;
		break;
	case VDBE_CMP_GT:
		result = cmp_res > 0;
		break;
	case VDBE_CMP_GE:
		result = cmp_res >= 0;
		break;
	}

	if ((pOp->p5 & SQL_STOREP2) != 0) {
		p->iCompare = cmp_res;
		Mem *pOut = &aMem[pOp->p2];
		mem_set_bool(pOut, result);
		REGISTER_TRACE(p, pOp->p2, pOut);
		return 0;
	}

	return result ? 1 : 0;
}

VDBE_CNP_INLINE int
vdbe_op_eq_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_cmp_impl(p, pOp, aMem, VDBE_CMP_EQ);
}

VDBE_CNP_INLINE int
vdbe_op_ne_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_cmp_impl(p, pOp, aMem, VDBE_CMP_NE);
}

VDBE_CNP_INLINE int
vdbe_op_lt_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_cmp_impl(p, pOp, aMem, VDBE_CMP_LT);
}

VDBE_CNP_INLINE int
vdbe_op_le_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_cmp_impl(p, pOp, aMem, VDBE_CMP_LE);
}

VDBE_CNP_INLINE int
vdbe_op_gt_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_cmp_impl(p, pOp, aMem, VDBE_CMP_GT);
}

VDBE_CNP_INLINE int
vdbe_op_ge_impl(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_cmp_impl(p, pOp, aMem, VDBE_CMP_GE);
}

#endif /* !VDBE_CNP_STUB_BUILD && !VDBE_CNP_FRAGMENT_BUILD */

#undef VDBE_CNP_INLINE

#endif /* SRC_BOX_SQL_VDBE_OPS_CNP_IMPL_H */
