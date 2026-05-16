#ifndef TARANTOOL_BOX_SQL_VDBESORT_TEMPLATES_H_INCLUDED
#define TARANTOOL_BOX_SQL_VDBESORT_TEMPLATES_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SortSubtask SortSubtask;

enum vdbe_sorter_fast_cmp_kind {
	VDBE_SORTER_FAST_CMP_UNSUPPORTED = 0,
	VDBE_SORTER_FAST_CMP_INTLIKE,
	VDBE_SORTER_FAST_CMP_STRING,
	VDBE_SORTER_FAST_CMP_VARBINARY,
	VDBE_SORTER_FAST_CMP_BOOL,
	VDBE_SORTER_FAST_CMP_DOUBLE,
};

enum {
	VDBE_SORTER_FAST_CMP_MAX_PARTS = 16,
	/*
	 * Sentinel returned by the stitched mixed-key fragments when runtime
	 * values fall outside the specialized path and the caller must restart
	 * comparison through the generic sorter comparator.
	 */
	VDBE_SORTER_COMPARE_CNP_FALLBACK = 0x7fffffff,
};

typedef int (*VdbeSorterCompareFallback)(SortSubtask *, bool *, const void *,
					      uint8_t, const uint16_t *,
					      const void *, uint8_t,
					      const uint16_t *);

#ifdef __cplusplus
extern "C" {
#endif

int
vdbeSorterCompareTemplateStrIntStrInt4(SortSubtask *task, bool *key2_cached,
				       const void *key1,
				       uint8_t key1_type_mask,
				       const uint16_t *key1_offsets,
				       const void *key2,
				       uint8_t key2_type_mask,
				       const uint16_t *key2_offsets,
				       uint8_t desc_mask,
				       VdbeSorterCompareFallback fallback);

int
vdbeSorterCompareCnpFieldString(const char **field1, const char **field2);

int
vdbeSorterCompareCnpFieldIntLike(const char **field1, const char **field2);

/*
 * Return a stitched mixed-key comparator body for the given sorter shape.
 * The returned code is entered through vdbeSorterCompareCnpEnter(), which
 * provides the tiny internal ABI used by the copied fragment templates.
 */
void *
vdbeSorterCompareCnpCodeGet(uint32_t part_count, uint16_t desc_mask,
			    const uint8_t *part_kind);

/*
 * Bridge from the normal C call site into the stitched fragment ABI.
 * `field1` and `field2` point at the first payload field after the MsgPack
 * array header; the bridge passes them by address so fragment helpers can
 * advance both cursors in place across the stitched sequence.
 */
int
vdbeSorterCompareCnpEnter(void *target, const char *field1, const char *field2);

#ifdef __cplusplus
}
#endif

#endif
