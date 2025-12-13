/*
 * Helpers for vdbe_field_ref operations extracted from vdbe.c
 */
#include "sql.h"
#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "tuple.h"
#include "msgpuck/msgpuck.h"

/* Use UPDATE_MAX_BLOBSIZE macro, which is now backed by a shared
 * non-static wrapper implemented in `vdbe.c`.
 */

/* Fetch a tuple field descriptor by fieldno. */
const struct tuple_field *
vdbe_field_ref_fetch_field(struct vdbe_field_ref *field_ref,
                          uint32_t fieldno)
{
    if (field_ref->tuple == NULL)
        return NULL;
    struct tuple_format *format = tuple_format(field_ref->tuple);
    if (fieldno >= tuple_format_field_count(format))
        return NULL;
    return tuple_format_field(format, fieldno);
}

/* Find the left-closest initialized slot for given fieldno. */
uint32_t
vdbe_field_ref_closest_slotno(struct vdbe_field_ref *field_ref,
                              uint32_t fieldno)
{
    uint64_t slot_bitmask = field_ref->slot_bitmask;
    assert(slot_bitmask != 0 && fieldno > 0);
    uint64_t le_mask = fieldno < 64 ? slot_bitmask & ((1LLU << fieldno) - 1)
                                   : slot_bitmask;
    assert(bit_clz_u64(le_mask) < 64);
    return 64 - bit_clz_u64(le_mask) - 1;
}

/* Fetch raw field data pointer using vdbe_field_ref and fill slots cache. */
const char *
vdbe_field_ref_fetch_data(struct vdbe_field_ref *field_ref, uint32_t fieldno)
{
    if (field_ref->slots[fieldno] != 0 || fieldno == 0)
        return field_ref->data + field_ref->slots[fieldno];

    const char *field_begin;
    const struct tuple_field *field = vdbe_field_ref_fetch_field(field_ref,
                                                                 fieldno);
    if (field != NULL && field->offset_slot != TUPLE_OFFSET_SLOT_NIL) {
        field_begin = tuple_field(field_ref->tuple, fieldno);
    } else {
        uint32_t prev = vdbe_field_ref_closest_slotno(field_ref, fieldno);
        if (fieldno >= 64) {
            for (uint32_t it = fieldno - 1; it > prev; it--) {
                if (field_ref->slots[it] == 0)
                    continue;
                prev = it;
                break;
            }
        }
        field_begin = field_ref->data + field_ref->slots[prev];
        for (prev++; prev < fieldno; prev++) {
            mp_next(&field_begin);
            field_ref->slots[prev] = (uint32_t)(field_begin - field_ref->data);
            bitmask64_set_bit(&field_ref->slot_bitmask, prev);
        }
        mp_next(&field_begin);
    }
    field_ref->slots[fieldno] = (uint32_t)(field_begin - field_ref->data);
    bitmask64_set_bit(&field_ref->slot_bitmask, fieldno);
    return field_begin;
}

/* Fetch a field into a Mem cell from vdbe_field_ref */
int
vdbe_field_ref_fetch(struct vdbe_field_ref *field_ref, uint32_t fieldno,
                     struct Mem *dest_mem)
{
    if (fieldno >= field_ref->field_count) {
        UPDATE_MAX_BLOBSIZE(dest_mem);
        return 0;
    }
    assert(sqlVdbeCheckMemInvariants(dest_mem) != 0);
    const char *data = vdbe_field_ref_fetch_data(field_ref, fieldno);
    uint32_t dummy;
    if (mem_from_mp(dest_mem, data, &dummy) != 0)
        return -1;
    UPDATE_MAX_BLOBSIZE(dest_mem);
    return 0;
}
