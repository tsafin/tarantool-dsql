#include "vdbesort_templates.h"

extern "C" {
#include "msgpuck.h"
}

#include <cstddef>
#include <cstring>

enum class VdbeSorterFastKind : uint8_t {
	IntLike = VDBE_SORTER_FAST_CMP_INTLIKE,
	String = VDBE_SORTER_FAST_CMP_STRING,
};

static inline int
vdbeSorterCompareTemplateIntLike(enum mp_type t1, const char **field1,
				    enum mp_type t2, const char **field2)
{
	if (t1 == MP_UINT) {
		uint64_t v1 = mp_decode_uint(field1);
		if (t2 == MP_UINT) {
			uint64_t v2 = mp_decode_uint(field2);
			return v1 < v2 ? -1 : v1 > v2 ? 1 : 0;
		}
		if (t2 != MP_INT)
			return -2;
		int64_t v2 = mp_decode_int(field2);
		return v2 < 0 ? 1 : v1 < (uint64_t)v2 ? -1 :
		       v1 > (uint64_t)v2 ? 1 : 0;
	}
	if (t1 != MP_INT)
		return -2;
	int64_t v1 = mp_decode_int(field1);
	if (t2 == MP_UINT) {
		uint64_t v2 = mp_decode_uint(field2);
		return v1 < 0 ? -1 : (uint64_t)v1 < v2 ? -1 :
		       (uint64_t)v1 > v2 ? 1 : 0;
	}
	if (t2 != MP_INT)
		return -2;
	int64_t v2 = mp_decode_int(field2);
	return v1 < v2 ? -1 : v1 > v2 ? 1 : 0;
}

static inline int
vdbeSorterCompareTemplateString(const char **field1, const char **field2)
{
	if (mp_typeof(**field1) != MP_STR || mp_typeof(**field2) != MP_STR)
		return -2;
	uint32_t len1 = mp_decode_strl(field1);
	uint32_t len2 = mp_decode_strl(field2);
	uint32_t len = len1 < len2 ? len1 : len2;
	int rc = std::memcmp(*field1, *field2, len);
	if (rc == 0)
		rc = len1 < len2 ? -1 : len1 > len2 ? 1 : 0;
	*field1 += len1;
	*field2 += len2;
	return rc;
}

template <VdbeSorterFastKind Kind>
static inline int
vdbeSorterCompareTemplateField(const char **field1, const char **field2)
{
	if constexpr (Kind == VdbeSorterFastKind::String) {
		return vdbeSorterCompareTemplateString(field1, field2);
	} else {
		return vdbeSorterCompareTemplateIntLike(mp_typeof(**field1), field1,
							 mp_typeof(**field2), field2);
	}
}

template <std::size_t PartNo>
static inline int
vdbeSorterCompareTemplateFallback(SortSubtask *task, bool *key2_cached,
				      const void *key1, uint8_t key1_type_mask,
				      const uint16_t *key1_offsets,
				      const void *key2, uint8_t key2_type_mask,
				      const uint16_t *key2_offsets,
				      VdbeSorterCompareFallback fallback)
{
	return fallback(task, key2_cached, key1, key1_type_mask, key1_offsets,
			key2, key2_type_mask, key2_offsets);
}

template <std::size_t PartNo, VdbeSorterFastKind Kind, VdbeSorterFastKind... Rest>
static int
vdbeSorterCompareTemplateParts(SortSubtask *task, bool *key2_cached,
			       const void *key1, uint8_t key1_type_mask,
			       const uint16_t *key1_offsets, const void *key2,
			       uint8_t key2_type_mask,
			       const uint16_t *key2_offsets, uint8_t desc_mask,
			       VdbeSorterCompareFallback fallback, bool use_offsets,
			       const char *field1, const char *field2)
{
	if (use_offsets) {
		field1 = static_cast<const char *>(key1) + key1_offsets[PartNo];
		field2 = static_cast<const char *>(key2) + key2_offsets[PartNo];
	}
	int rc = vdbeSorterCompareTemplateField<Kind>(&field1, &field2);
	if (rc == -2)
		return vdbeSorterCompareTemplateFallback<PartNo>(
			task, key2_cached, key1, key1_type_mask, key1_offsets, key2,
			key2_type_mask, key2_offsets, fallback);
	if (rc != 0) {
		if ((desc_mask & static_cast<uint8_t>(1U << PartNo)) != 0)
			rc = -rc;
		return rc;
	}
	if constexpr (sizeof...(Rest) == 0) {
		return 0;
	} else {
		return vdbeSorterCompareTemplateParts<PartNo + 1, Rest...>(
			task, key2_cached, key1, key1_type_mask, key1_offsets, key2,
			key2_type_mask, key2_offsets, desc_mask, fallback, use_offsets,
			field1, field2);
	}
}

template <VdbeSorterFastKind... Kinds>
static int
vdbeSorterCompareTemplateFixed(SortSubtask *task, bool *key2_cached,
			       const void *key1, uint8_t key1_type_mask,
			       const uint16_t *key1_offsets, const void *key2,
			       uint8_t key2_type_mask,
			       const uint16_t *key2_offsets, uint8_t desc_mask,
			       VdbeSorterCompareFallback fallback)
{
	const char *field1 = static_cast<const char *>(key1);
	const char *field2 = static_cast<const char *>(key2);
	bool use_offsets = key1_offsets != nullptr && key2_offsets != nullptr;
	if (!use_offsets &&
	    (mp_decode_array(&field1) < sizeof...(Kinds) ||
	     mp_decode_array(&field2) < sizeof...(Kinds))) {
		return fallback(task, key2_cached, key1, key1_type_mask,
				key1_offsets, key2, key2_type_mask, key2_offsets);
	}
	return vdbeSorterCompareTemplateParts<0, Kinds...>(
		task, key2_cached, key1, key1_type_mask, key1_offsets, key2,
		key2_type_mask, key2_offsets, desc_mask, fallback, use_offsets,
		field1, field2);
}

extern "C" int
vdbeSorterCompareTemplateStrIntStrInt4(SortSubtask *task, bool *key2_cached,
					    const void *key1,
					    uint8_t key1_type_mask,
					    const uint16_t *key1_offsets,
					    const void *key2,
					    uint8_t key2_type_mask,
					    const uint16_t *key2_offsets,
					    uint8_t desc_mask,
					    VdbeSorterCompareFallback fallback)
{
	return vdbeSorterCompareTemplateFixed<VdbeSorterFastKind::String,
					      VdbeSorterFastKind::IntLike,
					      VdbeSorterFastKind::String,
					      VdbeSorterFastKind::IntLike>(
		task, key2_cached, key1, key1_type_mask, key1_offsets, key2,
		key2_type_mask, key2_offsets, desc_mask, fallback);
}
