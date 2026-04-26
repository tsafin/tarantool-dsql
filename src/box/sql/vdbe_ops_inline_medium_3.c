/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 3
 * Phase 5.6d: Constraint drop and transaction opcodes
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (9 opcodes, 100-180 chars):
 * - OP_Savepoint: Manage savepoints (BEGIN/RELEASE/ROLLBACK)
 * - OP_TransactionBegin: Start new transaction
 * - OP_TransactionRollback: Rollback current transaction
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
 * - txn_begin(): Start new transaction
 * - txn_commit(): Transaction commit operation
 * - txn_savepoint_new(): Create new savepoint
 * - txn_savepoint_by_name(): Find savepoint by name
 * - txn_savepoint_release(): Release savepoint
 * - box_txn_rollback(): Rollback transaction
 * - box_txn_rollback_to_savepoint(): Rollback to savepoint
 * - in_txn(): Get current transaction
 * - box_generate_space_id(): Generate space ID from box module
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "tarantoolInt.h"
#include "box/txn.h"
#include "box/box.h"

/*
 * Opcode: SAVEPOINT - Manage savepoints (BEGIN/RELEASE/ROLLBACK)
 *
 * Manage savepoints within a transaction. P1 specifies the operation:
 * SAVEPOINT_BEGIN (1) - Create new savepoint with name in P4
 * SAVEPOINT_RELEASE (2) - Release savepoint with name in P4
 * SAVEPOINT_ROLLBACK (3) - Rollback to savepoint with name in P4
 *
 * Preconditions:
 * - Must be in a transaction (ER_NO_TRANSACTION if not)
 * - P1 must be valid savepoint operation (SAVEPOINT_BEGIN/RELEASE/ROLLBACK)
 * - P4 must point to savepoint name (null-terminated string)
 * - P3 optional: alternate savepoint name register for old names
 */
int
vdbe_op_savepoint_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	int p1;
	char *zName;
	struct txn *txn;

	(void)p;  /* Not used - savepoint doesn't modify Vdbe state */

	txn = in_txn();

	if (txn == NULL) {
		diag_set(ClientError, ER_NO_TRANSACTION);
		return -1;
	}

	p1 = pOp->p1;
	zName = pOp->p4.z;

	/* Validate savepoint operation type */
	if (p1 != SAVEPOINT_BEGIN && p1 != SAVEPOINT_RELEASE &&
	    p1 != SAVEPOINT_ROLLBACK) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			 "invalid savepoint operation");
		return -1;
	}
	assert(rlist_empty(&txn->savepoints) || box_txn());

	if (p1 == SAVEPOINT_BEGIN) {
		/* Create new savepoint - name stored in txn, we just need to create it */
		if (txn_savepoint_new(txn, zName) == NULL)
			return -1;
	} else {
		/* Find the named savepoint. If not found, try alternate name in P3 */
		struct txn_savepoint *sv = txn_savepoint_by_name(txn, zName);
		if (sv == NULL && pOp->p3 > 0) {
			struct Mem *old_name = &aMem[pOp->p3];
			sv = txn_savepoint_by_name(txn, old_name->z);
		}
		if (sv == NULL) {
			diag_set(ClientError, ER_NO_SUCH_SAVEPOINT);
			return -1;
		}

		if (p1 == SAVEPOINT_RELEASE) {
			txn_savepoint_release(sv);
		} else {
			assert(p1 == SAVEPOINT_ROLLBACK);
			if (box_txn_rollback_to_savepoint(sv) != 0)
				return -1;
		}
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: TRANSACTIONBEGIN - Start new transaction
 *
 * Begin a new Tarantool transaction. Only valid if not already in
 * a transaction. If already in a transaction, raises ER_ACTIVE_TRANSACTION.
 *
 * Side effects:
 * - Sets p->auto_commit = false (disables auto-commit mode)
 */
int
vdbe_op_transactionbegin_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)pOp;
	(void)aMem;

	if (in_txn()) {
		diag_set(ClientError, ER_ACTIVE_TRANSACTION);
		return -1;
	}

	struct txn *txn = txn_begin();
	if (txn == NULL) {
		return -1;
	}

	p->auto_commit = false;

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: TRANSACTIONROLLBACK - Rollback current transaction
 *
 * Rollback the current transaction. Only valid if in a transaction.
 * If not in a transaction, raises ER_SQL_EXECUTE error.
 *
 * Preconditions:
 * - Must be in a transaction (error if not via box_txn())
 */
int
vdbe_op_transactionrollback_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;

	if (box_txn()) {
		int rc = box_txn_rollback();
		if (rc != 0) {
			return -1;
		}
	} else {
		diag_set(ClientError, ER_SQL_EXECUTE, "cannot rollback - no "\
			 "transaction is active");
		return -1;
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: TRANSACTIONCOMMIT - Commit current transaction
 *
 * Commit the current transaction. If there is no active transaction, raise
 * ER_SQL_EXECUTE. If the commit fails, return -1 (error).
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
	} else {
		diag_set(ClientError, ER_SQL_EXECUTE, "cannot commit - no "
			 "transaction is active");
		return -1;
	}

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: TTRANSACTION - Start statement transaction or savepoint
 *
 * Start Tarantool's transaction for an auto-generated DML statement. If an
 * outer transaction is already active, create an anonymous savepoint instead
 * so ABORT can roll back only the current statement.
 */
int
vdbe_op_ttransaction_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)pOp;
	(void)aMem;

	if (!box_txn()) {
		if (txn_begin() == NULL)
			return -1;
	} else {
		p->anonymous_savepoint = txn_savepoint_new(in_txn(), NULL);
		if (p->anonymous_savepoint == NULL)
			return -1;
	}

	return 0;
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
	(void)aMem;

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
