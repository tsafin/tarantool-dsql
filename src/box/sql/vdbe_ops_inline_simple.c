/*
 * VDBE Inline Opcode Handlers - Simple Opcodes
 * Phase 5.6a: Refactored inline opcodes as handler functions
 *
 * This file contains wrapper functions for simple inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (6 opcodes, all < 100 chars in original form):
 * - OP_Noop: No operation
 * - OP_Explain: Explain query plan
 * - OP_SkipLoad: Skip VDBE loading
 * - OP_Expire: Expire cached schema
 * - OP_NotNull: Jump if not null (control flow)
 * - OP_Permutation: Permutation setup
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "tarantoolInt.h"

/*
 * Opcode: NOOP - No operation
 *
 * This opcode does nothing. It is often used as a placeholder.
 */
int
vdbe_op_noop_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;

	/* Do nothing - just return success */
	return 0;
}

/*
 * Opcode: EXPLAIN - Explain query plan
 *
 * Display information about the query plan.
 */
int
vdbe_op_explain_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;

	/* Explain is typically used during query planning, not execution */
	return 0;
}

/*
 * Opcode: SKIPLOAD - Skip VDBE loading
 *
 * Skip loading if in explain mode.
 */
int
vdbe_op_skipload_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	if (pOp->p1 != 0)
		mem_set_bool(&aMem[pOp->p1], false);
	return 0;
}

/*
 * Opcode: EXPIRE - Expire cached schema
 *
 * Expire schema cache entries.
 */
int
vdbe_op_expire_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;

	/* Schema cache expiration is handled at higher level */
	return 0;
}


/*
 * Opcode: NOTNULL - Jump if not null
 *
 * Jump to P2 if the value in P1 is not NULL.
 * This is a control flow opcode.
 */
int
vdbe_op_notnull_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;

	Mem *pIn1 = &aMem[pOp->p1];

	if (!mem_is_null(pIn1)) {
		/* Should jump to P2 */
		return 1;  /* Signal jump */
	}
	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: PERMUTATION - Permutation setup
 *
 * Set up permutation for result row.
 */
int
vdbe_op_permutation_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;

	/* Set up permutation array for OP_Compare */
	assert(pOp->p4type == P4_INTARRAY);
	assert(pOp->p4.ai);

	/* p4.ai[0] is the array length, actual permutation starts at p4.ai + 1 */
	p->aPermute = pOp->p4.ai + 1;

	return 0;
}
