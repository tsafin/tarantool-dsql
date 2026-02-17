/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 4
 * Phase 5.6e: Control flow and cursor operation opcodes
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (6 opcodes, 95-200 chars):
 * - OP_Once: Execute code block at most once
 * - OP_IfNot: Jump if register value is false
 * - OP_IfPos: Jump if positive and apply saturated decrement
 * - OP_IfNotZero: Jump if non-zero and decrement
 * - OP_DecrJumpZero: Decrement and jump if zero
 * - OP_NullRow: Mark cursor at null row and cleanup
 *
 * Helper dependencies:
 * - mem_is_bool(): Check if value is boolean (standard mem interface)
 * - mem_is_null(): Check if value is NULL (standard mem interface)
 * - mem_is_int(): Check if value is integer (standard mem interface)
 * - mem_is_uint(): Check if value is unsigned integer (standard mem interface)
 * - mem_str(): Get string representation of value (standard mem interface)
 * - diag_set(): Set diagnostic error (standard tarantool interface)
 * - sql_cursor_cleanup(): Clean up Tarantool cursor
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"

/*
 * Opcode: ONCE P1 P2 * * *
 * Synopsis: if r[P1]==0 then r[P1]=1 and jump to P2
 *
 * Check if the initialization flag for P1 is clear. The flag is stored
 * in the P1 element of the first instruction (OP_Init). If the flag is
 * clear, set it to 1 and jump to P2. Otherwise, continue to the next
 * instruction.
 *
 * This opcode is used to ensure that certain code blocks (like triggers)
 * are executed at most once during a single VDBE execution.
 *
 * Preconditions:
 * - p->aOp[0].opcode == OP_Init (first opcode must be initialization)
 * - P1 must be >= 0 (valid flag value)
 * - P2 must be >= 0 (valid jump target)
 */
int
vdbe_op_once_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;

	assert(p->aOp[0].opcode == OP_Init);

	if (p->aOp[0].p1 == pOp->p1) {
		/* Flag is set, so jump to P2 */
		return 1;  /* Signal jump to P2 */
	} else {
		/* Flag is not set, set it now for next execution */
		pOp->p1 = p->aOp[0].p1;
		return 0;  /* Continue to next instruction */
	}
}

/*
 * Opcode: IF P1 P2 P3 * *
 * Opcode: IFNOT P1 P2 P3 * *
 * Synopsis: if r[P1] goto P2
 *
 * Jump to P2 if the value in register P1 is true (OP_If) or false (OP_IfNot).
 * If the value in P1 is NULL, jump if and only if P3 is non-zero.
 *
 * The register P1 must contain a boolean or NULL value. If it contains
 * any other type, a type mismatch error is raised.
 *
 * Preconditions:
 * - P1 must be a valid register number
 * - P2 must be a valid jump target
 * - P3 must be 0 or 1 (controls NULL behavior)
 *
 * For OP_If: Jump if true
 * For OP_IfNot: Jump if false (negated)
 */
int
vdbe_op_ifnot_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	int should_jump;
	Mem *pIn1;

	(void)p;

	pIn1 = &aMem[pOp->p1];

	if (mem_is_null(pIn1)) {
		/* NULL value: use P3 to determine jump */
		should_jump = pOp->p3;
	} else if (mem_is_bool(pIn1)) {
		/* Boolean value: use value, negated for OP_IfNot */
		should_jump = pOp->opcode == OP_IfNot ? !pIn1->u.b : pIn1->u.b;
	} else {
		/* Type error: value is not boolean or NULL */
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn1), "boolean");
		return -1;  /* Error: return -1 */
	}

	if (should_jump) {
		return 1;  /* Signal jump to P2 */
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: IFPOS P1 P2 P3 * *
 * Synopsis: if r[P1]>P3 then r[P1]-=P3, goto P2
 *
 * Register P1 must hold an unsigned integer. If the value in P1
 * is greater than P3, subtract P3 from the value in P1 and jump to P2.
 * The subtraction uses saturated arithmetic to prevent underflow.
 *
 * This opcode is commonly used for LIMIT/OFFSET processing where we need
 * to decrement the limit/offset value and conditionally jump.
 *
 * Preconditions:
 * - P1 must be a valid register number
 * - P1 must contain an unsigned integer value
 * - P2 must be a valid jump target
 * - P3 must be >= 0 (decrement amount)
 */
int
vdbe_op_ifpos_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1;
	uint64_t res;

	(void)p;

	pIn1 = &aMem[pOp->p1];
	assert(mem_is_int(pIn1));

	if (mem_is_uint(pIn1) && pIn1->u.u != 0) {
		assert(pOp->p3 >= 0);
		/* Subtract P3 with saturated arithmetic */
		res = pIn1->u.u - (uint64_t)pOp->p3;
		/* Saturate: if result would be negative, use 0 */
		res &= -(res <= pIn1->u.u);
		pIn1->u.u = res;
		return 1;  /* Signal jump to P2 */
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: IFNOTZERO P1 P2 * * *
 * Synopsis: if r[P1]!=0 then r[P1]-=1, goto P2
 *
 * Register P1 must hold an unsigned integer. If the value is non-zero,
 * decrement it by 1 and jump to P2. Otherwise, continue to the next
 * instruction.
 *
 * This opcode is used for loop control, commonly in FOR loops and
 * multi-row processing scenarios.
 *
 * Preconditions:
 * - P1 must be a valid register number
 * - P1 must contain an unsigned integer value
 * - P2 must be a valid jump target
 */
int
vdbe_op_ifnotzero_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1;

	(void)p;

	pIn1 = &aMem[pOp->p1];
	assert(mem_is_uint(pIn1));

	if (pIn1->u.u > 0) {
		pIn1->u.u--;
		return 1;  /* Signal jump to P2 */
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: DECRJUMPZERO P1 P2 * * *
 * Synopsis: r[P1]-=1, if (r[P1]==0) goto P2
 *
 * Register P1 must hold an unsigned integer. Decrement the value in P1
 * by 1. If the new value is exactly zero, jump to P2. Otherwise,
 * continue to the next instruction.
 *
 * This opcode is similar to IFNOTZERO but always decrements (regardless
 * of whether the value is zero), and jumps on zero rather than non-zero.
 *
 * Preconditions:
 * - P1 must be a valid register number
 * - P1 must contain an unsigned integer value
 * - P2 must be a valid jump target
 */
int
vdbe_op_decrjumpzero_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1;

	(void)p;

	pIn1 = &aMem[pOp->p1];
	assert(mem_is_uint(pIn1));

	if (pIn1->u.u > 0) {
		pIn1->u.u--;
	}

	if (pIn1->u.u == 0) {
		return 1;  /* Signal jump to P2 */
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: NULLROW P1 * * * *
 * Synopsis: intkey=0
 *
 * Set the null-row flag for P1, indicating that the cursor is not
 * positioned on any valid row. Also mark the cursor cache as stale.
 * If the cursor is a Tarantool cursor, clean up its internal state.
 *
 * This opcode is used when a cursor needs to be reset to "no current row"
 * state, which can happen after DELETE operations or when a query produces
 * no results.
 *
 * Preconditions:
 * - P1 must be a valid cursor number (0 <= P1 < p->nCursor)
 * - p->apCsr[P1] must point to a valid VdbeCursor structure
 */
int
vdbe_op_nullrow_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeCursor *pC;

	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);

	pC->nullRow = 1;
	pC->cacheStatus = CACHE_STALE;

	if (pC->eCurType == CURTYPE_TARANTOOL) {
		assert(pC->uc.pCursor != 0);
		sql_cursor_cleanup(pC->uc.pCursor);
	}

	return 0;  /* Continue to next instruction */
}
