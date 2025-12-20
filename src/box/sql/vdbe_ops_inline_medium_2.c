/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 2
 * Phase 5.6c: Extract helper functions and implement medium opcodes
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (4 opcodes):
 * - OP_Decimal: Load decimal constant to register
 * - OP_AddImm: Add immediate value to register
 * - OP_Sequence: Get next sequence value from cursor
 * - OP_OpenSpace: Open space cursor (uses space_by_id from box/space.h)
 *
 * Helper dependencies:
 * - vdbe_prepare_null_out(): Initialize register with cleared NULL (extracted to vdbe_helpers.h)
 * - sqlVdbeMemAboutToChange(): Mark register as changed (exposed in vdbe_helpers.h)
 * - space_by_id(): Lookup space object (from box/space.h)
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "box/space.h"
#include "box/space_cache.h"

/*
 * Opcode: DECIMAL - Load decimal constant
 *
 * Write the decimal value P4 to register P2.
 *
 * Flags: OUT2
 */
int
vdbe_op_decimal_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;

	Mem *pOut;

	/* P2 is the destination register */
	pOut = &aMem[pOp->p2];

	/* Initialize output register as cleared NULL, then set decimal value */
	mem_set_null(pOut);
	mem_set_dec(pOut, pOp->p4.dec);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: ADDIMM - Add immediate value to register
 *
 * Add the constant integer P2 to the value in register P1.
 * The result is stored back in register P1.
 *
 * Preconditions:
 * - Register P1 must contain an unsigned integer value
 * - P2 must be >= 0
 *
 * Flags: IN1
 */
int
vdbe_op_addimm_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1;

	/* P1 is the register to modify */
	pIn1 = &aMem[pOp->p1];

	/* Mark register as about to change (for SCopy tracking) */
	sqlVdbeMemAboutToChange(p, pIn1);

	/* P1 must contain unsigned integer, P2 must be non-negative */
	assert(mem_is_uint(pIn1) && pOp->p2 >= 0);

	/* Add P2 to the register value */
	pIn1->u.u += pOp->p2;

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: SEQUENCE - Get next sequence value from cursor
 *
 * Find the next unused sequence number for the sequence generator
 * associated with cursor P1 and write the integer to register P2.
 *
 * The cursor P1 must be valid and have a sequence counter initialized.
 * This opcode is typically used with auto-increment columns.
 *
 * Flags: IN1, OUT2
 */
int
vdbe_op_sequence_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pOut;
	int64_t seq_val;

	/* P1 must be a valid cursor number */
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(p->apCsr[pOp->p1] != NULL);

	/* P2 is the destination register */
	pOut = &aMem[pOp->p2];

	/* Initialize output register as cleared NULL */
	mem_set_null(pOut);

	/* Get next sequence value from cursor counter and increment */
	seq_val = p->apCsr[pOp->p1]->seqCount++;

	/* Store the sequence value in the output register */
	mem_set_uint(pOut, seq_val);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: OPENSPACE - Open table space cursor
 *
 * Open a new cursor for a table space. P1 is the cursor number to allocate.
 * P2 is the space ID. P3 is a flag controlling the cursor type.
 *
 * The cursor is initialized to reference the table space identified by P2.
 * This opcode is used to set up access to Tarantool table spaces.
 *
 * Flags: IN3
 */
int
vdbe_op_openspace_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)aMem;

	struct space *space;

	/* P1 must be a valid cursor slot (positive and not yet used) */
	assert(pOp->p1 >= 0 && pOp->p1 > 0);

	/* Look up the space by ID in P2 */
	space = space_by_id(pOp->p2);

	/* Space must exist */
	assert(space != NULL);

	/* Store the space pointer in the cursor register (aMem[P1]) */
	mem_set_ptr(&aMem[pOp->p1], space);

	return 0;  /* Continue to next instruction */
}
