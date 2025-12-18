/* Data modification opcode handlers extracted from vdbe.c
 *
 * These handlers implement data modification operations including:
 * - Table row deletion (Delete)
 * - Table row update (Update)
 * - System space insertion (SInsert)
 * - System space deletion (SDelete)
 * - Index entry deletion (IdxDelete)
 *
 * Return values:
 * - 0: Success
 * - -1: Error occurred
 */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "box/box.h"
#include "box/error.h"
#include "box/space.h"
#include "box/space_cache.h"
#include "tarantoolInt.h"
#include "fiber.h"
#include "msgpuck.h"
#include "mpstream/mpstream.h"

/* Opcode: Delete P1 P2 P3 P4 *
 * Synopsis: delete row at cursor P1
 *
 * Delete the record at which the P1 cursor is currently pointing.
 *
 * If the OPFLAG_SAVEPOSITION bit of P5 is set, then the cursor will be
 * left pointing at  either the next or the previous
 * record in the table. If it is left pointing at the next record, then
 * the next Next instruction will be a no-op. As a result, in this case
 * it is ok to delete a record from within a Next loop. If
 * OPFLAG_SAVEPOSITION bit of P5 is clear, then the cursor will be
 * left in an undefined state.
 *
 * If the OPFLAG_AUXDELETE bit is set on P5, that indicates that this
 * delete one of several associated with deleting a table row and all its
 * associated index entries.  Exactly one of those deletes is the "primary"
 * delete.  The others are all on OPFLAG_FORDELETE cursors or else are
 * marked with the AUXDELETE flag.
 *
 * If the OPFLAG_NCHANGE flag of P2 (NB: P2 not P5) is set, then the row
 * change count is incremented (otherwise not).
 *
 * P1 must not be pseudo-table.  It has to be a real table with
 * multiple rows.
 *
 * If P4 is not NULL then it points to a Table object. In this case either
 * the update or pre-update hook, or both, may be invoked. The P1 cursor must
 * have been positioned using OP_NotFound prior to invoking this opcode in
 * this case. Specifically, if one is configured, the pre-update hook is
 * invoked if P4 is not NULL. The update-hook is invoked if one is configured,
 * P4 is not NULL, and the OPFLAG_NCHANGE flag is set in P2.
 */
int
vdbe_op_delete(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;
	int opflags;
	int rc = 0;

	opflags = pOp->p2;
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	BtCursor *pBtCur = pC->uc.pCursor;
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	assert(pC->uc.pCursor != 0);
	assert(pBtCur->eState == CURSOR_VALID);

	if (pBtCur->curFlags & BTCF_TaCursor) {
		rc = tarantoolsqlDelete(pBtCur);
	} else if (pBtCur->curFlags & BTCF_TEphemCursor) {
		rc = tarantoolsqlEphemeralDelete(pBtCur);
	} else {
		unreachable();
	}
	pC->cacheStatus = CACHE_STALE;
	pC->seekResult = 0;
	if (rc)
		return -1;

	if (opflags & OPFLAG_NCHANGE)
		p->nChange++;

	return 0;
}

/* Opcode: Update P1 P2 P3 P4 P5
 * Synopsis: key=r[P1]
 *
 * Process UPDATE operation. Primary key fields can not be
 * modified.
 * Under the hood it performs box_update() call.
 * For the performance sake, it takes whole affected row (P1)
 * and encodes into msgpack only fields to be updated (P3).
 *
 * @param P1 The first field to be updated. Fields are located
 *           in the range of [P1...] in decoded state.
 *           Encoded only fields which numbers are presented
 *           in @P3 array.
 * @param P2 P2 Encoded key to be passed to box_update().
 * @param P3 Index of a register with upd_fields blob.
 *           It's items are numbers of fields to be replaced with
 *           new values from P1. They must be sorted in ascending
 *           order.
 * @param P4 Register containing pointer to space to update.
 * @param P5 Flags. If P5 contains OPFLAG_NCHANGE, then VDBE
 *           accounts the change in a case of successful
 *           insertion in nChange counter. If P5 contains
 *           OPFLAG_OE_IGNORE, then we are processing INSERT OR
 *           INGORE statement. Thus, in case of conflict we don't
 *           raise an error.
 */
int
vdbe_op_update(Vdbe *p, Op *pOp, Mem *aMem)
{
	int rc = 0;
	struct Mem *new_tuple = &aMem[pOp->p1];
	if (pOp->p5 & OPFLAG_NCHANGE)
		p->nChange++;

	struct space *space = aMem[pOp->p4.i].u.p;
	assert(pOp->p4type == P4_INT32);

	struct Mem *key_mem = &aMem[pOp->p2];
	assert(mem_is_bin(key_mem));

	struct Mem *upd_fields_mem = &aMem[pOp->p3];
	assert(mem_is_bin(upd_fields_mem));
	uint32_t *upd_fields = (uint32_t *)upd_fields_mem->z;
	uint32_t upd_fields_cnt = upd_fields_mem->n / sizeof(uint32_t);

	/* Prepare Tarantool update ops msgpack. */
	struct region *region = &fiber()->gc;
	size_t used = region_used(region);
	bool is_error = false;
	struct mpstream stream;
	mpstream_init(&stream, region, region_reserve_cb, region_alloc_cb,
		      set_encode_error, &is_error);
	mpstream_encode_array(&stream, upd_fields_cnt);
	for (uint32_t i = 0; i < upd_fields_cnt; i++) {
		uint32_t field_idx = upd_fields[i];
		assert(field_idx < space->def->field_count);
		mpstream_encode_array(&stream, 3);
		mpstream_encode_strn(&stream, "=", 1);
		mpstream_encode_uint(&stream, field_idx);
		mem_to_mpstream(new_tuple + field_idx, &stream);
	}
	mpstream_flush(&stream);
	if (is_error) {
		region_truncate(&fiber()->gc, used);
		diag_set(OutOfMemory, stream.pos - stream.buf,
			"mpstream_flush", "stream");
		return -1;
	}
	uint32_t ops_size = region_used(region) - used;
	const char *ops = xregion_join(region, ops_size);
	assert(rc == 0);
	rc = box_update(space->def->id, 0, key_mem->z, key_mem->z + key_mem->n,
			ops, ops + ops_size, 0, NULL);
	region_truncate(&fiber()->gc, used);

	if (pOp->p5 & OPFLAG_OE_IGNORE) {
		/*
		 * Ignore any kind of fails and do not raise
		 * error message
		 */
		rc = 0;
		/*
		 * If we are in trigger, increment ignore raised
		 * counter.
		 */
		if (p->pFrame)
			p->ignoreRaised++;
	} else if (pOp->p5 & OPFLAG_OE_FAIL) {
		p->errorAction = ON_CONFLICT_ACTION_FAIL;
	} else if (pOp->p5 & OPFLAG_OE_ROLLBACK) {
		p->errorAction = ON_CONFLICT_ACTION_ROLLBACK;
	}
	if (rc != 0)
		return -1;
	return 0;
}

/* Opcode: SInsert P1 P2 * * P5
 * Synopsis: space id = P1, key = r[P2]
 *
 * This opcode is used only during DDL routine.
 * In contrast to ordinary insertion, insertion to system spaces
 * such as _space or _index will lead to schema changes.
 * Thus, usage of space pointers is going to be impossible,
 * as far as pointers can be expired since compilation time.
 *
 * If P5 is set to OPFLAG_NCHANGE, account overall changes
 * made to database.
 */
int
vdbe_op_sinsert(Vdbe *p, Op *pOp, Mem *aMem)
{
	assert(pOp->p1 > 0);
	assert(pOp->p2 >= 0);

	Mem *pIn2 = &aMem[pOp->p2];
	struct space *space = space_by_id(pOp->p1);
	assert(space != NULL);
	assert(space_is_system(space));
	assert(p->errorAction == ON_CONFLICT_ACTION_ABORT);
	if (tarantoolsqlInsert(space, pIn2->z, pIn2->z + pIn2->n) != 0)
		return -1;
	if (pOp->p5 & OPFLAG_NCHANGE)
		p->nChange++;
	return 0;
}

/* Opcode: SDelete P1 P2 P3 * P5
 * Synopsis: space id = P1, key = r[P2], searching index id = P3
 *
 * This opcode is used only during DDL routine.
 * Delete entry with given key from system space. P3 is the index
 * number by which to search for the key.
 *
 * If P5 is set to OPFLAG_NCHANGE, account overall changes
 * made to database.
 */
int
vdbe_op_sdelete(Vdbe *p, Op *pOp, Mem *aMem)
{
	assert(pOp->p1 > 0);
	assert(pOp->p2 >= 0);
	assert(pOp->p3 >= 0);

	Mem *pIn2 = &aMem[pOp->p2];
	struct space *space = space_by_id(pOp->p1);
	assert(space != NULL);
	assert(space_is_system(space));
	assert(p->errorAction == ON_CONFLICT_ACTION_ABORT);
	if (sql_delete_by_key(space, pOp->p3, pIn2->z, pIn2->n) != 0)
		return -1;
	if (pOp->p5 & OPFLAG_NCHANGE)
		p->nChange++;
	return 0;
}

/* Opcode: IdxDelete P1 P2 P3 * *
 * Synopsis: key=r[P2@P3]
 *
 * The content of P3 registers starting at register P2 form
 * an unpacked index key. This opcode removes that entry from the
 * index opened by cursor P1.
 */
int
vdbe_op_idxdelete(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeCursor *pC;
	BtCursor *pCrsr;
	int res;

	assert(pOp->p3 > 0);
	assert(pOp->p2 > 0 && pOp->p2 + pOp->p3 <= (p->nMem + 1 - p->nCursor) + 1);
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	pCrsr = pC->uc.pCursor;
	assert(pCrsr != 0);
	assert(pOp->p5 == 0);
	if (sql_cursor_seek(pCrsr, &aMem[pOp->p2], (u16)pOp->p3, &res) != 0)
		return -1;
	if (res == 0) {
		assert(pCrsr->eState == CURSOR_VALID);
		if (pCrsr->curFlags & BTCF_TaCursor) {
			if (tarantoolsqlDelete(pCrsr) != 0)
				return -1;
		} else if (pCrsr->curFlags & BTCF_TEphemCursor) {
			if (tarantoolsqlEphemeralDelete(pCrsr) != 0)
				return -1;
		} else {
			unreachable();
		}
	}
	pC->cacheStatus = CACHE_STALE;
	pC->seekResult = 0;
	return 0;
}
