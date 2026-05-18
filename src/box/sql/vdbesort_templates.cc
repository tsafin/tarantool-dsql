#include "vdbesort_templates.h"
#include "sqlInt.h"
#include "mem.h"

extern "C" {
#include "generated/vdbe_sorter_cnp_fragments.h"
#include "msgpuck.h"
}

#include <cstddef>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>

enum class VdbeSorterFastKind : uint8_t {
	IntLike = VDBE_SORTER_FAST_CMP_INTLIKE,
	String = VDBE_SORTER_FAST_CMP_STRING,
	Varbinary = VDBE_SORTER_FAST_CMP_VARBINARY,
	Bool = VDBE_SORTER_FAST_CMP_BOOL,
	Double = VDBE_SORTER_FAST_CMP_DOUBLE,
};

/*
 * Static template tier for small hot mixed layouts.
 *
 * This is the bounded precompiled matrix used by vdbeSorterGetCompare() for
 * very common layouts such as [str, intlike, str, intlike]. It stays separate
 * from the stitched long-tail path below because these helpers are ordinary
 * C++ functions selected directly at prepare time.
 */
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
struct VdbeSorterCompareField;

template <VdbeSorterFastKind Kind>
struct VdbeSorterWriteField;

template <>
struct VdbeSorterCompareField<VdbeSorterFastKind::String> {
	static inline int
	exec(const char **field1, const char **field2)
	{
		return vdbeSorterCompareTemplateString(field1, field2);
	}
};

template <>
struct VdbeSorterWriteField<VdbeSorterFastKind::String> {
	static inline bool
	measure(const struct Mem *mem, uint32_t *size)
	{
		if (!mem_is_str(mem) || mem_is_metatype(mem))
			return false;
		*size = mp_sizeof_str((uint32_t)mem->n);
		return true;
	}

	static inline char *
	encode(const struct Mem *mem, char *pos)
	{
		return mp_encode_str(pos, mem->z, (uint32_t)mem->n);
	}
};

template <>
struct VdbeSorterCompareField<VdbeSorterFastKind::IntLike> {
	static inline int
	exec(const char **field1, const char **field2)
	{
		return vdbeSorterCompareTemplateIntLike(mp_typeof(**field1), field1,
							mp_typeof(**field2), field2);
	}
};

template <>
struct VdbeSorterWriteField<VdbeSorterFastKind::IntLike> {
	static inline bool
	measure(const struct Mem *mem, uint32_t *size)
	{
		if (mem->type == MEM_TYPE_INT) {
			*size = mp_sizeof_int(mem->u.i);
			return true;
		}
		if (mem->type == MEM_TYPE_UINT) {
			*size = mp_sizeof_uint(mem->u.u);
			return true;
		}
		return false;
	}

	static inline char *
	encode(const struct Mem *mem, char *pos)
	{
		return mem->type == MEM_TYPE_INT ?
		       mp_encode_int(pos, mem->u.i) :
		       mp_encode_uint(pos, mem->u.u);
	}
};

template <>
struct VdbeSorterWriteField<VdbeSorterFastKind::Varbinary> {
	static inline bool
	measure(const struct Mem *mem, uint32_t *size)
	{
		if (!mem_is_bin(mem) || mem_is_metatype(mem))
			return false;
		*size = mp_sizeof_bin((uint32_t)mem->n);
		return true;
	}

	static inline char *
	encode(const struct Mem *mem, char *pos)
	{
		return mp_encode_bin(pos, mem->z, (uint32_t)mem->n);
	}
};

template <>
struct VdbeSorterWriteField<VdbeSorterFastKind::Bool> {
	static inline bool
	measure(const struct Mem *mem, uint32_t *size)
	{
		if (!mem_is_bool(mem))
			return false;
		*size = mp_sizeof_bool(mem->u.b);
		return true;
	}

	static inline char *
	encode(const struct Mem *mem, char *pos)
	{
		return mp_encode_bool(pos, mem->u.b);
	}
};

template <>
struct VdbeSorterWriteField<VdbeSorterFastKind::Double> {
	static inline bool
	measure(const struct Mem *mem, uint32_t *size)
	{
		if (!mem_is_double(mem))
			return false;
		*size = mp_sizeof_double(mem->u.r);
		return true;
	}

	static inline char *
	encode(const struct Mem *mem, char *pos)
	{
		return mp_encode_double(pos, mem->u.r);
	}
};

template <std::size_t PartNo, VdbeSorterFastKind... Kinds>
struct VdbeSorterWriteParts;

template <std::size_t PartNo, VdbeSorterFastKind Kind>
struct VdbeSorterWriteParts<PartNo, Kind> {
	static inline bool
	measure(const struct Mem *mems, uint32_t *total)
	{
		uint32_t part_size;
		if (!VdbeSorterWriteField<Kind>::measure(&mems[PartNo], &part_size))
			return false;
		*total += part_size;
		return true;
	}

	static inline char *
	encode(const struct Mem *mems, uint32_t offset_part_count,
		       uint16_t *offsets, char *base, char *pos)
	{
		if (PartNo < offset_part_count)
			offsets[PartNo] = (uint16_t)(pos - base);
		return VdbeSorterWriteField<Kind>::encode(&mems[PartNo], pos);
	}
};

template <std::size_t PartNo, VdbeSorterFastKind Kind, VdbeSorterFastKind Next,
	  VdbeSorterFastKind... Rest>
struct VdbeSorterWriteParts<PartNo, Kind, Next, Rest...> {
	static inline bool
	measure(const struct Mem *mems, uint32_t *total)
	{
		uint32_t part_size;
		if (!VdbeSorterWriteField<Kind>::measure(&mems[PartNo], &part_size))
			return false;
		*total += part_size;
		return VdbeSorterWriteParts<PartNo + 1, Next, Rest...>::measure(
			mems, total);
	}

	static inline char *
	encode(const struct Mem *mems, uint32_t offset_part_count,
		       uint16_t *offsets, char *base, char *pos)
	{
		if (PartNo < offset_part_count)
			offsets[PartNo] = (uint16_t)(pos - base);
		pos = VdbeSorterWriteField<Kind>::encode(&mems[PartNo], pos);
		return VdbeSorterWriteParts<PartNo + 1, Next, Rest...>::encode(
			mems, offset_part_count, offsets, base, pos);
	}
};

template <VdbeSorterFastKind... Kinds>
static int
vdbeSorterWriteTemplateFixed(struct VdbeSorter *sorter, const struct Mem *mems,
			     uint32_t count)
{
	(void)count;
	assert(count == sizeof...(Kinds));
	/*
	 * This stays intentionally simple: keep the generic reservation logic in
	 * C and only specialize the per-part sizing/encoding work here so the
	 * compiler can drop Kind-dependent branching with if-constexpr/template
	 * instantiation.
	 */
	uint32_t total = mp_sizeof_array(sizeof...(Kinds));
	if (!VdbeSorterWriteParts<0, Kinds...>::measure(mems, &total))
		return 1;
	uint8_t offset_part_count =
		(uint8_t)vdbeSorterOffsetCachePartCount(sorter);
	char *payload;
	uint16_t *offsets;
	int rc = vdbeSorterWriteTemplateBegin(sorter, (int)total, 0,
					      offset_part_count, &payload,
					      &offsets);
	if (rc != 0)
		return rc;
	char *pos = mp_encode_array(payload, sizeof...(Kinds));
	pos = VdbeSorterWriteParts<0, Kinds...>::encode(mems, offset_part_count,
							offsets, payload, pos);
	assert((uint32_t)(pos - payload) == total);
	return 0;
}

extern "C" int
vdbeSorterWriteTemplateStrStrIntStrIntStrIntStrIntStr10(
	struct VdbeSorter *sorter, const struct Mem *mems, uint32_t count)
{
	return vdbeSorterWriteTemplateFixed<VdbeSorterFastKind::String,
					    VdbeSorterFastKind::String,
					    VdbeSorterFastKind::IntLike,
					    VdbeSorterFastKind::String,
					    VdbeSorterFastKind::IntLike,
					    VdbeSorterFastKind::String,
					    VdbeSorterFastKind::IntLike,
					    VdbeSorterFastKind::String,
					    VdbeSorterFastKind::IntLike,
					    VdbeSorterFastKind::String>(
		sorter, mems, count);
}

extern "C" int
vdbeSorterWriteTemplateStrIntStrInt4(struct VdbeSorter *sorter,
				     const struct Mem *mems, uint32_t count)
{
	return vdbeSorterWriteTemplateFixed<VdbeSorterFastKind::String,
					    VdbeSorterFastKind::IntLike,
					    VdbeSorterFastKind::String,
					    VdbeSorterFastKind::IntLike>(
		sorter, mems, count);
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
	(void)PartNo;
	return fallback(task, key2_cached, key1, key1_type_mask, key1_offsets,
			key2, key2_type_mask, key2_offsets);
}

template <std::size_t PartNo, VdbeSorterFastKind... Kinds>
struct VdbeSorterCompareParts;

template <std::size_t PartNo, VdbeSorterFastKind Kind>
struct VdbeSorterCompareParts<PartNo, Kind> {
	static int
	exec(SortSubtask *task, bool *key2_cached, const void *key1,
	     uint8_t key1_type_mask, const uint16_t *key1_offsets,
	     const void *key2, uint8_t key2_type_mask,
	     const uint16_t *key2_offsets, uint8_t desc_mask,
	     VdbeSorterCompareFallback fallback, bool use_offsets,
	     const char *field1, const char *field2)
	{
		if (use_offsets) {
			field1 = static_cast<const char *>(key1) + key1_offsets[PartNo];
			field2 = static_cast<const char *>(key2) + key2_offsets[PartNo];
		}
		int rc = VdbeSorterCompareField<Kind>::exec(&field1, &field2);
		if (rc == -2) {
			return vdbeSorterCompareTemplateFallback<PartNo>(
				task, key2_cached, key1, key1_type_mask, key1_offsets,
				key2, key2_type_mask, key2_offsets, fallback);
		}
		if (rc != 0 &&
		    (desc_mask & static_cast<uint8_t>(1U << PartNo)) != 0)
			rc = -rc;
		return rc;
	}
};

template <std::size_t PartNo, VdbeSorterFastKind Kind, VdbeSorterFastKind Next,
	  VdbeSorterFastKind... Rest>
struct VdbeSorterCompareParts<PartNo, Kind, Next, Rest...> {
	static int
	exec(SortSubtask *task, bool *key2_cached, const void *key1,
	     uint8_t key1_type_mask, const uint16_t *key1_offsets,
	     const void *key2, uint8_t key2_type_mask,
	     const uint16_t *key2_offsets, uint8_t desc_mask,
	     VdbeSorterCompareFallback fallback, bool use_offsets,
	     const char *field1, const char *field2)
	{
		if (use_offsets) {
			field1 = static_cast<const char *>(key1) + key1_offsets[PartNo];
			field2 = static_cast<const char *>(key2) + key2_offsets[PartNo];
		}
		int rc = VdbeSorterCompareField<Kind>::exec(&field1, &field2);
		if (rc == -2) {
			return vdbeSorterCompareTemplateFallback<PartNo>(
				task, key2_cached, key1, key1_type_mask, key1_offsets,
				key2, key2_type_mask, key2_offsets, fallback);
		}
		if (rc != 0) {
			if ((desc_mask & static_cast<uint8_t>(1U << PartNo)) != 0)
				rc = -rc;
			return rc;
		}
		return VdbeSorterCompareParts<PartNo + 1, Next, Rest...>::exec(
			task, key2_cached, key1, key1_type_mask, key1_offsets, key2,
			key2_type_mask, key2_offsets, desc_mask, fallback, use_offsets,
			field1, field2);
	}
};

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
	return VdbeSorterCompareParts<0, Kinds...>::exec(
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

extern "C" int
vdbeSorterCompareCnpFieldString(const char **field1, const char **field2)
{
	return vdbeSorterCompareTemplateString(field1, field2);
}

extern "C" int
vdbeSorterCompareCnpFieldIntLike(const char **field1, const char **field2)
{
	return vdbeSorterCompareTemplateIntLike(mp_typeof(**field1), field1,
						mp_typeof(**field2), field2);
}

enum {
	VDBE_SORTER_CNP_ARENA_SIZE = 128 * 1024,
	CNP_R_X86_64_64 = 1,
	CNP_R_X86_64_PC32 = 2,
	CNP_R_X86_64_PLT32 = 4,
	CNP_R_X86_64_32 = 10,
	CNP_R_X86_64_32S = 11,
};

extern "C" int vdbeSorterCompareCnpTerminalEqualEntry(void);
extern "C" int vdbeSorterCompareCnpTerminalFallbackEntry(void);

/*
 * Cache one stitched body per mixed-key shape. The key is fully determined by
 * part count, DESC mask, and the per-part kind vector already derived from
 * key_def at sorter init time.
 */
struct VdbeSorterCnpShape {
	uint32_t part_count;
	uint16_t desc_mask;
	uint8_t part_kind[VDBE_SORTER_FAST_CMP_MAX_PARTS];
	void *code;
	VdbeSorterCnpShape *next;
};

static VdbeSorterCnpShape *g_sorter_cnp_shapes = nullptr;
static uint8_t *g_sorter_cnp_arena = nullptr;
static size_t g_sorter_cnp_arena_pos = 0;

static uint8_t *
vdbeSorterCnpArenaAlloc(size_t nbytes)
{
	nbytes = (nbytes + 15) & ~(size_t)15;
	if (g_sorter_cnp_arena == nullptr) {
		void *ptr = mmap(nullptr, VDBE_SORTER_CNP_ARENA_SIZE,
				 PROT_READ | PROT_WRITE | PROT_EXEC,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (ptr == MAP_FAILED)
			return nullptr;
		g_sorter_cnp_arena = static_cast<uint8_t *>(ptr);
		g_sorter_cnp_arena_pos = 0;
	}
	if (g_sorter_cnp_arena_pos + nbytes > VDBE_SORTER_CNP_ARENA_SIZE)
		return nullptr;
	uint8_t *res = g_sorter_cnp_arena + g_sorter_cnp_arena_pos;
	g_sorter_cnp_arena_pos += nbytes;
	return res;
}

/*
 * Minimal relocation patcher shared by the sorter-specific stitching path.
 *
 * Unlike the rejected raw-emitter approach, this code never invents new x86
 * instructions. It only copies bytes produced by clang from preserve-none
 * fragment templates and patches the extracted relocations.
 */
static void
vdbeSorterCnpPatch(uint8_t *patch_addr, uintptr_t target, uint8_t reloc_type,
		   int addend)
{
	switch (reloc_type) {
	case CNP_R_X86_64_64: {
		uint64_t value = (uint64_t)(target + (uintptr_t)addend);
		std::memcpy(patch_addr, &value, sizeof(value));
		break;
	}
	case CNP_R_X86_64_PC32:
	case CNP_R_X86_64_PLT32: {
		int64_t disp = (int64_t)target + addend -
			      ((int64_t)(uintptr_t)patch_addr + 4);
		int32_t value = (int32_t)disp;
		std::memcpy(patch_addr, &value, sizeof(value));
		break;
	}
	case CNP_R_X86_64_32: {
		uint32_t value = (uint32_t)(target + (uintptr_t)addend);
		std::memcpy(patch_addr, &value, sizeof(value));
		break;
	}
	case CNP_R_X86_64_32S: {
		int32_t value = (int32_t)((intptr_t)target + addend);
		std::memcpy(patch_addr, &value, sizeof(value));
		break;
	}
	default:
		break;
	}
}

/*
 * Pick one build-generated fragment template for a single key part.
 *
 * Today the long-tail stitched path only accepts STRING and INTLIKE parts.
 * Everything else stays on the generic mixed comparator until we add more
 * fragment kinds.
 */
static const struct sorter_cnp_fragment *
vdbeSorterCnpSelectFragment(uint8_t part_kind, bool is_desc)
{
	switch (part_kind) {
	case VDBE_SORTER_FAST_CMP_STRING:
		return &sorter_cnp_fragments[is_desc ? SORTER_CNP_FRAG_STRING_DESC :
						      SORTER_CNP_FRAG_STRING_ASC];
	case VDBE_SORTER_FAST_CMP_INTLIKE:
		return &sorter_cnp_fragments[is_desc ? SORTER_CNP_FRAG_INTLIKE_DESC :
						      SORTER_CNP_FRAG_INTLIKE_ASC];
	default:
		return nullptr;
	}
}

/*
 * Resolve the three relocation classes used by sorter fragments:
 *
 * 1. helper entry: string/intlike field comparator;
 * 2. next fragment: equal-prefix fallthrough to the next key part;
 * 3. fallback entry: escape back to the generic mixed comparator when a
 *    runtime value shape is not handled by the specialized path.
 */
static uintptr_t
vdbeSorterCnpResolveReloc(const struct sorter_cnp_fragment_reloc *rel,
			  void **part_addr, uint32_t part_count,
			  uint32_t part_no)
{
	if (std::strcmp(rel->symbol_name, "sorter_cnp_frag_next") == 0) {
		if (part_no + 1 < part_count)
			return (uintptr_t)part_addr[part_no + 1];
		return (uintptr_t)(void *)vdbeSorterCompareCnpTerminalEqualEntry;
	}
	if (std::strcmp(rel->symbol_name, "sorter_cnp_frag_fallback") == 0)
		return (uintptr_t)(void *)vdbeSorterCompareCnpTerminalFallbackEntry;
	if (std::strcmp(rel->symbol_name, "vdbeSorterCompareCnpFieldString") == 0)
		return (uintptr_t)(void *)vdbeSorterCompareCnpFieldString;
	if (std::strcmp(rel->symbol_name, "vdbeSorterCompareCnpFieldIntLike") == 0)
		return (uintptr_t)(void *)vdbeSorterCompareCnpFieldIntLike;
	return 0;
}

/*
 * Stitch one straight-line comparator body for an exact mixed sorter key.
 *
 * Each copied fragment keeps three logical exits:
 * - equal: jump to the next fragment;
 * - unsupported runtime shape: jump to fallback;
 * - ordered: return rc immediately.
 *
 * That is why the disassembly shows many small retq blocks: lexicographic
 * compare exits as soon as any part decides the ordering.
 */
static void *
vdbeSorterCnpCompile(uint32_t part_count, uint16_t desc_mask,
		     const uint8_t *part_kind)
{
	const struct sorter_cnp_fragment *parts[VDBE_SORTER_FAST_CMP_MAX_PARTS];
	void *part_addr[VDBE_SORTER_FAST_CMP_MAX_PARTS];
	size_t total_size = 0;

	/* Select one precompiled fragment template per key part. */
	for (uint32_t i = 0; i < part_count; ++i) {
		bool is_desc = (desc_mask & (uint16_t)(1U << i)) != 0;
		parts[i] = vdbeSorterCnpSelectFragment(part_kind[i], is_desc);
		if (parts[i] == nullptr)
			return nullptr;
		total_size += parts[i]->size;
	}

	uint8_t *code = vdbeSorterCnpArenaAlloc(total_size);
	if (code == nullptr)
		return nullptr;

	size_t pos = 0;
	/* First lay fragments out back-to-back, then patch their relocations. */
	for (uint32_t i = 0; i < part_count; ++i) {
		part_addr[i] = code + pos;
		std::memcpy(code + pos, parts[i]->bytes, parts[i]->size);
		pos += parts[i]->size;
	}

	for (uint32_t i = 0; i < part_count; ++i) {
		uint8_t *frag_code = static_cast<uint8_t *>(part_addr[i]);
		const struct sorter_cnp_fragment *frag = parts[i];
		/*
		 * Patch each copied template so:
		 * - helper calls point at the shared typed field comparators;
		 * - equal-prefix exits jump to the next copied fragment;
		 * - unsupported runtime values jump to the shared fallback entry.
		 */
		for (uint32_t r = 0; r < frag->num_relocs; ++r) {
			const struct sorter_cnp_fragment_reloc *rel = &frag->relocs[r];
			uintptr_t target = vdbeSorterCnpResolveReloc(rel, part_addr,
								 part_count, i);
			if (target == 0)
				return nullptr;
			vdbeSorterCnpPatch(frag_code + rel->offset, target,
					   rel->reloc_type, rel->addend);
		}
	}

	__builtin___clear_cache((char *)code, (char *)(code + total_size));
	return code;
}

extern "C" void *
vdbeSorterCompareCnpCodeGet(uint32_t part_count, uint16_t desc_mask,
			    const uint8_t *part_kind)
{
	/*
	 * The stitched tier only covers the long tail beyond the small static
	 * template matrix. Shorter hot shapes stay on direct C++ template
	 * entrypoints, and wider mixed layouts reuse one cached stitched body
	 * per exact sorter shape.
	 */
	if (part_count <= 4 || part_count > VDBE_SORTER_FAST_CMP_MAX_PARTS)
		return nullptr;
	for (uint32_t i = 0; i < part_count; ++i) {
		if (part_kind[i] != VDBE_SORTER_FAST_CMP_STRING &&
		    part_kind[i] != VDBE_SORTER_FAST_CMP_INTLIKE)
			return nullptr;
	}
	for (VdbeSorterCnpShape *it = g_sorter_cnp_shapes; it != nullptr;
	     it = it->next) {
		if (it->part_count != part_count || it->desc_mask != desc_mask)
			continue;
		if (std::memcmp(it->part_kind, part_kind, part_count) == 0)
			return it->code;
	}

	void *code = vdbeSorterCnpCompile(part_count, desc_mask, part_kind);
	if (code == nullptr)
		return nullptr;
	VdbeSorterCnpShape *shape = new VdbeSorterCnpShape();
	shape->part_count = part_count;
	shape->desc_mask = desc_mask;
	std::memset(shape->part_kind, 0, sizeof(shape->part_kind));
	std::memcpy(shape->part_kind, part_kind, part_count);
	shape->code = code;
	shape->next = g_sorter_cnp_shapes;
	g_sorter_cnp_shapes = shape;
	return code;
}

extern "C" VdbeSorterWriteTemplate
vdbeSorterWriterTemplateGet(uint32_t part_count, const uint8_t *part_kind)
{
	/*
	 * Bounded static writer tier. Keep only shapes that benchmarks proved
	 * hot and let the generic writer handle the long tail.
	 */
	if (part_count == 4 &&
	    part_kind[0] == VDBE_SORTER_FAST_CMP_STRING &&
	    part_kind[1] == VDBE_SORTER_FAST_CMP_INTLIKE &&
	    part_kind[2] == VDBE_SORTER_FAST_CMP_STRING &&
	    part_kind[3] == VDBE_SORTER_FAST_CMP_INTLIKE) {
		return vdbeSorterWriteTemplateStrIntStrInt4;
	}
	if (part_count == 10 &&
	    part_kind[0] == VDBE_SORTER_FAST_CMP_STRING &&
	    part_kind[1] == VDBE_SORTER_FAST_CMP_STRING &&
	    part_kind[2] == VDBE_SORTER_FAST_CMP_INTLIKE &&
	    part_kind[3] == VDBE_SORTER_FAST_CMP_STRING &&
	    part_kind[4] == VDBE_SORTER_FAST_CMP_INTLIKE &&
	    part_kind[5] == VDBE_SORTER_FAST_CMP_STRING &&
	    part_kind[6] == VDBE_SORTER_FAST_CMP_INTLIKE &&
	    part_kind[7] == VDBE_SORTER_FAST_CMP_STRING &&
	    part_kind[8] == VDBE_SORTER_FAST_CMP_INTLIKE &&
	    part_kind[9] == VDBE_SORTER_FAST_CMP_STRING) {
		return vdbeSorterWriteTemplateStrStrIntStrIntStrIntStrIntStr10;
	}
	return nullptr;
}

#if defined(__x86_64__)
/*
 * Tiny terminal entries used as relocation targets for the last fragment.
 *
 * They intentionally use the same preserve-none register world as the copied
 * fragment bodies, so the last stitched fragment can tail-jump here without
 * going through any extra C ABI bridge.
 */
extern "C" int __attribute__((naked))
vdbeSorterCompareCnpTerminalEqualEntry(void)
{
	__asm__ volatile(
		/* All stitched fragments matched, so the sorter keys are equal. */
		"xor %eax, %eax\n\t"
		"ret\n\t");
}

extern "C" int __attribute__((naked))
vdbeSorterCompareCnpTerminalFallbackEntry(void)
{
	__asm__ volatile(
		/* Return the out-of-line fallback sentinel to the C caller. */
		"mov $0x7fffffff, %eax\n\t"
		"ret\n\t");
}

extern "C" int __attribute__((naked))
vdbeSorterCompareCnpEnter(void *target, const char *field1, const char *field2)
{
	/*
	 * Bridge from normal SysV C into the sorter fragment ABI.
	 *
	 * The copied fragment bodies expect:
	 *   r12 = &field1
	 *   r13 = &field2
	 *
	 * The wide mixed comparator path passes raw MsgPack field cursors by
	 * address so helper calls can advance them in place across fragments.
	 */
	__asm__ volatile(
		"push %rbx\n\t"
		"push %rbp\n\t"
		"push %r12\n\t"
		"push %r13\n\t"
		"push %r14\n\t"
		"push %r15\n\t"
		"sub $24, %rsp\n\t"
		"mov %rdi, %rax\n\t"
		"mov %rsi, (%rsp)\n\t"
		"mov %rdx, 8(%rsp)\n\t"
		"lea (%rsp), %r12\n\t"
		"lea 8(%rsp), %r13\n\t"
		"call *%rax\n\t"
		"add $24, %rsp\n\t"
		"pop %r15\n\t"
		"pop %r14\n\t"
		"pop %r13\n\t"
		"pop %r12\n\t"
		"pop %rbp\n\t"
		"pop %rbx\n\t"
		"ret\n\t");
}
#else
extern "C" int
vdbeSorterCompareCnpEnter(void *target, const char *field1, const char *field2)
{
	(void)target;
	(void)field1;
	(void)field2;
	return VDBE_SORTER_COMPARE_CNP_FALLBACK;
}

extern "C" int
vdbeSorterCompareCnpTerminalEqualEntry(void)
{
	return 0;
}

extern "C" int
vdbeSorterCompareCnpTerminalFallbackEntry(void)
{
	return VDBE_SORTER_COMPARE_CNP_FALLBACK;
}
#endif
