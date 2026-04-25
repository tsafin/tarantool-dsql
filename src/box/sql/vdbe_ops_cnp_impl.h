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
#endif
#include "mem.h"
#include "vdbe_helpers.h"

#ifdef VDBE_CNP_STUB_BUILD
#define VDBE_CNP_INLINE static __attribute__((always_inline)) inline
#else
#define VDBE_CNP_INLINE static inline
#endif

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

#undef VDBE_CNP_INLINE

#endif /* SRC_BOX_SQL_VDBE_OPS_CNP_IMPL_H */
