/* Index operation opcode handlers extracted from vdbe.c
 *
 * These handlers implement index operations including:
 * - Index comparisons (IdxGE, IdxGT, IdxLE, IdxLT)
 * - Index lookups (Found, NotFound, NoConflict)
 * - Index modifications (IdxInsert, IdxReplace)
 *
 * Return values for index comparison handlers:
 * - 0: Continue normally (no jump)
 * - 1: Jump to P2 (condition met)
 * - -1: Error occurred
 *
 * Return values for index lookup handlers:
 * - 0: Continue normally
 * - 1: Jump to P2
 * - -1: Error occurred
 *
 * Return values for index insert/replace handlers:
 * - 0: Success
 * - -1: Error occurred
 */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "box/error.h"
#include "tarantoolInt.h"
#include "box/sequence.h"

#ifdef SQL_TEST
extern int sql_found_count;
#endif

/* Return values for index handlers */
#define VDBE_INDEX_CONTINUE  0   /* Continue to next instruction */
#define VDBE_INDEX_JUMP      1   /* Jump to P2 */
#define VDBE_INDEX_ERROR    -1   /* Error occurred */

/* Opcode: IdxGE P1 P2 P3 P4 P5
 * Synopsis: key=r[P3@P4]
 *
 * The P4 register values beginning with P3 form an unpacked index
 * key that omits the PRIMARY KEY.  Compare this key value against the index
 * that P1 is currently pointing to, ignoring the PRIMARY KEY
 * fields at the end.
 *
 * If the P1 index entry is greater than or equal to the key value
 * then jump to P2.  Otherwise fall through to the next instruction.
 */
/* Opcode: IdxGT P1 P2 P3 P4 P5
 * Synopsis: key=r[P3@P4]
 *
 * The P4 register values beginning with P3 form an unpacked index
 * key that omits the PRIMARY KEY.  Compare this key value against the index
 * that P1 is currently pointing to, ignoring the PRIMARY KEY
 * fields at the end.
 *
 * If the P1 index entry is greater than the key value
 * then jump to P2.  Otherwise fall through to the next instruction.
 */
/* Opcode: IdxLT P1 P2 P3 P4 P5
 * Synopsis: key=r[P3@P4]
 *
 * The P4 register values beginning with P3 form an unpacked index
 * key that omits the PRIMARY KEY.  Compare this key value against
 * the index that P1 is currently pointing to, ignoring the PRIMARY KEY
 * on the P1 index.
 *
 * If the P1 index entry is less than the key value then jump to P2.
 * Otherwise fall through to the next instruction.
 */
/* Opcode: IdxLE P1 P2 P3 P4 P5
 * Synopsis: key=r[P3@P4]
 *
 * The P4 register values beginning with P3 form an unpacked index
 * key that omits the PRIMARY KEY.  Compare this key value against
 * the index that P1 is currently pointing to, ignoring the PRIMARY KEY
 * on the P1 index.
 *
 * If the P1 index entry is less than or equal to the key value then jump
 * to P2. Otherwise fall through to the next instruction.
 */
int
vdbe_op_idx_compare(Vdbe *p, Op *pOp, Mem *aMem)
{
	struct VdbeCursor *pC;
	UnpackedRecord r;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	assert(pC->uc.pCursor != 0);
	assert(pOp->p5 == 0 || pOp->p5 == 1);
	assert(pOp->p4type == P4_INT32);
	r.key_def = pC->key_def;
	r.nField = (u16)pOp->p4.i;
	if (pOp->opcode < OP_IdxLT) {
		assert(pOp->opcode == OP_IdxLE || pOp->opcode == OP_IdxGT);
		r.default_rc = -1;
	} else {
		assert(pOp->opcode == OP_IdxGE || pOp->opcode == OP_IdxLT);
		r.default_rc = 0;
	}
	r.aMem = &aMem[pOp->p3];
#ifdef SQL_DEBUG
	{
		int i;
		for(i = 0; i < r.nField; i++)
			assert(memIsValid(&r.aMem[i]));
	}
#endif
	int res = tarantoolsqlIdxKeyCompare(pC->uc.pCursor, &r);
	assert((OP_IdxLE & 1) == (OP_IdxLT & 1) &&
		(OP_IdxGE & 1) == (OP_IdxGT & 1));
	if ((pOp->opcode & 1) == (OP_IdxLT & 1)) {
		assert(pOp->opcode == OP_IdxLE || pOp->opcode == OP_IdxLT);
		res = -res;
	} else {
		assert(pOp->opcode == OP_IdxGE || pOp->opcode == OP_IdxGT);
		res++;
	}
	if (res > 0)
		return VDBE_INDEX_JUMP;
	return VDBE_INDEX_CONTINUE;
}

/* Opcode: Found P1 P2 P3 P4 *
 * Synopsis: key=r[P3@P4]
 *
 * If P4==0 then register P3 holds a blob constructed by MakeRecord.  If
 * P4>0 then register P3 is the first of P4 registers that form an unpacked
 * record.
 *
 * Cursor P1 is on an index btree.  If the record identified by P3 and P4
 * is a prefix of any entry in P1 then a jump is made to P2 and
 * P1 is left pointing at the matching entry.
 *
 * This operation leaves the cursor in a state where it can be
 * advanced in the forward direction.  The Next instruction will work,
 * but not the Prev instruction.
 *
 * See also: NotFound, NoConflict, NotExists. SeekGe
 */
/* Opcode: NotFound P1 P2 P3 P4 *
 * Synopsis: key=r[P3@P4]
 *
 * If P4==0 then register P3 holds a blob constructed by MakeRecord.  If
 * P4>0 then register P3 is the first of P4 registers that form an unpacked
 * record.
 *
 * Cursor P1 is on an index btree.  If the record identified by P3 and P4
 * is not the prefix of any entry in P1 then a jump is made to P2.  If P1
 * does contain an entry whose prefix matches the P3/P4 record then control
 * falls through to the next instruction and P1 is left pointing at the
 * matching entry.
 *
 * This operation leaves the cursor in a state where it cannot be
 * advanced in either direction.  In other words, the Next and Prev
 * opcodes do not work after this operation.
 *
 * See also: Found, NotExists, NoConflict
 */
/* Opcode: NoConflict P1 P2 P3 P4 *
 * Synopsis: key=r[P3@P4]
 *
 * If P4==0 then register P3 holds a blob constructed by MakeRecord.  If
 * P4>0 then register P3 is the first of P4 registers that form an unpacked
 * record.
 *
 * Cursor P1 is on an index btree.  If the record identified by P3 and P4
 * contains any NULL value, jump immediately to P2.  If all terms of the
 * record are not-NULL then a check is done to determine if any row in the
 * P1 index btree has a matching key prefix.  If there are no matches, jump
 * immediately to P2.  If there is a match, fall through and leave the P1
 * cursor pointing to the matching row.
 *
 * This opcode is similar to OP_NotFound with the exceptions that the
 * branch is always taken if any part of the search key input is NULL.
 *
 * This operation leaves the cursor in a state where it cannot be
 * advanced in either direction.  In other words, the Next and Prev
 * opcodes do not work after this operation.
 *
 * See also: NotFound, Found, NotExists
 */
int
vdbe_op_found_notfound_noconflict(Vdbe *p, Op *pOp, Mem *aMem)
{
	int alreadyExists;
	int takeJump;
	int ii;
	struct VdbeCursor *pC;
	int res;
	UnpackedRecord *pFree;
	UnpackedRecord *pIdxKey;
	UnpackedRecord r;
	Mem *pIn3;

#ifdef SQL_TEST
	if (pOp->opcode != OP_NoConflict)
		sql_found_count++;
#endif

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(pOp->p4type == P4_INT32);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);
#ifdef SQL_DEBUG
	pC->seekOp = pOp->opcode;
#endif
	pIn3 = &aMem[pOp->p3];
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	assert(pC->uc.pCursor != 0);
	if (pOp->p4.i > 0) {
		r.key_def = pC->key_def;
		r.nField = (u16)pOp->p4.i;
		r.aMem = pIn3;
#ifdef SQL_DEBUG
		for(ii = 0; ii < r.nField; ii++) {
			assert(memIsValid(&r.aMem[ii]));
		}
#endif
		pIdxKey = &r;
		pFree = 0;
	} else {
		pIdxKey = sqlVdbeAllocUnpackedRecord(pC->key_def);
		pFree = pIdxKey;
		assert(mem_is_bin(pIn3));
		sqlVdbeRecordUnpackMsgpack(pC->key_def,
					       pIn3->z, pIdxKey);
	}
	pIdxKey->default_rc = 0;
	pIdxKey->opcode = pOp->opcode;
	takeJump = 0;
	if (pOp->opcode == OP_NoConflict) {
		/* For the OP_NoConflict opcode, take the jump if any of the
		 * input fields are NULL, since any key with a NULL will not
		 * conflict
		 */
		for(ii = 0; ii < pIdxKey->nField; ii++) {
			if (mem_is_null(&pIdxKey->aMem[ii])) {
				takeJump = 1;
				break;
			}
		}
	}
	pC->uc.pCursor->iter_type = ITER_EQ;
	if (sql_cursor_seek(pC->uc.pCursor, pIdxKey->aMem, pIdxKey->nField,
			    &res) != 0) {
		if (pFree != NULL)
			sql_xfree(pFree);
		return VDBE_INDEX_ERROR;
	}
	if (pFree != NULL)
		sql_xfree(pFree);
	pC->seekResult = res;
	alreadyExists = (res == 0);
	pC->nullRow = 1 - alreadyExists;
	pC->cacheStatus = CACHE_STALE;
	if (pOp->opcode == OP_Found) {
		if (alreadyExists)
			return VDBE_INDEX_JUMP;
	} else {
		if (takeJump || !alreadyExists)
			return VDBE_INDEX_JUMP;
	}
	return VDBE_INDEX_CONTINUE;
}

/* Opcode: IdxInsert P1 P2 P3 * P5
 * Synopsis: key=r[P1]
 *
 * @param P1 Index of a register with MessagePack data to insert.
 * @param P2 Register containing pointer to space to insert into.
 * @param P3 If not 0, than it is an index of a register that
 *           contains value that will be inserted into field with
 *           AUTOINCREMENT. If the value is NULL, than the newly
 *           generated autoincrement value will be saved to VDBE
 *           context.
 * @param P5 Flags. If P5 contains OPFLAG_NCHANGE, then VDBE
 *        accounts the change in a case of successful insertion in
 *        nChange counter. If P5 contains OPFLAG_OE_IGNORE, then
 *        we are processing INSERT OR INGORE statement. Thus, in
 *        case of conflict we don't raise an error.
 */
/* Opcode: IdxReplace P1 P2 P3 * P5
 * Synopsis: key=r[P1]
 *
 * This opcode works exactly as IdxInsert does, but in Tarantool
 * internals it invokes box_replace() instead of box_insert().
 */
int
vdbe_op_idx_insert_replace(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn2;
	int rc;

	pIn2 = &aMem[pOp->p1];
	assert(mem_is_bin(pIn2));
	struct space *space = aMem[pOp->p2].u.p;
	assert(space != NULL);
	if (space->def->id != 0) {
		/* Make sure that memory has been allocated on region. */
		assert(mem_is_ephemeral(&aMem[pOp->p1]));
		if (pOp->opcode == OP_IdxInsert) {
			rc = tarantoolsqlInsert(space, pIn2->z,
						pIn2->z + pIn2->n);
		} else {
			rc = tarantoolsqlReplace(space, pIn2->z,
						pIn2->z + pIn2->n);
		}
	} else {
		rc = tarantoolsqlEphemeralInsert(space, pIn2->z,
						pIn2->z + pIn2->n);
	}
	if (rc != 0) {
		if ((pOp->p5 & OPFLAG_OE_IGNORE) != 0) {
			/*
			 * Ignore any kind of fails and do not
			 * raise error message. If we are in
			 * trigger, increment ignore raised
			 * counter.
			 */
			rc = 0;
			if (p->pFrame != NULL)
				p->ignoreRaised++;
			return VDBE_INDEX_CONTINUE;
		}
		if ((pOp->p5 & OPFLAG_OE_FAIL) != 0) {
			p->errorAction = ON_CONFLICT_ACTION_FAIL;
		} else if ((pOp->p5 & OPFLAG_OE_ROLLBACK) != 0) {
			p->errorAction = ON_CONFLICT_ACTION_ROLLBACK;
		}
		return VDBE_INDEX_ERROR;
	}
	if ((pOp->p5 & OPFLAG_NCHANGE) != 0)
		p->nChange++;
	if (pOp->p3 > 0 && mem_is_null(&aMem[pOp->p3])) {
		assert(space->sequence != NULL);
		int64_t value;
		if (sequence_get_value(space->sequence, &value) != 0)
			return VDBE_INDEX_ERROR;
		if (vdbe_add_new_autoinc_id(p, value) != 0)
			return VDBE_INDEX_ERROR;
	}
	return VDBE_INDEX_CONTINUE;
}
