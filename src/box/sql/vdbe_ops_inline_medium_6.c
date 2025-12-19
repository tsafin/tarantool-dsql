/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 6
 * Phase 5.6g: Value loading, cursor management, and data retrieval opcodes
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (5 opcodes, 114-209 chars):
 * - OP_Decimal: Load decimal constant into register
 * - OP_OpenSpace: Create space reference cursor by ID lookup
 * - OP_Sequence: Get sequence counter value and increment
 * - OP_SequenceTest: Test sequence counter, jump if zero
 * - OP_Fetch: Fetch field value from record
 *
 * Helper dependencies:
 * - vdbe_prepare_null_out(): Initialize output register
 * - mem_set_dec(): Set decimal value in register
 * - space_by_id(): Look up space by ID
 * - mem_set_ptr(): Set pointer value in register
 * - mem_set_uint(): Set unsigned integer value in register
 * - vdbe_field_ref_fetch(): Fetch field from record
 * - isSorter(): Check if cursor is sorter type
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_helpers.h"

/*
 * Opcode: DECIMAL P1 P2 * * P4
 * Synopsis: r[P2]=P4 (decimal constant)
 *
 * Load the decimal value P4 into register P2. This opcode is used
 * to load decimal constants into memory during query execution.
 *
 * Preconditions:
 * - P2 must be a valid register number
 * - P4 contains the decimal value to load
 */
int
vdbe_op_decimal_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pOut;

	(void)aMem;

	/* Initialize output register */
	pOut = vdbe_prepare_null_out(p, pOp->p2);

	/* Set the decimal value from P4 */
	mem_set_dec(pOut, pOp->p4.dec);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: OPENSPACE P1 P2 * * *
 * Synopsis: space=space_by_id(P2); r[P1]=space
 *
 * Open a reference to the Tarantool space with ID P2 and store
 * the space pointer in register P1. This is used to prepare for
 * subsequent operations that need to access space metadata.
 *
 * Preconditions:
 * - P1 must be a valid register number (P1 > 0)
 * - P2 must be a valid space ID
 * - The space with ID P2 must exist
 */
int
vdbe_op_openspace_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	struct space *space;

	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p1 > 0);

	/* Look up the space by ID */
	space = space_by_id(pOp->p2);
	assert(space != NULL);

	/* Store the space pointer in the output register */
	mem_set_ptr(&aMem[pOp->p1], space);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: SEQUENCE P1 P2 * * *
 * Synopsis: r[P2]=sequence(P1); increment sequence counter
 *
 * Get the current value of the sequence counter from cursor P1,
 * store it in register P2, and then increment the counter.
 * This is used for tracking the position in iteration sequences.
 *
 * Preconditions:
 * - P1 must be a valid cursor number (0 <= P1 < p->nCursor)
 * - p->apCsr[P1] must point to a valid VdbeCursor structure
 * - P2 must be a valid register number
 */
int
vdbe_op_sequence_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pOut;
	int64_t seq_val;

	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(p->apCsr[pOp->p1] != NULL);

	/* Initialize output register */
	pOut = vdbe_prepare_null_out(p, pOp->p2);

	/* Get current sequence value and increment counter */
	seq_val = p->apCsr[pOp->p1]->seqCount++;

	/* Store the sequence value (before increment) in output register */
	mem_set_uint(pOut, (uint64_t)seq_val);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: SEQUENCETEST P1 P2 * * *
 * Synopsis: if (seqCount++ == 0) jump P2
 *
 * Test the sequence counter of cursor P1. Increment it, and if the
 * old value was 0 (meaning this is the first sequence), jump to P2.
 * This is used to execute initialization code only on the first
 * iteration through a sequence.
 *
 * Preconditions:
 * - P1 must be a valid cursor number (0 <= P1 < p->nCursor)
 * - p->apCsr[P1] must point to a valid VdbeCursor structure
 * - P2 must be a valid program counter offset (for jump)
 */
int
vdbe_op_sequencetest_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeCursor *pC;

	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != NULL);
	assert(isSorter(pC));

	/* Test and increment: if old value was 0, we need to jump */
	if ((pC->seqCount++) == 0) {
		/* Jump to P2 */
		return 1;  /* Special case: jump handled by dispatcher */
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: FETCH P1 P2 P3 * *
 * Synopsis: r[P3]=record[P1,P2]
 *
 * Fetch field P2 from the record referenced by P1 and store the
 * result in register P3. The field reference in P1 must be a valid
 * vdbe_field_ref structure that points to an unpacked record.
 *
 * Returns 0 on success or -1 on error (field fetch failed).
 *
 * Preconditions:
 * - P1 must point to a valid vdbe_field_ref structure
 * - P2 must be a valid field index in the record
 * - P3 must be a valid register number
 */
int
vdbe_op_fetch_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	struct vdbe_field_ref *ref;
	Mem *pRes;

	/* Get the field reference from input register */
	ref = aMem[pOp->p1].u.p;

	/* Initialize output register */
	pRes = vdbe_prepare_null_out(p, pOp->p3);

	/* Fetch the field value - error handling */
	if (vdbe_field_ref_fetch(ref, pOp->p2, pRes) != 0) {
		return -1;  /* Error: field fetch failed */
	}

	return 0;  /* Continue to next instruction */
}
