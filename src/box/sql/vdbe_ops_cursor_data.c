/* Cursor data access opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"
#include "tarantoolInt.h"
#include "tuple.h"
#include "msgpuck/msgpuck.h"

#ifdef SQL_TEST
extern int sql_xfer_count;
#endif

/* Opcode: ResultRow P1 P2 * * *
 * Synopsis: output=r[P1@P2]
 *
 * The registers P1 through P1+P2-1 contain a single row of
 * results. This opcode causes the sql_step() call to terminate
 * with an SQL_ROW return code and it sets up the sql_stmt
 * structure to provide access to the r(P1)..r(P1+P2-1) values as
 * the result row.
 *
 * Returns 1 to signal that the VDBE should return SQL_ROW to the caller.
 */
int vdbe_op_resultrow(Vdbe *p, Op *pOp, Mem *aMem)
{
	assert(p->nResColumn == pOp->p2);
	assert(pOp->p1 > 0);
	assert(pOp->p1 + pOp->p2 <= (p->nMem+1 - p->nCursor) + 1);
	assert(p->iStatement == 0 && p->anonymous_savepoint == NULL);

	/* Invalidate all ephemeral cursor row caches */
	p->cacheCtr = (p->cacheCtr + 2) |1;

	p->pResultSet = &aMem[pOp->p1];
#ifdef SQL_DEBUG
	struct Mem *pMem = p->pResultSet;
	for (int i = 0; i < pOp->p2; i++) {
		assert(memIsValid(&pMem[i]));
		REGISTER_TRACE(p, pOp->p1+i, &pMem[i]);
	}
#endif

	/* NOTE: Trace functionality (db->mTrace) removed - db is not accessible */

	/* Set PC for return and signal that we should return SQL_ROW */
	p->pc = (int)(pOp - p->aOp) + 1;
	return 1;  /* Special return: caller should return SQL_ROW */
}

/* Opcode: Column P1 P2 P3 P4 P5
 * Synopsis: r[P3]=PX
 *
 * Interpret the data that cursor P1 points to as a structure built using
 * the MakeRecord instruction.  (See the MakeRecord opcode for additional
 * information about the format of the data.)  Extract the P2-th column
 * from this record.  If there are less that (P2+1)
 * values in the record, extract a NULL.
 *
 * The value extracted is stored in register P3.
 *
 * If the column contains fewer than P2 fields, then extract a NULL.  Or,
 * if the P4 argument is a P4_MEM use the value of the P4 argument as
 * the result.
 *
 * If the OPFLAG_CLEARCACHE bit is set on P5 and P1 is a pseudo-table cursor,
 * then the cache of the cursor is reset prior to extracting the column.
 * The first OP_Column against a pseudo-table after the value of the content
 * register has changed should have this bit set.
 *
 * If the OPFLAG_LENGTHARG and OPFLAG_TYPEOFARG bits are set on P5 when
 * the result is guaranteed to only be used as the argument of a length()
 * or typeof() function, respectively.  The loading of large blobs can be
 * skipped for length() and all content loading can be skipped for typeof().
 */
int vdbe_op_column(Vdbe *p, Op *pOp, Mem *aMem)
{
	int p2 = pOp->p2;	   /* column number to retrieve */
	VdbeCursor *pC = p->apCsr[pOp->p1]; /* The VDBE cursor */
	BtCursor *pCrsr = NULL; /* The BTree cursor */
	Mem *pDest;        /* Where to write the extracted value */
	Mem *pReg;         /* PseudoTable input register */

	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	pDest = vdbe_prepare_null_out(p, pOp->p3);
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(pC != 0);
	assert(p2 < pC->nField);
	assert(pC->eCurType != CURTYPE_PSEUDO || pC->nullRow);
	assert(pC->eCurType != CURTYPE_SORTER);

	if (pC->cacheStatus != p->cacheCtr) {                /*OPTIMIZATION-IF-FALSE*/
		if (pC->nullRow) {
			if (pC->eCurType == CURTYPE_PSEUDO) {
				assert(pC->uc.pseudoTableReg > 0);
				pReg = &aMem[pC->uc.pseudoTableReg];
				assert(mem_is_bin(pReg));
				assert(memIsValid(pReg));
				vdbe_field_ref_prepare_data(&pC->field_ref,
							    pReg->z, pReg->n);
			} else {
				goto op_column_out;
			}
		} else {
			pCrsr = pC->uc.pCursor;
			assert(pC->eCurType == CURTYPE_TARANTOOL);
			assert(pCrsr);
			assert(sqlCursorIsValid(pCrsr));
			assert(pCrsr->curFlags & BTCF_TaCursor ||
			       pCrsr->curFlags & BTCF_TEphemCursor);
			vdbe_field_ref_prepare_tuple(&pC->field_ref,
						     pCrsr->last_tuple);
		}
		pC->cacheStatus = p->cacheCtr;
	}
	assert(pC->eCurType == CURTYPE_TARANTOOL ||
	       pC->eCurType == CURTYPE_PSEUDO);
	struct Mem *default_val_mem =
		pOp->p4type == P4_MEM ? pOp->p4.pMem : NULL;
	if (vdbe_field_ref_fetch(&pC->field_ref, p2, pDest) != 0)
		return -1;

	if (mem_is_null(pDest) &&
	    (uint32_t) p2  >= pC->field_ref.field_count &&
	    default_val_mem != NULL) {
		mem_copy_as_ephemeral(pDest, default_val_mem);
	}
	if (pDest->type == MEM_TYPE_NULL)
		goto op_column_out;
	enum field_type field_type = field_type_MAX;
	/* Currently PSEUDO cursor does not have info about field types. */
	if (pC->eCurType == CURTYPE_TARANTOOL)
		field_type = pC->uc.pCursor->space->def->fields[p2].type;
	if (field_type == FIELD_TYPE_ANY)
		pDest->flags |= MEM_Any;
	else if (field_type == FIELD_TYPE_SCALAR)
		pDest->flags |= MEM_Scalar;
	else if (field_type == FIELD_TYPE_NUMBER)
		pDest->flags |= MEM_Number;
op_column_out:
	REGISTER_TRACE(p, pOp->p3, pDest);
	return 0;
}

static inline int
vdbe_op_column_decode_fast(struct Mem *mem, const char *data,
			   enum field_type field_type)
{
	switch (mp_typeof(*data)) {
	case MP_NIL:
		mp_decode_nil(&data);
		mem_set_null(mem);
		return 0;
	case MP_UINT:
		if (field_type == FIELD_TYPE_INTEGER ||
		    field_type == FIELD_TYPE_UNSIGNED) {
			mem->u.u = mp_decode_uint(&data);
			mem->type = MEM_TYPE_UINT;
			mem->flags = 0;
			return 0;
		}
		return 1;
	case MP_INT:
		if (field_type == FIELD_TYPE_INTEGER) {
			mem->u.i = mp_decode_int(&data);
			mem->type = MEM_TYPE_INT;
			mem->flags = 0;
			return 0;
		}
		return 1;
	case MP_STR:
		if (field_type != FIELD_TYPE_STRING)
			return 1;
		mem->n = (int)mp_decode_strl(&data);
		if (mem_copy_str(mem, data, mem->n) != 0)
			return -1;
		mem->flags &= ~(MEM_Scalar | MEM_Any);
		return 0;
	case MP_BOOL:
		if (field_type != FIELD_TYPE_BOOLEAN)
			return 1;
		mem->u.b = mp_decode_bool(&data);
		mem->type = MEM_TYPE_BOOL;
		mem->flags = 0;
		return 0;
	case MP_FLOAT:
		if (field_type != FIELD_TYPE_DOUBLE)
			return 1;
		mem->u.r = mp_decode_float(&data);
		if (sqlIsNaN(mem->u.r))
			mem_set_null(mem);
		else {
			mem->type = MEM_TYPE_DOUBLE;
			mem->flags = 0;
		}
		return 0;
	case MP_DOUBLE:
		if (field_type != FIELD_TYPE_DOUBLE)
			return 1;
		mem->u.r = mp_decode_double(&data);
		if (sqlIsNaN(mem->u.r))
			mem_set_null(mem);
		else {
			mem->type = MEM_TYPE_DOUBLE;
			mem->flags = 0;
		}
		return 0;
	default:
		return 1;
	}
}

static inline const char *
vdbe_field_ref_scan_forward_inline(struct vdbe_field_ref *field_ref,
				   uint32_t prev, uint32_t fieldno)
{
	assert(prev < fieldno);
	const char *field_begin = field_ref->data + field_ref->slots[prev];
	for (prev++; prev < fieldno; prev++) {
		mp_next(&field_begin);
		field_ref->slots[prev] =
			(uint32_t)(field_begin - field_ref->data);
		bitmask64_set_bit(&field_ref->slot_bitmask, prev);
	}
	mp_next(&field_begin);
	field_ref->slots[fieldno] = (uint32_t)(field_begin - field_ref->data);
	bitmask64_set_bit(&field_ref->slot_bitmask, fieldno);
	if (fieldno > field_ref->rightmost_slot)
		field_ref->rightmost_slot = fieldno;
	return field_begin;
}

static inline uint32_t
vdbe_field_ref_find_prev_slotno_inline(struct vdbe_field_ref *field_ref,
				       uint32_t fieldno)
{
	uint32_t prev;
	if (fieldno > field_ref->rightmost_slot) {
		prev = field_ref->rightmost_slot;
	} else {
		prev = vdbe_field_ref_closest_slotno(field_ref, fieldno);
	}
	if (fieldno >= 64) {
		for (uint32_t it = fieldno - 1; it > prev; it--) {
			if (field_ref->slots[it] == 0)
				continue;
			prev = it;
			break;
		}
	}
	return prev;
}

/*
 * Fetch a field by jumping straight to a known offset slot. If the tuple's
 * field_map does not contain the slot for this row shape, fall back to the
 * generic fetch path so sparse / optional fields still behave correctly.
 */
static inline const char *
vdbe_field_ref_fetch_data_offset_slot_inline(struct vdbe_field_ref *field_ref,
					     int32_t offset_slot,
					     uint32_t fieldno);

static inline const char *
vdbe_field_ref_fetch_data_inline(struct vdbe_field_ref *field_ref,
				 uint32_t fieldno)
{
	if (field_ref->slots[fieldno] != 0 || fieldno == 0)
		return field_ref->data + field_ref->slots[fieldno];

	const char *field_begin;
	const struct tuple_field *field = vdbe_field_ref_fetch_field(field_ref,
								     fieldno);
	if (field != NULL && field->offset_slot != TUPLE_OFFSET_SLOT_NIL) {
		field_begin = tuple_field(field_ref->tuple, fieldno);
	} else {
		uint32_t prev =
			vdbe_field_ref_find_prev_slotno_inline(field_ref, fieldno);
		return vdbe_field_ref_scan_forward_inline(field_ref, prev,
							 fieldno);
	}
	field_ref->slots[fieldno] = (uint32_t)(field_begin - field_ref->data);
	bitmask64_set_bit(&field_ref->slot_bitmask, fieldno);
	if (fieldno > field_ref->rightmost_slot)
		field_ref->rightmost_slot = fieldno;
	return field_begin;
}

static inline const char *
vdbe_field_ref_fetch_data_offset_slot_inline(struct vdbe_field_ref *field_ref,
					     int32_t offset_slot,
					     uint32_t fieldno)
{
	if (field_ref->slots[fieldno] != 0 || fieldno == 0)
		return field_ref->data + field_ref->slots[fieldno];

	assert(field_ref->tuple != NULL);
	assert(offset_slot != TUPLE_OFFSET_SLOT_NIL);
	const uint32_t *field_map = tuple_field_map(field_ref->tuple);
	uint32_t offset = field_map_get_offset(field_map, offset_slot,
					       MULTIKEY_NONE);
	if (offset == 0)
		return vdbe_field_ref_fetch_data_inline(field_ref, fieldno);
	assert(offset >= field_ref->field0_offset);
	const uint32_t field_offset = offset - field_ref->field0_offset;
	const char *field_begin = field_ref->data + field_offset;
	field_ref->slots[fieldno] = field_offset;
	bitmask64_set_bit(&field_ref->slot_bitmask, fieldno);
	if (fieldno > field_ref->rightmost_slot)
		field_ref->rightmost_slot = fieldno;
	return field_begin;
}

static inline int32_t
vdbe_op_column_offset_slot(const Op *pOp)
{
	uint16_t encoded = (pOp->p5 & OPFLAG_CNP_COLUMN_OFFSET_SLOT_MASK) >>
		OPFLAG_CNP_COLUMN_OFFSET_SLOT_SHIFT;
	return encoded == 0 ? TUPLE_OFFSET_SLOT_NIL : -(int32_t)encoded;
}

#ifdef ENABLE_SQL_CNP
static inline const struct cnp_column_path *
cnp_column_path(const Vdbe *p, const Op *pOp)
{
	if (p == NULL || p->cnp_column_path == NULL || p->aOp == NULL)
		return NULL;
	int pc = (int)(pOp - p->aOp);
	if (pc < 0 || pc >= p->nOp)
		return NULL;
	const struct cnp_column_path *path = &p->cnp_column_path[pc];
	return path->enabled ? path : NULL;
}

static inline const struct cnp_column_group *
cnp_column_group(const Vdbe *p, const Op *pOp)
{
	if (p == NULL || p->cnp_column_group == NULL || p->aOp == NULL)
		return NULL;
	int pc = (int)(pOp - p->aOp);
	if (pc < 0 || pc >= p->nOp)
		return NULL;
	const struct cnp_column_group *group = &p->cnp_column_group[pc];
	return group->enabled ? group : NULL;
}

/*
 * Seed one dense OP_Column cluster so later columns in the group can hit the
 * cached slots[] array instead of re-walking the tuple.
 */
static inline void
vdbe_field_ref_preload_group_inline(struct vdbe_field_ref *field_ref,
				    const struct cnp_column_group *group)
{
	if (group == NULL || field_ref->field_count == 0)
		return;
	if (group->min_field >= field_ref->field_count)
		return;

	uint32_t min_field = group->min_field;
	uint32_t max_field = MIN(group->max_field, field_ref->field_count - 1);
	if (group->min_offset_slot != TUPLE_OFFSET_SLOT_NIL)
		(void)vdbe_field_ref_fetch_data_offset_slot_inline(
			field_ref, group->min_offset_slot, min_field);
	else
		(void)vdbe_field_ref_fetch_data_inline(field_ref, min_field);
	if (max_field > min_field)
		(void)vdbe_field_ref_scan_forward_inline(field_ref, min_field,
							 max_field);
}

/* Advance a fixed number of fields to the right, caching every intermediate. */
static inline const char *
vdbe_field_ref_scan_hops_inline(struct vdbe_field_ref *field_ref,
				uint32_t fieldno, uint16_t hop_count)
{
	assert(hop_count > 0);
	const char *field_begin = field_ref->data + field_ref->slots[fieldno];
	for (uint16_t hop = 0; hop < hop_count; hop++) {
		mp_next(&field_begin);
		fieldno++;
		field_ref->slots[fieldno] =
			(uint32_t)(field_begin - field_ref->data);
		bitmask64_set_bit(&field_ref->slot_bitmask, fieldno);
	}
	if (fieldno > field_ref->rightmost_slot)
		field_ref->rightmost_slot = fieldno;
	return field_begin;
}

/*
 * Resolve an offset slot from the tuple's active format. This is the covering
 * scan path, where the helper family is the same, but the slot cannot be baked
 * from the base space format at compile time.
 */
static inline int32_t
vdbe_field_ref_offset_slot_inline(struct vdbe_field_ref *field_ref,
				     uint32_t fieldno)
{
	const struct tuple_field *field =
		vdbe_field_ref_fetch_field(field_ref, fieldno);
	return field == NULL ? TUPLE_OFFSET_SLOT_NIL : field->offset_slot;
}

/*
 * Execute the precomputed "anchor + hop_count" route for an unhinted target:
 * jump to the nearest hinted anchor first, then walk right a fixed number of
 * MsgPack fields.
 */
static inline const char *
vdbe_field_ref_fetch_data_hint_path_inline(struct vdbe_field_ref *field_ref,
					   const struct cnp_column_path *path)
{
	assert(path != NULL);
	int32_t offset_slot = path->offset_slot;
	if (offset_slot == TUPLE_OFFSET_SLOT_NIL) {
		offset_slot = vdbe_field_ref_offset_slot_inline(field_ref,
								path->anchor_field);
	}
	if (offset_slot == TUPLE_OFFSET_SLOT_NIL) {
		uint32_t target = path->anchor_field + path->hop_count;
		return vdbe_field_ref_fetch_data_inline(field_ref, target);
	}
	if (path->hop_count == 0) {
		return vdbe_field_ref_fetch_data_offset_slot_inline(
			field_ref, offset_slot, path->anchor_field);
	}
	(void)vdbe_field_ref_fetch_data_offset_slot_inline(
		field_ref, offset_slot, path->anchor_field);
	return vdbe_field_ref_scan_hops_inline(field_ref, path->anchor_field,
					       path->hop_count);
}
#else
#define cnp_column_group(p, pOp) NULL
#define cnp_column_path(p, pOp) NULL

static inline void
vdbe_field_ref_preload_group_inline(struct vdbe_field_ref *field_ref,
				    const void *group)
{
	(void)field_ref;
	(void)group;
}

static inline int32_t
vdbe_field_ref_offset_slot_inline(struct vdbe_field_ref *field_ref,
				     uint32_t fieldno)
{
	(void)field_ref;
	(void)fieldno;
	return TUPLE_OFFSET_SLOT_NIL;
}

static inline const char *
vdbe_field_ref_fetch_data_hint_path_inline(struct vdbe_field_ref *field_ref,
					   const void *path)
{
	(void)field_ref;
	(void)path;
	return NULL;
}
#endif

static int
vdbe_op_column_typed_fast(Vdbe *p, Op *pOp, Mem *aMem,
			  enum field_type expected_type)
{
	int p2 = pOp->p2;
	VdbeCursor *pC = p->apCsr[pOp->p1];
	Mem *pDest;
	Mem *pReg;

	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	pDest = vdbe_prepare_null_out(p, pOp->p3);
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(pC != NULL);
	assert(p2 < pC->nField);
	assert(pC->eCurType != CURTYPE_PSEUDO || pC->nullRow);
	assert(pC->eCurType != CURTYPE_SORTER);

	if (pC->cacheStatus != p->cacheCtr) {
		if (pC->nullRow) {
			if (pC->eCurType == CURTYPE_PSEUDO) {
				assert(pC->uc.pseudoTableReg > 0);
				pReg = &aMem[pC->uc.pseudoTableReg];
				assert(mem_is_bin(pReg));
				assert(memIsValid(pReg));
				vdbe_field_ref_prepare_data(&pC->field_ref,
							    pReg->z, pReg->n);
			} else {
				goto out;
			}
		} else {
			BtCursor *pCrsr = pC->uc.pCursor;
			assert(pC->eCurType == CURTYPE_TARANTOOL);
			assert(pCrsr != NULL);
			assert(sqlCursorIsValid(pCrsr));
			assert(pCrsr->curFlags & BTCF_TaCursor ||
			       pCrsr->curFlags & BTCF_TEphemCursor);
			vdbe_field_ref_prepare_tuple(&pC->field_ref,
						     pCrsr->last_tuple);
		}
		pC->cacheStatus = p->cacheCtr;
	}

	struct Mem *default_val_mem =
		pOp->p4type == P4_MEM ? pOp->p4.pMem : NULL;
	if (pC->eCurType != CURTYPE_TARANTOOL ||
	    pC->uc.pCursor->space->def->fields[p2].type != expected_type)
		return vdbe_op_column(p, pOp, aMem);
	if ((uint32_t)p2 < pC->field_ref.field_count) {
		const char *data = vdbe_field_ref_fetch_data(&pC->field_ref, p2);
		int rc = vdbe_op_column_decode_fast(pDest, data, expected_type);
		if (rc < 0)
			return -1;
		if (rc > 0) {
			uint32_t dummy;
			if (mem_from_mp(pDest, data, &dummy) != 0)
				return -1;
		}
		UPDATE_MAX_BLOBSIZE(pDest);
	} else {
		UPDATE_MAX_BLOBSIZE(pDest);
	}

	if (mem_is_null(pDest) &&
	    (uint32_t)p2 >= pC->field_ref.field_count &&
	    default_val_mem != NULL) {
		mem_copy_as_ephemeral(pDest, default_val_mem);
	}
	if (pDest->type == MEM_TYPE_NULL)
		goto out;

	if (expected_type == FIELD_TYPE_NUMBER)
		pDest->flags |= MEM_Number;
out:
	REGISTER_TRACE(p, pOp->p3, pDest);
	return 0;
}

static int
vdbe_op_column_typed_exact_fast(Vdbe *p, Op *pOp, Mem *aMem,
				enum field_type expected_type)
{
	int p2 = pOp->p2;
	VdbeCursor *pC = p->apCsr[pOp->p1];
	Mem *pDest;
	Mem *pReg;

	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	pDest = vdbe_prepare_null_out(p, pOp->p3);
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(pC != NULL);
	assert(p2 < pC->nField);
	assert(pC->eCurType != CURTYPE_PSEUDO || pC->nullRow);
	assert(pC->eCurType != CURTYPE_SORTER);

	if (pC->cacheStatus != p->cacheCtr) {
		if (pC->nullRow) {
			if (pC->eCurType == CURTYPE_PSEUDO) {
				assert(pC->uc.pseudoTableReg > 0);
				pReg = &aMem[pC->uc.pseudoTableReg];
				assert(mem_is_bin(pReg));
				assert(memIsValid(pReg));
				vdbe_field_ref_prepare_data(&pC->field_ref,
							    pReg->z, pReg->n);
			} else {
				goto out;
			}
		} else {
			BtCursor *pCrsr = pC->uc.pCursor;
			assert(pCrsr != NULL);
			assert(sqlCursorIsValid(pCrsr));
			assert(pCrsr->curFlags & BTCF_TaCursor ||
			       pCrsr->curFlags & BTCF_TEphemCursor);
			vdbe_field_ref_prepare_tuple(&pC->field_ref,
						     pCrsr->last_tuple);
		}
		pC->cacheStatus = p->cacheCtr;
	}

	struct Mem *default_val_mem =
		pOp->p4type == P4_MEM ? pOp->p4.pMem : NULL;
	if (pC->eCurType != CURTYPE_TARANTOOL)
		return vdbe_op_column(p, pOp, aMem);
	assert(pC->uc.pCursor->space->def->fields[p2].type == expected_type);
	vdbe_field_ref_preload_group_inline(&pC->field_ref,
					    cnp_column_group(p, pOp));
	if ((uint32_t)p2 < pC->field_ref.field_count) {
		const char *data = vdbe_field_ref_fetch_data_inline(&pC->field_ref,
								    p2);
		int rc = vdbe_op_column_decode_fast(pDest, data, expected_type);
		if (rc < 0)
			return -1;
		if (rc > 0) {
			uint32_t dummy;
			if (mem_from_mp(pDest, data, &dummy) != 0)
				return -1;
		}
		UPDATE_MAX_BLOBSIZE(pDest);
	} else {
		UPDATE_MAX_BLOBSIZE(pDest);
	}

	if (mem_is_null(pDest) &&
	    (uint32_t)p2 >= pC->field_ref.field_count &&
	    default_val_mem != NULL) {
		mem_copy_as_ephemeral(pDest, default_val_mem);
	}
	if (pDest->type == MEM_TYPE_NULL)
		goto out;
	assert(expected_type != FIELD_TYPE_NUMBER);
out:
	REGISTER_TRACE(p, pOp->p3, pDest);
	return 0;
}

/*
 * Offset-slot helpers serve both:
 * - primary scans with a statically encoded slot in OP.p5;
 * - covering scans, where the slot is resolved from the runtime tuple format;
 * - anchor+hops paths that still start from a slot-bearing anchor.
 */
static int
vdbe_op_column_typed_offset_slot_fast(Vdbe *p, Op *pOp, Mem *aMem,
				      enum field_type expected_type)
{
	int p2 = pOp->p2;
	VdbeCursor *pC = p->apCsr[pOp->p1];
	Mem *pDest;
	Mem *pReg;

	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	pDest = vdbe_prepare_null_out(p, pOp->p3);
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(pC != NULL);
	assert(p2 < pC->nField);
	assert(pC->eCurType != CURTYPE_PSEUDO || pC->nullRow);
	assert(pC->eCurType != CURTYPE_SORTER);

	if (pC->cacheStatus != p->cacheCtr) {
		if (pC->nullRow) {
			if (pC->eCurType == CURTYPE_PSEUDO) {
				assert(pC->uc.pseudoTableReg > 0);
				pReg = &aMem[pC->uc.pseudoTableReg];
				assert(mem_is_bin(pReg));
				assert(memIsValid(pReg));
				vdbe_field_ref_prepare_data(&pC->field_ref,
							    pReg->z, pReg->n);
			} else {
				goto out;
			}
		} else {
			BtCursor *pCrsr = pC->uc.pCursor;
			assert(pCrsr != NULL);
			assert(sqlCursorIsValid(pCrsr));
			assert(pCrsr->curFlags & BTCF_TaCursor ||
			       pCrsr->curFlags & BTCF_TEphemCursor);
			vdbe_field_ref_prepare_tuple(&pC->field_ref,
						     pCrsr->last_tuple);
		}
		pC->cacheStatus = p->cacheCtr;
	}

	struct Mem *default_val_mem =
		pOp->p4type == P4_MEM ? pOp->p4.pMem : NULL;
	if (pC->eCurType != CURTYPE_TARANTOOL)
		return vdbe_op_column(p, pOp, aMem);
	int32_t offset_slot = vdbe_op_column_offset_slot(pOp);
	const struct cnp_column_path *path = cnp_column_path(p, pOp);
	/* Covering scans discover the slot from the tuple format at runtime. */
	if (offset_slot == TUPLE_OFFSET_SLOT_NIL && path == NULL)
		offset_slot = vdbe_field_ref_offset_slot_inline(&pC->field_ref, p2);
	assert(pC->uc.pCursor->space->def->fields[p2].type == expected_type);
	if (offset_slot == TUPLE_OFFSET_SLOT_NIL && path == NULL)
		return vdbe_op_column_typed_exact_fast(p, pOp, aMem,
						       expected_type);
	vdbe_field_ref_preload_group_inline(&pC->field_ref,
					    cnp_column_group(p, pOp));
	if ((uint32_t)p2 < pC->field_ref.field_count) {
		const char *data = path != NULL ?
			vdbe_field_ref_fetch_data_hint_path_inline(&pC->field_ref,
							       path) :
			vdbe_field_ref_fetch_data_offset_slot_inline(
				&pC->field_ref, offset_slot, p2);
		int rc = vdbe_op_column_decode_fast(pDest, data, expected_type);
		if (rc < 0)
			return -1;
		if (rc > 0) {
			uint32_t dummy;
			if (mem_from_mp(pDest, data, &dummy) != 0)
				return -1;
		}
		UPDATE_MAX_BLOBSIZE(pDest);
	} else {
		UPDATE_MAX_BLOBSIZE(pDest);
	}

	if (mem_is_null(pDest) &&
	    (uint32_t)p2 >= pC->field_ref.field_count &&
	    default_val_mem != NULL) {
		mem_copy_as_ephemeral(pDest, default_val_mem);
	}
	if (pDest->type == MEM_TYPE_NULL)
		goto out;
	assert(expected_type != FIELD_TYPE_NUMBER);
out:
	REGISTER_TRACE(p, pOp->p3, pDest);
	return 0;
}

int
vdbe_op_column_unsigned_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_fast(p, pOp, aMem, FIELD_TYPE_UNSIGNED);
}

int
vdbe_op_column_string_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_fast(p, pOp, aMem, FIELD_TYPE_STRING);
}

int
vdbe_op_column_double_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_fast(p, pOp, aMem, FIELD_TYPE_DOUBLE);
}

int
vdbe_op_column_integer_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_fast(p, pOp, aMem, FIELD_TYPE_INTEGER);
}

int
vdbe_op_column_boolean_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_fast(p, pOp, aMem, FIELD_TYPE_BOOLEAN);
}

int
vdbe_op_column_unsigned_exact_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_exact_fast(p, pOp, aMem,
					       FIELD_TYPE_UNSIGNED);
}

int
vdbe_op_column_string_exact_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_exact_fast(p, pOp, aMem,
					       FIELD_TYPE_STRING);
}

int
vdbe_op_column_double_exact_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_exact_fast(p, pOp, aMem,
					       FIELD_TYPE_DOUBLE);
}

int
vdbe_op_column_integer_exact_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_exact_fast(p, pOp, aMem,
					       FIELD_TYPE_INTEGER);
}

int
vdbe_op_column_boolean_exact_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_exact_fast(p, pOp, aMem,
					       FIELD_TYPE_BOOLEAN);
}

int
vdbe_op_column_unsigned_offset_slot_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_offset_slot_fast(p, pOp, aMem,
						     FIELD_TYPE_UNSIGNED);
}

int
vdbe_op_column_string_offset_slot_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_offset_slot_fast(p, pOp, aMem,
						     FIELD_TYPE_STRING);
}

int
vdbe_op_column_double_offset_slot_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_offset_slot_fast(p, pOp, aMem,
						     FIELD_TYPE_DOUBLE);
}

int
vdbe_op_column_integer_offset_slot_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_offset_slot_fast(p, pOp, aMem,
						     FIELD_TYPE_INTEGER);
}

int
vdbe_op_column_boolean_offset_slot_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_column_typed_offset_slot_fast(p, pOp, aMem,
						     FIELD_TYPE_BOOLEAN);
}

/* Opcode: RowData P1 P2 * * P5
 * Synopsis: r[P2]=data
 *
 * Write into register P2 the complete row content for the row at
 * which cursor P1 is currently pointing.
 * There is no interpretation of the data.
 * It is just copied onto the P2 register exactly as
 * it is found in the database file.
 * P5 can be used in debug mode to check if xferOptimization has
 * actually started processing.
 *
 * If cursor P1 is an index, then the content is the key of the row.
 * If cursor P2 is a table, then the content extracted is the data.
 *
 * If the P1 cursor must be pointing to a valid row (not a NULL row)
 * of a real table, not a pseudo-table.
 */
int SQL_PRESERVE_NONE vdbe_op_rowdata(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;
	BtCursor *pCrsr;
	u32 n;

/*
 * Flag P5 is cleared after the first insertion using xfer
 * optimization.
 */
#ifdef SQL_TEST
	if ((pOp->p5 & OPFLAG_XFER_OPT) != 0) {
		pOp->p5 &= ~OPFLAG_XFER_OPT;
		sql_xfer_count++;
	}
#endif

	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);

	assert(pOp->p1>=0 && pOp->p1<p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	assert(pC->eCurType != CURTYPE_SORTER);
	assert(pC->nullRow == 0);
	assert(pC->uc.pCursor != 0);
	pCrsr = pC->uc.pCursor;

	/* The OP_RowData opcodes always follow
	 * OP_Rewind/Op_Next with no intervening instructions
	 * that might invalidate the cursor.
	 * If this where not the case, on of the following assert()s
	 * would fail.
	 */
	assert(sqlCursorIsValid(pCrsr));
	assert(pCrsr->eState == CURSOR_VALID);
	assert(pCrsr->curFlags & BTCF_TaCursor ||
	       pCrsr->curFlags & BTCF_TEphemCursor);
	tarantoolsqlPayloadFetch(pCrsr, &n);
	if (n > SQL_MAX_LENGTH) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob too big");
		return -1;
	}

	char *buf = xregion_alloc(&fiber()->gc, n);
	sqlCursorPayload(pCrsr, 0, n, buf);
	mem_set_bin_ephemeral(pOut, buf, n);
	assert(sqlVdbeCheckMemInvariants(pOut));
	UPDATE_MAX_BLOBSIZE(pOut);
	REGISTER_TRACE(p, pOp->p2, pOut);
	return 0;
}
