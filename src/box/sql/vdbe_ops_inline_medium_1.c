/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 1
 * Phase 5.6b: Refactored inline opcodes as handler functions
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (3 opcodes, simple register/cursor operations):
 * - OP_Close: Close a cursor
 * - OP_IsNull: Jump if register is NULL
 * - OP_NotNull: Jump if register is not NULL (already in simple batch)
 *
 * Note: Some opcodes like OP_AddImm, OP_OpenSpace, OP_Sequence, etc. require
 * additional helper functions from vdbe.c (memAboutToChange, space_by_id,
 * vdbe_prepare_null_out). These will be handled in Phase 5.6c once we extract
 * those helpers to a shared location.
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"

/*
 * Opcode: CLOSE - Close cursor P1
 *
 * Close a cursor number P1. The cursor must have been previously opened.
 * All pending operations on the cursor are terminated.
 *
 * Flags: IN1
 */
int
vdbe_op_close_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;

	int nCursor = p->nCursor;

	/* P1 must be a valid cursor number */
	assert(pOp->p1 >= 0 && pOp->p1 < nCursor);

	/* Free the cursor if it exists */
	if (p->apCsr[pOp->p1] != NULL) {
		sqlVdbeFreeCursor(p->apCsr[pOp->p1]);
		p->apCsr[pOp->p1] = NULL;
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: ISNULL - Jump to P2 if register P1 is NULL
 *
 * Test the value in register P1. If it is NULL, jump to instruction P2.
 * If not NULL, fall through to the next instruction.
 *
 * Flags: IN1, JUMP
 */
int
vdbe_op_isnull_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;

	Mem *pIn1;

	pIn1 = &aMem[pOp->p1];

	if (mem_is_null(pIn1)) {
		/* Jump to P2 */
		return 1;  /* Signal jump */
	}

	return 0;  /* Continue to next instruction */
}
