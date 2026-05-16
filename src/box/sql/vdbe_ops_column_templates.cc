#include "sqlInt.h"
#include "tuple_format.h"
#include "vdbeInt.h"
#include "vdbe_debug.h"
#include "vdbe_ops.h"
#include "msgpuck/msgpuck.h"

extern "C" {
#include "mem.h"
}

enum class VdbeColumnNavKind {
	StaticSlot,
	RuntimeSlot,
	HintPath,
};

static inline int32_t
vdbeOpColumnOffsetSlot(const Op *pOp)
{
	uint16_t encoded = (pOp->p5 & OPFLAG_CNP_COLUMN_OFFSET_SLOT_MASK) >>
		OPFLAG_CNP_COLUMN_OFFSET_SLOT_SHIFT;
	return encoded == 0 ? TUPLE_OFFSET_SLOT_NIL : -(int32_t)encoded;
}

static inline int
vdbeOpColumnDecodeIntegerExactFast(struct Mem *mem, const char *data)
{
	switch (mp_typeof(*data)) {
	case MP_NIL:
		mp_decode_nil(&data);
		mem_set_null(mem);
		return 0;
	case MP_UINT:
		mem->u.u = mp_decode_uint(&data);
		mem->type = MEM_TYPE_UINT;
		mem->flags = 0;
		return 0;
	case MP_INT:
		mem->u.i = mp_decode_int(&data);
		mem->type = MEM_TYPE_INT;
		mem->flags = 0;
		return 0;
	default:
		return 1;
	}
}

template <enum field_type Type>
static inline int
vdbeOpColumnDecodeExactFast(struct Mem *mem, const char *data)
{
	if constexpr (Type == FIELD_TYPE_INTEGER) {
		return vdbeOpColumnDecodeIntegerExactFast(mem, data);
	} else {
		static_assert(Type == FIELD_TYPE_STRING,
			      "Only string/integer specializations are instantiated");
		switch (mp_typeof(*data)) {
		case MP_NIL:
			mp_decode_nil(&data);
			mem_set_null(mem);
			return 0;
		case MP_STR:
			mem_set_str_ephemeral(mem, (char *)data,
					      mp_decode_strl(&data));
			return 0;
		default:
			return 1;
		}
	}
}

static inline int
vdbeOpColumnRefreshCache(Vdbe *p, Op *pOp, Mem *aMem, VdbeCursor *pC)
{
	(void)pOp;
	if (pC->cacheStatus == p->cacheCtr)
		return 0;
	if (pC->nullRow) {
		if (pC->eCurType == CURTYPE_PSEUDO) {
			assert(pC->uc.pseudoTableReg > 0);
			Mem *pReg = &aMem[pC->uc.pseudoTableReg];
			assert(mem_is_bin(pReg));
			assert(memIsValid(pReg));
			vdbe_field_ref_prepare_data(&pC->field_ref, pReg->z,
						    pReg->n);
		} else {
			return 1;
		}
	} else {
		BtCursor *pCrsr = pC->uc.pCursor;
		assert(pCrsr != NULL);
		assert(sqlCursorIsValid(pCrsr));
		assert(pCrsr->curFlags & BTCF_TaCursor ||
		       pCrsr->curFlags & BTCF_TEphemCursor);
		vdbe_field_ref_prepare_tuple(&pC->field_ref, pCrsr->last_tuple);
	}
	pC->cacheStatus = p->cacheCtr;
	return 0;
}

template <enum field_type Type, VdbeColumnNavKind Nav, bool UseGroup>
static int
vdbeOpColumnOffsetSlotFastImpl(Vdbe *p, Op *pOp, Mem *aMem)
{
	int p2 = pOp->p2;
	VdbeCursor *pC = p->apCsr[pOp->p1];
	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	Mem *pDest = vdbe_prepare_null_out(p, pOp->p3);
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(pC != NULL);
	assert(p2 < pC->nField);
	assert(pC->eCurType != CURTYPE_PSEUDO || pC->nullRow);
	assert(pC->eCurType != CURTYPE_SORTER);
	Mem *default_val_mem = pOp->p4type == P4_MEM ? pOp->p4.pMem : NULL;

	if (vdbeOpColumnRefreshCache(p, pOp, aMem, pC) != 0)
		goto out;
	if (pC->eCurType != CURTYPE_TARANTOOL)
		return vdbe_op_column(p, pOp, aMem);
	assert(pC->uc.pCursor->space->def->fields[p2].type == Type);
	if constexpr (UseGroup) {
		vdbe_field_ref_preload_group_fast(&pC->field_ref,
						  vdbe_cnp_column_group_get(
							  p, pOp));
	}
	if ((uint32_t)p2 < pC->field_ref.field_count) {
		const char *data;
		if constexpr (Nav == VdbeColumnNavKind::StaticSlot) {
			data = vdbe_field_ref_fetch_data_offset_slot_fast(
				&pC->field_ref, vdbeOpColumnOffsetSlot(pOp), p2);
		} else if constexpr (Nav == VdbeColumnNavKind::RuntimeSlot) {
			int32_t offset_slot =
				vdbe_field_ref_offset_slot_fast(&pC->field_ref,
								p2);
			if (offset_slot == TUPLE_OFFSET_SLOT_NIL) {
				if constexpr (Type == FIELD_TYPE_INTEGER)
					return vdbe_op_column_integer_exact_fast(
						p, pOp, aMem);
				else
					return vdbe_op_column_string_exact_fast(
						p, pOp, aMem);
			}
			data = vdbe_field_ref_fetch_data_offset_slot_fast(
				&pC->field_ref, offset_slot, p2);
		} else {
			data = vdbe_field_ref_fetch_data_hint_path_fast(
				&pC->field_ref, vdbe_cnp_column_path_get(p, pOp));
		}
		int rc = vdbeOpColumnDecodeExactFast<Type>(pDest, data);
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
out:
	REGISTER_TRACE(p, pOp->p3, pDest);
	return 0;
}

extern "C" int
vdbe_op_column_string_offset_slot_static_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_STRING,
		VdbeColumnNavKind::StaticSlot, false>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_string_offset_slot_static_group_fast(Vdbe *p, Op *pOp,
						    Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_STRING,
		VdbeColumnNavKind::StaticSlot, true>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_string_offset_slot_path_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_STRING,
		VdbeColumnNavKind::HintPath, false>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_string_offset_slot_path_group_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_STRING,
		VdbeColumnNavKind::HintPath, true>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_string_offset_slot_runtime_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_STRING,
		VdbeColumnNavKind::RuntimeSlot, false>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_integer_offset_slot_static_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_INTEGER,
		VdbeColumnNavKind::StaticSlot, false>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_integer_offset_slot_static_group_fast(Vdbe *p, Op *pOp,
						     Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_INTEGER,
		VdbeColumnNavKind::StaticSlot, true>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_integer_offset_slot_path_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_INTEGER,
		VdbeColumnNavKind::HintPath, false>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_integer_offset_slot_path_group_fast(Vdbe *p, Op *pOp,
						   Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_INTEGER,
		VdbeColumnNavKind::HintPath, true>(p, pOp, aMem);
}

extern "C" int
vdbe_op_column_integer_offset_slot_runtime_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbeOpColumnOffsetSlotFastImpl<FIELD_TYPE_INTEGER,
		VdbeColumnNavKind::RuntimeSlot, false>(p, pOp, aMem);
}
