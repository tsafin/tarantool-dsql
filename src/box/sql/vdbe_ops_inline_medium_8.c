/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 8
 * Phase 5.8: Remaining opcodes - misc, DDL, and data access
 *
 * This file contains wrapper functions for the final batch of inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (12 opcodes):
 * - OP_ElseNotEq: Conditional jump if comparison was not equal
 * - OP_ResetCount: Reset VMs internal change counter
 * - OP_FCopy: Copy integer register across frames
 * - OP_FetchByName: Fetch tuple field by name
 * - OP_NextSystemSpaceId: Get next ID from system space
 * - OP_NextIdEphemeral: Get next rowid for ephemeral space
 * - OP_CreateForeignKey: Create foreign key constraint
 * - OP_CreateCheck: Create check constraint
 * - OP_AddFuncDefault: Add function as field default
 * - OP_CheckViewReferences: Verify no view references before drop
 * - OP_LoadAnalysis: Load sql_stat1 analysis (currently a no-op)
 * - OP_RenameTable: Rename table and rebuild trigger bodies
 *
 * Coroutine opcodes (Gosub, Return, Yield, InitCoroutine, EndCoroutine)
 * are implemented inline in vdbe_dispatch_wrapper.c because they require
 * direct pc/aOp manipulation in the dispatcher loop.
 *
 * OP_Program (trigger sub-programs) is handled by the generated dispatcher;
 * dispatcher due to its VdbeFrame setup complexity.
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_helpers.h"
#include "tarantoolInt.h"
#include "box/index.h"
#include "box/tuple.h"
#include "box/tuple_format.h"
#include "box/tuple_dictionary.h"
#include "box/space.h"
#include "box/space_cache.h"
#include "box/error.h"

/*
 * Opcode: ElseNotEq P2 * * * *
 * Synopsis: if compare!=0 goto P2
 *
 * This opcode must immediately follow an OP_Lt or OP_Gt comparison with
 * the SQL_STOREP2 flag set. If the comparison result (p->iCompare) is
 * non-zero (i.e., the values were not equal), jump to P2.
 *
 * Return value:
 * - 0: Values are equal, continue to next instruction
 * - 1: Values are not equal, jump to P2
 */
int
vdbe_op_elsenoteq_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(pOp > p->aOp);
	assert(pOp[-1].opcode == OP_Lt || pOp[-1].opcode == OP_Gt);
	assert(pOp[-1].p5 & SQL_STOREP2);
	return (p->iCompare != 0) ? 1 : 0;
}

/*
 * Opcode: ResetCount * * * * *
 *
 * The value of the change counter is copied to the database handle
 * change counter (returned by subsequent calls to sql_changes()).
 * Then the VM's internal change counter resets to 0.
 * This is used by trigger programs.
 *
 * Return value:
 * - 0: Normal completion
 */
int
vdbe_op_resetcount_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)pOp;
	(void)aMem;
	sqlVdbeSetChanges(p->nChange);
	p->nChange = 0;
	p->ignoreRaised = 0;
	return 0;
}

/*
 * Opcode: FCopy P1 P2 P3 * *
 * Synopsis: reg[P2@cur_frame]= reg[P1@root_frame(OPFLAG_SAME_FRAME)]
 *
 * Copy integer value of register P1 in root frame into register P2 of
 * current frame. If current frame is topmost, copy within single frame.
 * Source register must hold an integer value.
 *
 * P3 flags:
 * - OPFLAG_SAME_FRAME: Do shallow copy within the same frame
 * - OPFLAG_NOOP_IF_NULL: Do nothing (set NULL) if reg[P1] is NULL
 *
 * Return value:
 * - 0: Normal completion
 */
int
vdbe_op_fcopy_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeFrame *pFrame;
	Mem *pIn1, *pOut;

	if (p->pFrame && ((pOp->p3 & OPFLAG_SAME_FRAME) == 0)) {
		for (pFrame = p->pFrame; pFrame->pParent; pFrame = pFrame->pParent)
			;
		pIn1 = &pFrame->aMem[pOp->p1];
	} else {
		pIn1 = &aMem[pOp->p1];
	}

	if ((pOp->p3 & OPFLAG_NOOP_IF_NULL) != 0 && mem_is_null(pIn1)) {
		pOut = vdbe_prepare_null_out(p, pOp->p2);
		(void)pOut;
	} else {
		assert(memIsValid(pIn1));
		assert(mem_is_int(pIn1));
		pOut = vdbe_prepare_null_out(p, pOp->p2);
		mem_copy_as_ephemeral(pOut, pIn1);
	}
	return 0;
}

/*
 * Opcode: FetchByName P1 * P3 P4 *
 *
 * Interpret data P1 points at as an initialized vdbe_field_ref object.
 * Find the field whose name matches the string in P4, then store the
 * retrieved value in register P3.
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (field not found or fetch failed)
 */
int
vdbe_op_fetchbyname_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	struct vdbe_field_ref *ref = p->aMem[pOp->p1].u.p;
	assert(pOp->p4type == P4_DYNAMIC);
	uint32_t id;
	if (ref->format != NULL) {
		const char *name = pOp->p4.z;
		uint32_t len = strlen(name);
		uint32_t hash = field_name_hash(name, len);
		struct tuple_dictionary *dict = ref->format->dict;
		if (tuple_fieldno_by_name(dict, name, len, hash, &id) != 0) {
			diag_set(ClientError, ER_SQL_CANT_RESOLVE_FIELD, name);
			return -1;
		}
	} else {
		/*
		 * If no format is specified, assume the vdbe_field_ref
		 * contains one field for the field constraint, and assume
		 * this was already checked when the field constraint was
		 * created.
		 */
		assert(ref->field_count == 1);
		id = 0;
	}
	struct Mem *res = vdbe_prepare_null_out(p, pOp->p3);
	if (vdbe_field_ref_fetch(ref, id, res) != 0)
		return -1;
	(void)p;
	return 0;
}

/*
 * Opcode: NextSystemSpaceId P1 P2 P3 * *
 * Synopsis: r[P2]=next_id(space_id=P1, fieldno=P3)
 *
 * Get the maximum ID from system space P1 (BOX_SEQUENCE_ID or BOX_FUNC_ID)
 * and store max+1 in register P2. If the space is empty, store 1.
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error
 */
int
vdbe_op_nextsystemspaceid_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(pOp->p1 >= 0 && pOp->p3 >= 0);
	uint32_t space_id = pOp->p1;
	assert(space_id == BOX_SEQUENCE_ID || space_id == BOX_FUNC_ID);
	struct Mem *res = &p->aMem[pOp->p2];
	char key[1];
	struct tuple *tuple;
	char *key_end = mp_encode_array(key, 0);
	assert(key_end - key == 1);
	if (box_index_max(space_id, 0, key, key_end, &tuple) != 0)
		return -1;
	if (tuple == NULL) {
		mem_set_uint(res, 1);
		return 0;
	}
	uint32_t fieldno = pOp->p3;
	uint64_t id;
	if (tuple_field_u64(tuple, fieldno, &id) != 0)
		return -1;
	mem_set_uint(res, id + 1);
	return 0;
}

/*
 * Opcode: NextIdEphemeral P1 P2 * * *
 * Synopsis: r[P2]=get_next_rowid(space[P1])
 *
 * Store the next rowid for the ephemeral space pointed at by register P1
 * into register P2. The rowid is required because tuples inserted into an
 * ephemeral space may not be unique on their own, so a synthetic rowid
 * makes the PK unique.
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (overflow or vtab failure)
 */
int
vdbe_op_nextidephemeral_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	struct space *space = (struct space *)p->aMem[pOp->p1].u.p;
	assert(space->def->id == 0);
	uint64_t rowid;
	if (space->vtab->ephemeral_rowid_next(space, &rowid) != 0)
		return -1;
	if (rowid > INT32_MAX) {
		diag_set(ClientError, ER_ROWID_OVERFLOW);
		return -1;
	}
	struct Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	mem_set_uint(pOut, rowid);
	return 0;
}

/*
 * Opcode: CreateForeignKey P1 P2 * P4 *
 *
 * Create a new FOREIGN KEY constraint. The constraint name is stored in P4.
 * Registers starting at P1 hold: child_id, parent_id, and either
 * child/parent fieldnos (as integers) or a mapping (as a map value).
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error
 */
int
vdbe_op_createforeignkey_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(pOp->p1 >= 0);
	struct Mem *mems = &p->aMem[pOp->p1];
	assert(mem_is_uint(&mems[0]) && mem_is_uint(&mems[1]));
	uint32_t child_id = mems[0].u.u;
	uint32_t parent_id = mems[1].u.u;
	const char *name = pOp->p4.z;
	const char *mapping = NULL;
	uint32_t child_fieldno = 0;
	uint32_t parent_fieldno = 0;
	if (mem_is_uint(&mems[2])) {
		assert(mem_is_uint(&mems[3]));
		child_fieldno = mems[2].u.u;
		parent_fieldno = mems[3].u.u;
	} else {
		assert(mem_is_map(&mems[2]));
		mapping = mems[2].z;
	}
	if (sql_foreign_key_create(name, child_id, parent_id, child_fieldno,
				   parent_fieldno, mapping) != 0)
		return -1;
	if (p->nChange == 0)
		p->nChange = 1;
	return 0;
}

/*
 * Opcode: CreateCheck P1 P2 P3 P4 P5
 *
 * Create a new check constraint. The check name is stored in P4. Register
 * r[P1] contains the ID of the space, register r[P2] contains the ID of
 * the function. If P5 is not 0 then P3 is the fieldno of the field
 * containing this check.
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error
 */
int
vdbe_op_createcheck_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(pOp->p1 >= 0 && pOp->p2 >= 0 && pOp->p3 >= 0);
	uint32_t space_id = p->aMem[pOp->p1].u.u;
	uint32_t func_id = p->aMem[pOp->p2].u.u;
	const char *name = pOp->p4.z;
	bool is_field_ck = pOp->p5 != 0;
	uint32_t fieldno = pOp->p3;
	if (sql_check_create(name, space_id, func_id, fieldno,
			     is_field_ck) != 0)
		return -1;
	if (p->nChange == 0)
		p->nChange = 1;
	return 0;
}

/*
 * Opcode: AddFuncDefault P1 P2 P3 * *
 * Synopsis: Add function r[P2] as default for field P3 of box.space[r[P1]]
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error
 */
int
vdbe_op_addfuncdefault_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(p->aMem[pOp->p1].type == MEM_TYPE_UINT);
	uint32_t space_id = p->aMem[pOp->p1].u.u;
	uint32_t fieldno = pOp->p3;
	assert(p->aMem[pOp->p2].type == MEM_TYPE_UINT);
	uint32_t func_id = p->aMem[pOp->p2].u.u;
	if (sql_add_default(space_id, fieldno, func_id) != 0)
		return -1;
	return 0;
}

/*
 * Opcode: CheckViewReferences P1 * * * *
 * Synopsis: r[P1] = space id
 *
 * Check that the space to be dropped has no view references. Raises an
 * error if any other views depend on this space.
 *
 * Return value:
 * - 0: Normal completion (no view references)
 * - -1: Error (view references exist or space not found)
 */
int
vdbe_op_checkviewreferences_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(pOp->p1 > 0);
	Mem *pIn1 = &p->aMem[pOp->p1];
	uint64_t space_id = pIn1->u.u;
	assert(space_id <= INT32_MAX);
	struct space *space = space_by_id(space_id);
	assert(space != NULL);
	if (space->def->view_ref_count > 0) {
		diag_set(ClientError, ER_DROP_SPACE, space->def->name,
			 "other views depend on this space");
		return -1;
	}
	return 0;
}

/*
 * Opcode: LoadAnalysis P1 * * * *
 *
 * Read the sql_stat1 table for database P1 and load the content of that
 * table into the internal index hash table. Currently a no-op pending
 * full statistics implementation.
 *
 * Return value:
 * - 0: Normal completion (no-op)
 */
int
vdbe_op_loadanalysis_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)aMem;
	assert(pOp->p1 == 0);
	/* TODO: Enable analysis when sql_analysis_load() is available. */
	return 0;
}

/*
 * Opcode: RenameTable P1 * * P4 *
 *
 * Rename the space with ID P1 to the name stored in P4. Also rebuilds
 * all trigger CREATE statements that reference the old table name.
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (rename or trigger rebuild failed)
 */
int
vdbe_op_renametable_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)aMem;
	uint32_t space_id = pOp->p1;
	struct space *space = space_by_id(space_id);
	assert(space != NULL);
	struct sql_trigger *triggers = space->sql_triggers;
	assert(space->def->name != NULL);
	const char *zNewTableName = pOp->p4.z;
	char *zOldTableName = sql_xstrdup(space_name(space));
	if (sql_rename_table(space_id, zNewTableName) != 0) {
		sql_xfree(zOldTableName);
		return -1;
	}
	/*
	 * Rebuild 'CREATE TRIGGER' expressions of all triggers
	 * created on this table. This action is not atomic due to
	 * lack of transactional DDL, but best effort is made.
	 */
	for (struct sql_trigger *trigger = triggers; trigger != NULL; ) {
		struct sql_trigger *next_trigger = trigger->next;
		if (tarantoolsqlRenameTrigger(trigger->zName, zOldTableName,
					      zNewTableName) != 0) {
			sql_xfree(zOldTableName);
			return -1;
		}
		trigger = next_trigger;
	}
	sql_xfree(zOldTableName);
	return 0;
}
