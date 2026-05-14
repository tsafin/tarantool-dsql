#ifndef TARANTOOL_BOX_SQL_VDBESORT_TEMPLATES_H_INCLUDED
#define TARANTOOL_BOX_SQL_VDBESORT_TEMPLATES_H_INCLUDED

#include <stdbool.h>
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

#ifdef __cplusplus
}
#endif

#endif
