/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 6
 * Phase 5.6h: Control flow and advanced opcode handling
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (2 opcodes):
 * - OP_SequenceTest: Test sequence counter, jump if zero
 * - OP_Fetch: Fetch field value from record
 *
 * Note: OP_Decimal, OP_OpenSpace, and OP_Sequence are defined in
 * vdbe_ops_inline_medium_2.c to avoid duplicate definitions.
 *
 * Helper dependencies:
 * - vdbe_prepare_null_out(): Initialize output register
 * - vdbe_field_ref_fetch(): Fetch field from record
 * - isSorter(): Check if cursor is sorter type
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_helpers.h"
#include "tarantoolInt.h"
#include "space_cache.h"

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
