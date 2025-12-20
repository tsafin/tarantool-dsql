/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 5
 * Phase 5.6f: Type/value, space management, and sorting operations
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (5 opcodes, 123-203 chars):
 * - OP_ShowCreateTable: Generate CREATE TABLE statement text
 * - OP_ResetSorter: Reset sorter state
 * - OP_Sort: Sort records (test harness)
 * - OP_Clear: Clear space/truncate table
 * - OP_Param: Load parameter from frame
 *
 * Helper dependencies:
 * - sql_show_create_table(): Generate CREATE TABLE statement
 * - space_by_id(): Look up space by ID
 * - isSorter(): Check if cursor is sorter type
 * - sqlVdbeSorterReset(): Reset sorter state
 * - box_truncate(): Truncate space
 * - vdbe_prepare_null_out(): Initialize output register
 * - mem_copy_as_ephemeral(): Copy memory as ephemeral
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_helpers.h"
#include "box/space_cache.h"
#include "box/box.h"

/*
 * Opcode: SHOWCREATETTABLE P1 P2 * * *
 * Synopsis: output=sql_show_create_table(P1)
 *
 * Generate the CREATE TABLE statement text that would be needed to
 * recreate the table P1. The text is returned in register P2.
 * An error message, if any, is returned in register P2+1.
 *
 * This opcode is used for the sqlite_master table and for dumping
 * schema information.
 *
 * Preconditions:
 * - P1 must be a valid space ID
 * - P2 must be a valid register number
 * - P2+1 must also be a valid register (for error output)
 */
int
vdbe_op_showcreatettable_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	struct Mem *ret = &aMem[pOp->p2];
	struct Mem *err = &aMem[pOp->p2 + 1];

	(void)p;

	sqlVdbeMemAboutToChange(p, ret);
	sqlVdbeMemAboutToChange(p, err);

	sql_show_create_table(aMem[pOp->p1].u.i, ret, err);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: RESETSORTER P1 * * * *
 * Synopsis: intkey=0
 *
 * Reset the sorter pointed to by cursor P1. If the cursor is not
 * pointing to a sorter, do nothing.
 *
 * This opcode is used to reset a sorter between multiple sort operations,
 * or to clean up after sorting is complete.
 *
 * Preconditions:
 * - P1 must be a valid cursor number (0 <= P1 < p->nCursor)
 * - p->apCsr[P1] must point to a valid VdbeCursor structure
 */
int
vdbe_op_resetsorter_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeCursor *pC;

	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != NULL);

	if (isSorter(pC)) {
		sqlVdbeSorterReset(pC->uc.pSorter);
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: SORT P1 P2 * * *
 * Synopsis: sort P1 in order, generate sort counter, fall-through to REWIND
 *
 * This opcode is a no-op except in test harness mode where it updates
 * sort and search counters. In normal execution, control falls through to
 * the next opcode (which should typically be OP_Rewind).
 *
 * The actual sorting is performed by cursor P1 when it's a sorter cursor.
 * This opcode just signals that a sort operation has been requested.
 *
 * Preconditions:
 * - P1 must be a valid cursor number
 * - The next opcode should handle the actual cursor positioning
 */
int
vdbe_op_sort_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;

	/* Test harness code - normally a no-op */
#ifdef SQL_TEST
	extern int sql_sort_count;
	extern int sql_search_count;
	sql_sort_count++;
	sql_search_count--;
#endif

	/*
	 * In the original code, this falls through to OP_Rewind.
	 * Since we're not using goto semantics, we'll just return
	 * and let the dispatcher continue naturally.
	 */

	return 0;  /* Continue to next instruction (falls through) */
}

/*
 * Opcode: CLEAR P1 P2 * * *
 * Synopsis: if P2>0 then clear table P1 (space)
 *
 * Delete all entries from space P1. If P2>0, the operation is performed.
 * If P2==0, this is a no-op.
 *
 * This opcode is used to implement TRUNCATE TABLE and to clear
 * temporary tables during query execution.
 *
 * Preconditions:
 * - P1 must be a valid space ID (> 0)
 * - P2 controls whether to actually perform the truncation
 */
int
vdbe_op_clear_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	uint32_t space_id;
	struct space *space;

	(void)p;
	(void)aMem;

	assert(pOp->p1 > 0);
	space_id = pOp->p1;
	space = space_by_id(space_id);
	assert(space != NULL);

	if (pOp->p2 > 0) {
		if (box_truncate(space_id) != 0) {
			return -1;  /* Error: truncate failed */
		}
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: PARAM P1 P2 * * *
 * Synopsis: r[P2]=parameter(P1)
 *
 * This opcode copies a parameter value from the enclosing frame into
 * register P2. The parameter index is P1, relative to the start of the
 * frame's parameter registers.
 *
 * Used for accessing parameters passed to subprograms and triggers.
 *
 * Preconditions:
 * - p->pFrame must be non-NULL (must be in a subprogram context)
 * - P1 must be a valid parameter index
 * - P2 must be a valid register number
 */
int
vdbe_op_param_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeFrame *pFrame;
	Mem *pIn;
	Mem *pOut;

	(void)aMem;

	pOut = vdbe_prepare_null_out(p, pOp->p2);
	pFrame = p->pFrame;

	assert(pFrame != NULL);
	assert(pOp->p1 >= 0);
	assert(pOp->p1 < pFrame->nChildMem);

	pIn = &pFrame->aMem[pOp->p1 + pFrame->aOp[pFrame->pc].p1];

	mem_copy_as_ephemeral(pOut, pIn);

	return 0;  /* Continue to next instruction */
}
