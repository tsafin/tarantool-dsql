#include "vdbesort_templates.h"

extern "C" {
#include "msgpuck.h"
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <vector>

enum class VdbeSorterFastKind : uint8_t {
	IntLike = VDBE_SORTER_FAST_CMP_INTLIKE,
	String = VDBE_SORTER_FAST_CMP_STRING,
	Varbinary = VDBE_SORTER_FAST_CMP_VARBINARY,
	Bool = VDBE_SORTER_FAST_CMP_BOOL,
	Double = VDBE_SORTER_FAST_CMP_DOUBLE,
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

static inline int
vdbeSorterCompareTemplateVarbinary(const char **field1, const char **field2)
{
	if (mp_typeof(**field1) != MP_BIN || mp_typeof(**field2) != MP_BIN)
		return -2;
	uint32_t len1 = mp_decode_binl(field1);
	uint32_t len2 = mp_decode_binl(field2);
	uint32_t len = len1 < len2 ? len1 : len2;
	int rc = std::memcmp(*field1, *field2, len);
	if (rc == 0)
		rc = len1 < len2 ? -1 : len1 > len2 ? 1 : 0;
	*field1 += len1;
	*field2 += len2;
	return rc;
}

static inline int
vdbeSorterCompareTemplateBool(const char **field1, const char **field2)
{
	if (mp_typeof(**field1) != MP_BOOL || mp_typeof(**field2) != MP_BOOL)
		return -2;
	bool v1 = mp_decode_bool(field1);
	bool v2 = mp_decode_bool(field2);
	return v1 == v2 ? 0 : v1 ? 1 : -1;
}

static inline int
vdbeSorterCompareTemplateDouble(const char **field1, const char **field2)
{
	double v1, v2;
	enum mp_type t1 = mp_typeof(**field1);
	enum mp_type t2 = mp_typeof(**field2);
	if (t1 == MP_FLOAT)
		v1 = mp_decode_float(field1);
	else if (t1 == MP_DOUBLE)
		v1 = mp_decode_double(field1);
	else
		return -2;
	if (t2 == MP_FLOAT)
		v2 = mp_decode_float(field2);
	else if (t2 == MP_DOUBLE)
		v2 = mp_decode_double(field2);
	else
		return -2;
	return v1 < v2 ? -1 : v1 > v2 ? 1 : 0;
}

template <VdbeSorterFastKind Kind>
static inline int
vdbeSorterCompareTemplateField(const char **field1, const char **field2)
{
	if constexpr (Kind == VdbeSorterFastKind::String) {
		return vdbeSorterCompareTemplateString(field1, field2);
	} else if constexpr (Kind == VdbeSorterFastKind::IntLike) {
		return vdbeSorterCompareTemplateIntLike(mp_typeof(**field1), field1,
							mp_typeof(**field2), field2);
	} else if constexpr (Kind == VdbeSorterFastKind::Varbinary) {
		return vdbeSorterCompareTemplateVarbinary(field1, field2);
	} else if constexpr (Kind == VdbeSorterFastKind::Bool) {
		return vdbeSorterCompareTemplateBool(field1, field2);
	} else {
		return vdbeSorterCompareTemplateDouble(field1, field2);
	}
}

template <std::size_t PartNo>
static inline int
vdbeSorterCompareTemplateFallback(SortSubtask *task, bool *key2_cached,
				  const void *key1, uint8_t key1_type_mask,
				  const uint16_t *key1_offsets,
				  const void *key2, uint8_t key2_type_mask,
				  const uint16_t *key2_offsets,
				  VdbeSorterCompareFunc fallback)
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
			       const uint16_t *key2_offsets, uint16_t desc_mask,
			       VdbeSorterCompareFunc fallback, bool use_offsets,
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
		if ((desc_mask & static_cast<uint16_t>(1U << PartNo)) != 0)
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
			       const uint16_t *key2_offsets, uint16_t desc_mask,
			       VdbeSorterCompareFunc fallback)
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
				       uint16_t desc_mask,
				       VdbeSorterCompareFunc fallback)
{
	return vdbeSorterCompareTemplateFixed<VdbeSorterFastKind::String,
					      VdbeSorterFastKind::IntLike,
					      VdbeSorterFastKind::String,
					      VdbeSorterFastKind::IntLike>(
		task, key2_cached, key1, key1_type_mask, key1_offsets, key2,
		key2_type_mask, key2_offsets, desc_mask, fallback);
}

#if defined(__x86_64__)

static bool
vdbeSorterCompareJitInitFields(const void *key1, const void *key2,
			       uint32_t part_count, const char **field1,
			       const char **field2)
{
	*field1 = static_cast<const char *>(key1);
	*field2 = static_cast<const char *>(key2);
	return mp_decode_array(field1) >= part_count &&
	       mp_decode_array(field2) >= part_count;
}

static int
vdbeSorterCompareJitFieldIntLike(const char **field1, const char **field2)
{
	return vdbeSorterCompareTemplateIntLike(mp_typeof(**field1), field1,
						 mp_typeof(**field2), field2);
}

static int
vdbeSorterCompareJitFieldString(const char **field1, const char **field2)
{
	return vdbeSorterCompareTemplateString(field1, field2);
}

static int
vdbeSorterCompareJitFieldVarbinary(const char **field1, const char **field2)
{
	return vdbeSorterCompareTemplateVarbinary(field1, field2);
}

static int
vdbeSorterCompareJitFieldBool(const char **field1, const char **field2)
{
	return vdbeSorterCompareTemplateBool(field1, field2);
}

static int
vdbeSorterCompareJitFieldDouble(const char **field1, const char **field2)
{
	return vdbeSorterCompareTemplateDouble(field1, field2);
}

static void *
vdbeSorterCompareJitFieldHelper(uint8_t kind)
{
	switch (kind) {
	case VDBE_SORTER_FAST_CMP_INTLIKE:
		return reinterpret_cast<void *>(vdbeSorterCompareJitFieldIntLike);
	case VDBE_SORTER_FAST_CMP_STRING:
		return reinterpret_cast<void *>(vdbeSorterCompareJitFieldString);
	case VDBE_SORTER_FAST_CMP_VARBINARY:
		return reinterpret_cast<void *>(vdbeSorterCompareJitFieldVarbinary);
	case VDBE_SORTER_FAST_CMP_BOOL:
		return reinterpret_cast<void *>(vdbeSorterCompareJitFieldBool);
	case VDBE_SORTER_FAST_CMP_DOUBLE:
		return reinterpret_cast<void *>(vdbeSorterCompareJitFieldDouble);
	default:
		return nullptr;
	}
}

struct VdbeSorterJitShape {
	uint32_t part_count;
	uint16_t desc_mask;
	uint8_t part_kind[VDBE_SORTER_FAST_CMP_MAX_PARTS];
	VdbeSorterCompareFunc func;
	VdbeSorterJitShape *next;
};

struct VdbeSorterJitArena {
	uint8_t *base;
	size_t size;
	size_t pos;
};

static VdbeSorterJitShape *g_vdbe_sorter_jit_shapes = nullptr;
static VdbeSorterJitArena g_vdbe_sorter_jit_arena = {nullptr, 0, 0};

enum {
	VDBE_SORTER_JIT_ARENA_SIZE = 256 * 1024,
};

static uint8_t *
vdbeSorterJitArenaAlloc(size_t nbytes)
{
	nbytes = (nbytes + 15) & ~static_cast<size_t>(15);
	if (g_vdbe_sorter_jit_arena.base == nullptr) {
		void *ptr = mmap(nullptr, VDBE_SORTER_JIT_ARENA_SIZE,
				 PROT_READ | PROT_WRITE | PROT_EXEC,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (ptr == MAP_FAILED)
			return nullptr;
		g_vdbe_sorter_jit_arena.base = static_cast<uint8_t *>(ptr);
		g_vdbe_sorter_jit_arena.size = VDBE_SORTER_JIT_ARENA_SIZE;
		g_vdbe_sorter_jit_arena.pos = 0;
	}
	if (g_vdbe_sorter_jit_arena.pos + nbytes > g_vdbe_sorter_jit_arena.size)
		return nullptr;
	uint8_t *res = g_vdbe_sorter_jit_arena.base + g_vdbe_sorter_jit_arena.pos;
	g_vdbe_sorter_jit_arena.pos += nbytes;
	return res;
}

struct VdbeSorterJitEmitter {
	std::vector<uint8_t> code;

	void
	emit8(uint8_t v)
	{
		code.push_back(v);
	}

	void
	emit32(int32_t v)
	{
		uint8_t bytes[sizeof(v)];
		std::memcpy(bytes, &v, sizeof(v));
		code.insert(code.end(), bytes, bytes + sizeof(v));
	}

	void
	emit64(uint64_t v)
	{
		uint8_t bytes[sizeof(v)];
		std::memcpy(bytes, &v, sizeof(v));
		code.insert(code.end(), bytes, bytes + sizeof(v));
	}

	size_t
	pos() const
	{
		return code.size();
	}

	size_t
	emitJe()
	{
		emit8(0x0f);
		emit8(0x84);
		size_t at = pos();
		emit32(0);
		return at;
	}

	size_t
	emitJne()
	{
		emit8(0x0f);
		emit8(0x85);
		size_t at = pos();
		emit32(0);
		return at;
	}

	size_t
	emitJmp()
	{
		emit8(0xe9);
		size_t at = pos();
		emit32(0);
		return at;
	}

	void
	patchRel32(size_t at, size_t target)
	{
		int32_t disp = static_cast<int32_t>(target - (at + 4));
		std::memcpy(code.data() + at, &disp, sizeof(disp));
	}

	void
	emitMovAbsRax(void *ptr)
	{
		emit8(0x48);
		emit8(0xb8);
		emit64(reinterpret_cast<uint64_t>(ptr));
	}

	void
	emitCallAbs(void *ptr)
	{
		emitMovAbsRax(ptr);
		emit8(0xff);
		emit8(0xd0);
	}

	void
	emitPrologue()
	{
		emit8(0x55); /* push %rbp */
		emit8(0x48);
		emit8(0x89);
		emit8(0xe5); /* mov %rsp, %rbp */
		emit8(0x57); /* push %rdi */
		emit8(0x56); /* push %rsi */
		emit8(0x52); /* push %rdx */
		emit8(0x51); /* push %rcx */
		emit8(0x41);
		emit8(0x50); /* push %r8 */
		emit8(0x41);
		emit8(0x51); /* push %r9 */
		emit8(0x48);
		emit8(0x83);
		emit8(0xec);
		emit8(0x10); /* sub $16, %rsp */
	}

	void
	emitEpilogue()
	{
		emit8(0xc9); /* leave */
		emit8(0xc3); /* ret */
	}
};

static VdbeSorterCompareFunc
vdbeSorterCompileJitMixedCompare(uint32_t part_count, uint16_t desc_mask,
				 const uint8_t *part_kind,
				 VdbeSorterCompareFunc fallback)
{
	VdbeSorterJitEmitter e;
	e.code.reserve(256 + part_count * 48);
	e.emitPrologue();

	/* Bail out to the generic comparator if offset caches are in play. */
	e.emit8(0x48);
	e.emit8(0x83);
	e.emit8(0x7d);
	e.emit8(0xd8); /* [rbp-40] == saved r8 == key1_offsets */
	e.emit8(0x00);
	size_t key1_offsets_jne = e.emitJne();
	e.emit8(0x48);
	e.emit8(0x83);
	e.emit8(0x7d);
	e.emit8(0x18); /* [rbp+24] == key2_offsets */
	e.emit8(0x00);
	size_t key2_offsets_jne = e.emitJne();

	/* Initialize field1/field2 after the MsgPack array header. */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x7d);
	e.emit8(0xe8); /* mov -24(%rbp), %rdi */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x75);
	e.emit8(0xd0); /* mov -48(%rbp), %rsi */
	e.emit8(0xba);
	e.emit32(static_cast<int32_t>(part_count)); /* mov imm32, %edx */
	e.emit8(0x48);
	e.emit8(0x8d);
	e.emit8(0x4d);
	e.emit8(0xc8); /* lea -56(%rbp), %rcx */
	e.emit8(0x4c);
	e.emit8(0x8d);
	e.emit8(0x45);
	e.emit8(0xc0); /* lea -64(%rbp), %r8 */
	e.emitCallAbs(reinterpret_cast<void *>(vdbeSorterCompareJitInitFields));
	e.emit8(0x85);
	e.emit8(0xc0); /* test %eax, %eax */
	size_t init_je = e.emitJe();

	std::vector<size_t> fallback_patches;
	fallback_patches.push_back(key1_offsets_jne);
	fallback_patches.push_back(key2_offsets_jne);
	fallback_patches.push_back(init_je);
	std::vector<size_t> return_patches;

	for (uint32_t i = 0; i < part_count; ++i) {
		void *helper = vdbeSorterCompareJitFieldHelper(part_kind[i]);
		if (helper == nullptr)
			return nullptr;

		e.emit8(0x48);
		e.emit8(0x8d);
		e.emit8(0x7d);
		e.emit8(0xc8); /* lea -56(%rbp), %rdi */
		e.emit8(0x48);
		e.emit8(0x8d);
		e.emit8(0x75);
		e.emit8(0xc0); /* lea -64(%rbp), %rsi */
		e.emitCallAbs(helper);
		e.emit8(0x83);
		e.emit8(0xf8);
		e.emit8(0xfe); /* cmp $-2, %eax */
		fallback_patches.push_back(e.emitJe());
		e.emit8(0x85);
		e.emit8(0xc0); /* test %eax, %eax */
		size_t next_je = e.emitJe();
		if ((desc_mask & static_cast<uint16_t>(1U << i)) != 0) {
			e.emit8(0xf7);
			e.emit8(0xd8); /* neg %eax */
		}
		return_patches.push_back(e.emitJmp());
		e.patchRel32(next_je, e.pos());
	}

	e.emit8(0x31);
	e.emit8(0xc0); /* xor %eax, %eax */
	size_t success_pos = e.pos();
	e.emitEpilogue();

	size_t fallback_pos = e.pos();
	for (size_t at : fallback_patches)
		e.patchRel32(at, fallback_pos);

	/*
	 * Restore the original compare signature and tail out to the generic
	 * fallback. key2_type_mask and key2_offsets are the caller stack args.
	 */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x7d);
	e.emit8(0xf8); /* mov -8(%rbp), %rdi */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x75);
	e.emit8(0xf0); /* mov -16(%rbp), %rsi */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x55);
	e.emit8(0xe8); /* mov -24(%rbp), %rdx */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x4d);
	e.emit8(0xe0); /* mov -32(%rbp), %rcx */
	e.emit8(0x4c);
	e.emit8(0x8b);
	e.emit8(0x45);
	e.emit8(0xd8); /* mov -40(%rbp), %r8 */
	e.emit8(0x4c);
	e.emit8(0x8b);
	e.emit8(0x4d);
	e.emit8(0xd0); /* mov -48(%rbp), %r9 */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x45);
	e.emit8(0x18); /* mov 24(%rbp), %rax */
	e.emit8(0x50); /* push %rax (arg8) */
	e.emit8(0x48);
	e.emit8(0x8b);
	e.emit8(0x45);
	e.emit8(0x10); /* mov 16(%rbp), %rax */
	e.emit8(0x50); /* push %rax (arg7) */
	e.emitCallAbs(reinterpret_cast<void *>(fallback));
	e.emit8(0x48);
	e.emit8(0x83);
	e.emit8(0xc4);
	e.emit8(0x10); /* add $16, %rsp */
	e.emitEpilogue();

	for (size_t at : return_patches)
		e.patchRel32(at, success_pos);

	uint8_t *mem = vdbeSorterJitArenaAlloc(e.code.size());
	if (mem == nullptr)
		return nullptr;
	std::memcpy(mem, e.code.data(), e.code.size());
	return reinterpret_cast<VdbeSorterCompareFunc>(mem);
}

extern "C" VdbeSorterCompareFunc
vdbeSorterGetJitMixedCompare(uint32_t part_count, uint16_t desc_mask,
			     const uint8_t *part_kind,
			     VdbeSorterCompareFunc fallback)
{
	if (part_count <= 4 || part_count > VDBE_SORTER_FAST_CMP_MAX_PARTS)
		return nullptr;
	for (VdbeSorterJitShape *it = g_vdbe_sorter_jit_shapes; it != nullptr;
	     it = it->next) {
		if (it->part_count != part_count || it->desc_mask != desc_mask)
			continue;
		if (std::memcmp(it->part_kind, part_kind, part_count) == 0)
			return it->func;
	}
	VdbeSorterCompareFunc func =
		vdbeSorterCompileJitMixedCompare(part_count, desc_mask, part_kind,
						fallback);
	if (func == nullptr)
		return nullptr;
	VdbeSorterJitShape *shape = new VdbeSorterJitShape();
	shape->part_count = part_count;
	shape->desc_mask = desc_mask;
	std::memset(shape->part_kind, 0, sizeof(shape->part_kind));
	std::memcpy(shape->part_kind, part_kind, part_count);
	shape->func = func;
	shape->next = g_vdbe_sorter_jit_shapes;
	g_vdbe_sorter_jit_shapes = shape;
	return func;
}

#else

extern "C" VdbeSorterCompareFunc
vdbeSorterGetJitMixedCompare(uint32_t part_count, uint16_t desc_mask,
			     const uint8_t *part_kind,
			     VdbeSorterCompareFunc fallback)
{
	(void)part_count;
	(void)desc_mask;
	(void)part_kind;
	(void)fallback;
	return nullptr;
}

#endif
