/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 3
 * Phase 5.6d: Constraint drop and transaction opcodes
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (6 opcodes, 100-180 chars):
 * - OP_TransactionCommit: Commit current transaction
 * - OP_DropTupleCheck: Drop tuple-level check constraint
 * - OP_DropTupleForeignKey: Drop tuple-level foreign key constraint
 * - OP_DropFieldCheck: Drop field-level check constraint
 * - OP_DropFieldForeignKey: Drop field-level foreign key constraint
 * - OP_GenSpaceid: Generate unique space ID
 *
 * Helper dependencies:
 * - vdbe_prepare_null_out(): Initialize register with cleared NULL (from vdbe_helpers.h)
 * - mem_set_uint(): Set unsigned integer value (standard mem interface)
 * - sql_tuple_check_drop(): SQL constraint helper
 * - sql_tuple_foreign_key_drop(): SQL constraint helper
 * - sql_field_check_drop(): SQL constraint helper
 * - sql_field_foreign_key_drop(): SQL constraint helper
 * - txn_commit(): Transaction commit operation
 * - in_txn(): Get current transaction
 * - box_generate_space_id(): Generate space ID from box module
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "box/txn.h"

/*
 * Opcode: TRANSACTIONCOMMIT - Commit current transaction
 *
 * Commit the current transaction. If the current transaction is NULL (not
 * in a transaction), this operation does nothing. If the commit fails,
 * go to abort_due_to_error.
 *
 * Transaction management must happen correctly for data consistency.
 */
int
vdbe_op_transactioncommit_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;

	struct txn *txn = in_txn();

	if (txn != NULL) {
		if (txn_commit(txn) != 0) {
			/* Transaction commit failed - return error */
			return -1;
		}
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: DROPTUPLECHECK - Drop tuple-level check constraint
 *
 * Drop a check constraint at the tuple level. The constraint ID is in P1,
 * and the constraint name (for debugging/logging) is in P4.
 *
 * Preconditions:
 * - P1 must be >= 0 (valid constraint ID)
 * - P4 must point to a valid null-terminated string (constraint name)
 *
 * Side effects:
 * - Sets p->nChange to 1 if successful (signals schema change)
 */
int
vdbe_op_droptuplecheckundidocheck_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p4.z != NULL);

	if (sql_tuple_check_drop(pOp->p1, pOp->p4.z) != 0) {
		/* Constraint drop failed - return error */
		return -1;
	}

	/* Mark that schema has changed (nChange must be 0 before operation) */
	assert(p->nChange == 0);
	p->nChange = 1;

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: DROPTUPLEFOREIGNKEY - Drop tuple-level foreign key constraint
 *
 * Drop a foreign key constraint at the tuple level. The constraint ID is in P1,
 * and the constraint name (for debugging/logging) is in P4.
 *
 * Preconditions:
 * - P1 must be >= 0 (valid constraint ID)
 * - P4 must point to a valid null-terminated string (constraint name)
 *
 * Side effects:
 * - Sets p->nChange to 1 if successful (signals schema change)
 */
int
vdbe_op_droptupleforeignkey_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p4.z != NULL);

	if (sql_tuple_foreign_key_drop(pOp->p1, pOp->p4.z) != 0) {
		/* Constraint drop failed - return error */
		return -1;
	}

	/* Mark that schema has changed (nChange must be 0 before operation) */
	assert(p->nChange == 0);
	p->nChange = 1;

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: DROPFIELDCHECK - Drop field-level check constraint
 *
 * Drop a check constraint at the field level. The constraint ID is in P1,
 * the field index is in P3, and the constraint name (for debugging/logging)
 * is in P4.
 *
 * Preconditions:
 * - P1 must be >= 0 (valid constraint ID)
 * - P3 must be a valid field index
 * - P4 must point to a valid null-terminated string (constraint name)
 *
 * Side effects:
 * - Sets p->nChange to 1 if successful (signals schema change)
 */
int
vdbe_op_dropfieldcheck_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p4.z != NULL);

	if (sql_field_check_drop(pOp->p1, pOp->p3, pOp->p4.z) != 0) {
		/* Constraint drop failed - return error */
		return -1;
	}

	/* Mark that schema has changed (nChange must be 0 before operation) */
	assert(p->nChange == 0);
	p->nChange = 1;

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: DROPFIELDFOREIGNKEY - Drop field-level foreign key constraint
 *
 * Drop a foreign key constraint at the field level. The constraint ID is in P1,
 * the field index is in P3, and the constraint name (for debugging/logging)
 * is in P4.
 *
 * Preconditions:
 * - P1 must be >= 0 (valid constraint ID)
 * - P3 must be a valid field index
 * - P4 must point to a valid null-terminated string (constraint name)
 *
 * Side effects:
 * - Sets p->nChange to 1 if successful (signals schema change)
 */
int
vdbe_op_dropfieldforeignkey_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;

	assert(pOp->p1 >= 0 && pOp->p4.z != NULL);

	if (sql_field_foreign_key_drop(pOp->p1, pOp->p3, pOp->p4.z) != 0) {
		/* Constraint drop failed - return error */
		return -1;
	}

	/* Mark that schema has changed (nChange must be 0 before operation) */
	assert(p->nChange == 0);
	p->nChange = 1;

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: GENSPACEID - Generate unique space ID
 *
 * Generate a unique space ID and store it in register P1.
 * The space ID is obtained from the box module and is guaranteed to be unique.
 *
 * Preconditions:
 * - P1 must be > 0 (valid register number)
 *
 * Side effects:
 * - Register P1 is cleared and set to the generated space ID (unsigned integer)
 */
int
vdbe_op_genspaceid_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pOut;
	uint32_t u;

	assert(pOp->p1 > 0);

	/* Initialize output register and get pointer */
	pOut = vdbe_prepare_null_out(p, pOp->p1);

	/* Generate unique space ID from box module */
	if (box_generate_space_id(&u, false) != 0) {
		/* ID generation failed - return error */
		return -1;
	}

	/* Store generated ID in output register */
	mem_set_uint(pOut, u);

	return 0;  /* Continue to next instruction */
}
