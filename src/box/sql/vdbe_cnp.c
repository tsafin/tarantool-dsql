/*
 * Copy-and-Patch JIT compiler for VDBE.
 *
 * Copies pre-extracted machine code stencils into an mmap'd buffer,
 * patches operand holes and jump targets, then marks the buffer
 * executable.
 *
 * Dispatch model: each stencil is a regular function returning int64_t.
 * The return value is the address of the next stencil function to call.
 * The runtime loops calling stencils until a return value falls below a
 * threshold (indicating a terminal status code rather than an address).
 *
 * Resume model: OP_ResultRow calls cnp_signal_row() via HOLE_SIGNAL
 * to set p->cnp_row_ready.  The exec loop detects this flag, saves the
 * next stencil address in p->cnp_resume_func, and returns SQL_ROW to the
 * caller.  On the next call to vdbe_cnp_exec(), execution resumes from
 * p->cnp_resume_func.
 *
 * Coroutine model: coroutine stencils (Gosub/Return/Yield/InitCoroutine/
 * EndCoroutine) return (target_pc + CNP_PC_JUMP_BASE) instead of a stencil
 * address.  The exec loop uses p->cnp_pc_stencil[] to look up the target
 * stencil by PC index.
 */

#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <elf.h>
#undef EV_NONE

#include "sqlInt.h"
#include "vdbeInt.h"
#include "vdbe.h"
#include "vdbe_cnp.h"
#include "vdbe_ops.h"
#include "box/error.h"
#include "vdbe_cnp_vdbe_view.h"

#ifdef ENABLE_SQL_CNP

/*
 * Verify that CnpVdbeView field offsets match the real Vdbe layout.
 * These assertions catch any accidental struct reordering.
 */
_Static_assert(offsetof(struct Vdbe, pc) == offsetof(struct CnpVdbeView, pc),
	       "CnpVdbeView.pc offset mismatch");
_Static_assert(offsetof(struct Vdbe, iCompare) ==
		       offsetof(struct CnpVdbeView, iCompare),
	       "CnpVdbeView.iCompare offset mismatch");
#include "generated/vdbe_cnp_stencils.h"

/*
 * R_X86_64_32 (unsigned 32-bit absolute) — not in the stencils header
 * by default because we defined only the signed variant.  Add it here.
 */
#ifndef CNP_R_X86_64_32
#define CNP_R_X86_64_32 10
#endif

/* Counters exposed via box.stat.sql() */
extern int64_t sql_cnp_compile_count;
extern int64_t sql_cnp_compile_success_count;
extern int64_t sql_cnp_exec_count;
extern int64_t sql_cnp_step_count;
extern int64_t sql_cnp_resume_count;
extern int64_t sql_cnp_pc_jump_count;
extern int64_t sql_cnp_row_return_count;
extern int64_t sql_cnp_done_return_count;
extern int64_t sql_cnp_error_return_count;
extern int64_t sql_cnp_compiled_bytes;

/*
 * perf.map support (Linux `perf` JIT symbol resolution).
 *
 * When enabled, each compiled CnP program is registered in
 * /tmp/perf-<PID>.map so that `perf report --kallsyms` / `perf script`
 * can symbolicate JIT frames.
 *
 * Activation: set environment variable SQL_CNP_PERF_MAP=1 before
 * starting tarantool.
 *
 * Symbol naming: `vdbe_cnp_stmt_<stmt_id>_ops_<nOp>`.
 * SQL text is intentionally excluded from names to avoid leaking query
 * content into a world-visible file.
 *
 * File lifetime: opened once with O_TRUNC | O_WRONLY | O_CREAT at mode
 * 0600 so only the process owner can read it.  Entries are never removed
 * (perf.map has no unload semantics), but the arena is 8 MB and wraps, so
 * old entries may become stale after a wrap.
 */
static FILE  *g_cnp_perf_file = NULL;
static int    g_cnp_perf_enabled = -1; /* -1 = not checked yet */
static int64_t g_cnp_perf_seq = 0;

static void
cnp_symbol_name(char *buf, size_t size, const struct Vdbe *p)
{
	snprintf(buf, size, "vdbe_cnp_stmt_%08x_ops_%d",
		 (unsigned)p->stmt_id, p->nOp);
}

static size_t
cnp_align_up(size_t offset, size_t align)
{
	return (offset + align - 1) & ~(align - 1);
}

static void
cnp_perf_map_open(void)
{
	if (g_cnp_perf_enabled >= 0)
		return;
	const char *env = getenv("SQL_CNP_PERF_MAP");
	if (env == NULL || env[0] != '1') {
		g_cnp_perf_enabled = 0;
		return;
	}
	char path[64];
	snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		g_cnp_perf_enabled = 0;
		return;
	}
	g_cnp_perf_file = fdopen(fd, "w");
	if (g_cnp_perf_file == NULL) {
		close(fd);
		g_cnp_perf_enabled = 0;
		return;
	}
	/* Line buffering: each fprintf is one complete record. */
	setlinebuf(g_cnp_perf_file);
	g_cnp_perf_enabled = 1;
}

/*
 * Register one compiled program in /tmp/perf-PID.map.
 * Format required by Linux perf: "<hex_start> <hex_size> <name>\n"
 */
static void
cnp_perf_map_add(struct Vdbe *p, uint8_t *code, uint32_t size)
{
	cnp_perf_map_open();
	if (!g_cnp_perf_enabled)
		return;
	char sym[64];
	cnp_symbol_name(sym, sizeof(sym), p);
	fprintf(g_cnp_perf_file, "%lx %x %s\n",
		(unsigned long)(uintptr_t)code,
		(unsigned)size,
		sym);
	g_cnp_perf_seq++;
}

/*
 * .eh_frame CFI registration for stack unwinding through CnP frames.
 *
 * CnP stencils are plain function pointers with no prologue.  Without CFI
 * the CPU unwinder stops at the first CnP frame, producing "?? ()" in gdb bt.
 *
 * The CnP calling convention is uniform: CFA = RSP+8, RA = [RSP].
 * One static CIE covers every compiled program; each program gets one FDE.
 *
 * .eh_frame layout (x86-64, no augmentation, absolute 8-byte addresses):
 *
 *   CIE  [20 bytes]: length=16, id=0, ver=1, aug="", code_align=1,
 *                    data_align=-8, RA=16, DW_CFA_def_cfa(RSP,8),
 *                    DW_CFA_offset(RIP,1), nop pad
 *   FDE  [24 bytes]: length=20, cie_ptr, pc_begin(8), pc_range(8)
 *   Term [ 4 bytes]: 0x00000000 (required section terminator)
 *
 * Total: 48 bytes per compiled program.
 *
 * Without the 'z'/'R' augmentation in the CIE, the libgcc parser uses
 * native pointer width (8 bytes on x86-64) for pc_begin and pc_range.
 *
 * Always active when HAVE_REGISTER_FRAME is available (CMake check).
 * Overhead: one 48-byte malloc + __register_frame() per compile.
 */
#ifdef HAVE_REGISTER_FRAME

extern void __register_frame(const void *);
extern void __deregister_frame(const void *);

/*
 * Pre-encoded CIE (20 bytes).
 * Content after length field = 16 bytes (= length value):
 *   CIE id(4) version(1) aug(1) code_align(1) data_align(1) RA(1)
 *   DW_CFA_def_cfa(3) DW_CFA_offset(2) nop pad(2)
 */
static const uint8_t cnp_cie_template[20] = {
	/* length = 16 (content after this 4-byte field) */
	0x10, 0x00, 0x00, 0x00,
	/* CIE id = 0 */
	0x00, 0x00, 0x00, 0x00,
	/* version = 1 */
	0x01,
	/* augmentation = "" (NUL) */
	0x00,
	/* code_align_factor = 1 (uleb128) */
	0x01,
	/* data_align_factor = -8 (sleb128: 0x78) */
	0x78,
	/* return_address_register = 16 (RIP on x86-64) */
	0x10,
	/* DW_CFA_def_cfa: register=RSP(7), offset=8 */
	0x0c, 0x07, 0x08,
	/* DW_CFA_offset: register=RIP(16), factored_offset=1 */
	0x90, 0x01,
	/* DW_CFA_nop padding to reach 16 bytes of content */
	0x00, 0x00,
};

#define CNP_CIE_SIZE   20
/* FDE: length(4) + cie_ptr(4) + pc_begin(8) + pc_range(8) = 24 bytes total */
#define CNP_FDE_SIZE   24
/* Section terminator: 4-byte zero length record */
#define CNP_TERM_SIZE  4
#define CNP_EHFRAME_SIZE  (CNP_CIE_SIZE + CNP_FDE_SIZE + CNP_TERM_SIZE)

static void
cnp_register_frame(struct Vdbe *p)
{
	if (p->cnp_ehframe != NULL)
		return;

	uint8_t *buf = (uint8_t *)malloc(CNP_EHFRAME_SIZE);
	if (buf == NULL)
		return;

	/* CIE */
	memcpy(buf, cnp_cie_template, CNP_CIE_SIZE);

	/* FDE at buf + CNP_CIE_SIZE */
	uint8_t *fde = buf + CNP_CIE_SIZE;

	/* FDE length = content after length field = 20 */
	uint32_t fde_content_len = CNP_FDE_SIZE - 4;
	memcpy(fde + 0, &fde_content_len, 4);

	/* CIE pointer: distance from &fde[4] back to &buf[0].
	 * libgcc computes: cie = &fde_cie_ptr - cie_ptr_value */
	uint32_t cie_ptr = CNP_CIE_SIZE + 4;
	memcpy(fde + 4, &cie_ptr, 4);

	/* pc_begin: 8-byte absolute (no augmentation → native pointer width) */
	uint64_t code_addr = (uint64_t)(uintptr_t)p->cnp_code;
	memcpy(fde + 8, &code_addr, 8);

	/* pc_range: 8-byte size */
	uint64_t code_range = (uint64_t)p->cnp_size;
	memcpy(fde + 16, &code_range, 8);

	/* Section terminator */
	uint32_t term = 0;
	memcpy(buf + CNP_CIE_SIZE + CNP_FDE_SIZE, &term, 4);

	__register_frame(buf);
	p->cnp_ehframe = buf;
}

static void
cnp_deregister_frame(struct Vdbe *p)
{
	if (p->cnp_ehframe == NULL)
		return;
	__deregister_frame(p->cnp_ehframe);
	free(p->cnp_ehframe);
	p->cnp_ehframe = NULL;
}

#else /* !HAVE_REGISTER_FRAME */

static inline void cnp_register_frame(struct Vdbe *p)   { (void)p; }
static inline void cnp_deregister_frame(struct Vdbe *p) { (void)p; }

#endif /* HAVE_REGISTER_FRAME */

/*
 * GDB JIT registration for named CnP frames.
 *
 * Reuses the same __jit_debug_descriptor / __jit_debug_register_code
 * interface that LLVM MCJIT uses.  For each compiled program we build a
 * minimal in-memory ELF relocatable object containing a .text section with
 * the copied machine code and a global function symbol whose name matches the
 * perf/JITDUMP identity: vdbe_cnp_stmt_<stmt_id>_ops_<nOp>.
 *
 * With that object registered, GDB can show the generated function name in
 * frame #0, set breakpoints by symbol, and disassemble the JIT code by name.
 */
struct jit_code_entry {
	struct jit_code_entry *next_entry;
	struct jit_code_entry *prev_entry;
	const char *symfile_addr;
	uint64_t symfile_size;
};

struct jit_descriptor {
	uint32_t version;
	uint32_t action_flag;
	struct jit_code_entry *relevant_entry;
	struct jit_code_entry *first_entry;
};

enum {
	JIT_NOACTION = 0,
	JIT_REGISTER_FN = 1,
	JIT_UNREGISTER_FN = 2,
};

extern struct jit_descriptor __jit_debug_descriptor;
extern void __jit_debug_register_code(void);

#if !defined(ENABLE_SQL_JIT)
struct jit_descriptor __jit_debug_descriptor;

__attribute__((noinline)) void
__jit_debug_register_code(void)
{
}
#endif

struct cnp_gdb_entry {
	struct jit_code_entry jit;
	size_t symfile_size;
	uint8_t symfile[];
};

static struct cnp_gdb_entry *
cnp_build_gdb_symfile(struct Vdbe *p)
{
	static const char shstrtab[] =
		"\0.text\0.symtab\0.strtab\0.shstrtab\0";
	enum {
		SEC_NULL = 0,
		SEC_TEXT = 1,
		SEC_SYMTAB = 2,
		SEC_STRTAB = 3,
		SEC_SHSTRTAB = 4,
		SEC_COUNT = 5,
		SYM_NULL = 0,
		SYM_TEXT = 1,
		SYM_FUNC = 2,
		SYM_COUNT = 3,
	};
	char func_name[64];
	cnp_symbol_name(func_name, sizeof(func_name), p);
	size_t func_name_len = strlen(func_name) + 1;
	size_t strtab_size = 1 + func_name_len;

	size_t off = sizeof(Elf64_Ehdr);
	size_t text_off = cnp_align_up(off, 16);
	size_t text_size = p->cnp_size;
	size_t symtab_off = cnp_align_up(text_off + text_size, 8);
	size_t symtab_size = sizeof(Elf64_Sym) * SYM_COUNT;
	size_t strtab_off = symtab_off + symtab_size;
	size_t shstrtab_off = strtab_off + strtab_size;
	size_t shoff = cnp_align_up(shstrtab_off + sizeof(shstrtab), 8);
	size_t symfile_size = shoff + sizeof(Elf64_Shdr) * SEC_COUNT;

	struct cnp_gdb_entry *entry =
		(struct cnp_gdb_entry *)calloc(1, sizeof(*entry) + symfile_size);
	if (entry == NULL)
		return NULL;

	uint8_t *buf = entry->symfile;
	entry->symfile_size = symfile_size;
	entry->jit.symfile_addr = (const char *)buf;
	entry->jit.symfile_size = symfile_size;

	Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELFCLASS64;
	ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELFOSABI_NONE;
	ehdr->e_type = ET_REL;
	ehdr->e_machine = EM_X86_64;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_shentsize = sizeof(Elf64_Shdr);
	ehdr->e_shnum = SEC_COUNT;
	ehdr->e_shoff = shoff;
	ehdr->e_shstrndx = SEC_SHSTRTAB;

	memcpy(buf + text_off, p->cnp_code, text_size);

	char *strtab = (char *)(buf + strtab_off);
	size_t func_name_off = 1;
	strtab[0] = '\0';
	memcpy(strtab + func_name_off, func_name, func_name_len);
	memcpy(buf + shstrtab_off, shstrtab, sizeof(shstrtab));

	Elf64_Sym *symtab = (Elf64_Sym *)(buf + symtab_off);
	symtab[SYM_TEXT].st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
	symtab[SYM_TEXT].st_shndx = SEC_TEXT;
	symtab[SYM_FUNC].st_name = func_name_off;
	symtab[SYM_FUNC].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	symtab[SYM_FUNC].st_shndx = SEC_TEXT;
	symtab[SYM_FUNC].st_size = text_size;

	Elf64_Shdr *shdr = (Elf64_Shdr *)(buf + shoff);
	shdr[SEC_TEXT].sh_name = 1;
	shdr[SEC_TEXT].sh_type = SHT_PROGBITS;
	shdr[SEC_TEXT].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
	shdr[SEC_TEXT].sh_addr = (Elf64_Addr)(uintptr_t)p->cnp_code;
	shdr[SEC_TEXT].sh_offset = text_off;
	shdr[SEC_TEXT].sh_size = text_size;
	shdr[SEC_TEXT].sh_addralign = 16;

	shdr[SEC_SYMTAB].sh_name = 7;
	shdr[SEC_SYMTAB].sh_type = SHT_SYMTAB;
	shdr[SEC_SYMTAB].sh_offset = symtab_off;
	shdr[SEC_SYMTAB].sh_size = symtab_size;
	shdr[SEC_SYMTAB].sh_link = SEC_STRTAB;
	shdr[SEC_SYMTAB].sh_info = SYM_FUNC;
	shdr[SEC_SYMTAB].sh_addralign = 8;
	shdr[SEC_SYMTAB].sh_entsize = sizeof(Elf64_Sym);

	shdr[SEC_STRTAB].sh_name = 15;
	shdr[SEC_STRTAB].sh_type = SHT_STRTAB;
	shdr[SEC_STRTAB].sh_offset = strtab_off;
	shdr[SEC_STRTAB].sh_size = strtab_size;
	shdr[SEC_STRTAB].sh_addralign = 1;

	shdr[SEC_SHSTRTAB].sh_name = 23;
	shdr[SEC_SHSTRTAB].sh_type = SHT_STRTAB;
	shdr[SEC_SHSTRTAB].sh_offset = shstrtab_off;
	shdr[SEC_SHSTRTAB].sh_size = sizeof(shstrtab);
	shdr[SEC_SHSTRTAB].sh_addralign = 1;

	return entry;
}

static void
cnp_gdb_register(struct Vdbe *p)
{
	if (p->cnp_gdb_entry != NULL)
		return;

	struct cnp_gdb_entry *entry = cnp_build_gdb_symfile(p);
	if (entry == NULL)
		return;

	struct jit_code_entry *jit = &entry->jit;
	jit->prev_entry = NULL;
	jit->next_entry = __jit_debug_descriptor.first_entry;
	if (jit->next_entry != NULL)
		jit->next_entry->prev_entry = jit;

	__jit_debug_descriptor.relevant_entry = jit;
	__jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
	__jit_debug_descriptor.first_entry = jit;
	__jit_debug_register_code();
	p->cnp_gdb_entry = entry;
}

static void
cnp_gdb_deregister(struct Vdbe *p)
{
	struct cnp_gdb_entry *entry = (struct cnp_gdb_entry *)p->cnp_gdb_entry;
	if (entry == NULL)
		return;

	struct jit_code_entry *jit = &entry->jit;
	if (jit->prev_entry != NULL)
		jit->prev_entry->next_entry = jit->next_entry;
	else
		__jit_debug_descriptor.first_entry = jit->next_entry;
	if (jit->next_entry != NULL)
		jit->next_entry->prev_entry = jit->prev_entry;

	__jit_debug_descriptor.relevant_entry = jit;
	__jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
	__jit_debug_register_code();

	free(entry);
	p->cnp_gdb_entry = NULL;
}

static void
cnp_invalidate_program(struct Vdbe *p)
{
	cnp_gdb_deregister(p);
	cnp_deregister_frame(p);
	if (p->cnp_pc_stencil != NULL) {
		free(p->cnp_pc_stencil);
		p->cnp_pc_stencil = NULL;
	}
	p->cnp_code = NULL;
	p->cnp_size = 0;
	p->cnp_compiled = CNP_NOT_COMPILED;
	p->cnp_resume_func = NULL;
	p->cnp_row_ready = 0;
	p->cnp_nop = 0;
}

/*
 * JITDUMP support for per-opcode perf attribution via `perf inject --jit`.
 *
 * Activation: SQL_CNP_JITDUMP=1
 *
 * The JITDUMP binary file is mmap'd (perf reads it live).  We emit two
 * records per compiled program:
 *   JIT_CODE_LOAD        — symbol name + address + size
 *   JIT_CODE_DEBUG_INFO  — one entry per opcode (addr, lineno, name)
 *
 * Workflow:
 *   SQL_CNP_JITDUMP=1 VDBE_DISPATCHER=cnp perf record -k mono ./src/tarantool bench.lua
 *   perf inject --jit -i perf.data -o perf.jit.data
 *   perf report -i perf.jit.data --stdio
 */

/* JITDUMP record types */
#define JIT_CODE_LOAD       0
#define JIT_CODE_DEBUG_INFO 2

#pragma pack(push, 1)
struct jitdump_file_header {
	uint32_t magic;       /* 0x4A695444 "JiTD" LE, or "DTiJ" BE */
	uint32_t version;     /* 1 */
	uint32_t total_size;  /* sizeof(header) */
	uint32_t elf_mach;    /* EM_X86_64 = 62 */
	uint32_t pad1;
	uint32_t pid;
	uint64_t timestamp;   /* CLOCK_MONOTONIC nanoseconds */
	uint64_t flags;       /* 0 */
};

struct jitdump_record_header {
	uint32_t id;
	uint32_t total_size;
	uint64_t timestamp;
};

struct jitdump_code_load {
	struct jitdump_record_header header;
	uint32_t pid;
	uint32_t tid;
	uint64_t vma;
	uint64_t code_addr;
	uint64_t code_size;
	uint64_t code_index;
};

struct jitdump_debug_entry {
	uint64_t addr;
	uint32_t lineno;
	uint32_t discrim;
	/* followed by NUL-terminated filename string */
};

struct jitdump_code_debug_info {
	struct jitdump_record_header header;
	uint64_t code_addr;
	uint64_t nr_entry;
	/* followed by nr_entry jitdump_debug_entry + filename strings */
};
#pragma pack(pop)

/* Initial mmap size; grown with ftruncate+mremap as needed */
#define CNP_JITDUMP_INIT_SIZE (1024 * 1024)  /* 1 MB */

static int      g_cnp_jitdump_enabled = -1;
static int      g_cnp_jitdump_fd = -1;
static uint8_t *g_cnp_jitdump_map = NULL;
static size_t   g_cnp_jitdump_map_size = 0;
static size_t   g_cnp_jitdump_pos = 0;
static uint64_t g_cnp_jitdump_code_index = 0;

static uint64_t
cnp_jitdump_timestamp(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int
cnp_jitdump_grow(size_t needed)
{
	size_t new_size = g_cnp_jitdump_map_size;
	while (new_size - g_cnp_jitdump_pos < needed)
		new_size *= 2;
	if (new_size == g_cnp_jitdump_map_size)
		return 0;
	if (ftruncate(g_cnp_jitdump_fd, (off_t)new_size) != 0)
		return -1;
	void *new_map = mremap(g_cnp_jitdump_map,
			       g_cnp_jitdump_map_size, new_size,
			       MREMAP_MAYMOVE);
	if (new_map == MAP_FAILED)
		return -1;
	g_cnp_jitdump_map = (uint8_t *)new_map;
	g_cnp_jitdump_map_size = new_size;
	return 0;
}

static void
cnp_jitdump_open(void)
{
	if (g_cnp_jitdump_enabled >= 0)
		return;
	const char *env = getenv("SQL_CNP_JITDUMP");
	if (env == NULL || env[0] != '1') {
		g_cnp_jitdump_enabled = 0;
		return;
	}

	char path[64];
	snprintf(path, sizeof(path), "/tmp/jit-%d.dump", (int)getpid());
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		g_cnp_jitdump_enabled = 0;
		return;
	}
	if (ftruncate(fd, CNP_JITDUMP_INIT_SIZE) != 0) {
		close(fd);
		g_cnp_jitdump_enabled = 0;
		return;
	}
	void *map = mmap(NULL, CNP_JITDUMP_INIT_SIZE,
			 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		g_cnp_jitdump_enabled = 0;
		return;
	}
	g_cnp_jitdump_fd = fd;
	g_cnp_jitdump_map = (uint8_t *)map;
	g_cnp_jitdump_map_size = CNP_JITDUMP_INIT_SIZE;
	g_cnp_jitdump_pos = 0;

	/* Write file header */
	struct jitdump_file_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic      = 0x4A695444;
	hdr.version    = 1;
	hdr.total_size = sizeof(hdr);
	hdr.elf_mach   = 62; /* EM_X86_64 */
	hdr.pid        = (uint32_t)getpid();
	hdr.timestamp  = cnp_jitdump_timestamp();
	hdr.flags      = 0;

	memcpy(g_cnp_jitdump_map, &hdr, sizeof(hdr));
	g_cnp_jitdump_pos = sizeof(hdr);
	g_cnp_jitdump_enabled = 1;
}

static void
cnp_jitdump_write(struct Vdbe *p, int nOp, Op *aOp, uint32_t *pc_offset)
{
	cnp_jitdump_open();
	if (!g_cnp_jitdump_enabled)
		return;

	uint8_t *code = (uint8_t *)p->cnp_code;
	uint32_t code_size = p->cnp_size;
	uint64_t ts = cnp_jitdump_timestamp();
	uint64_t idx = g_cnp_jitdump_code_index++;

	/* Build symbol name */
	char sym[64];
	cnp_symbol_name(sym, sizeof(sym), p);
	int sym_len = (int)strlen(sym) + 1; /* include NUL */

	/* --- JIT_CODE_LOAD record --- */
	uint32_t load_size = (uint32_t)(sizeof(struct jitdump_code_load) +
					sym_len + code_size);
	if (cnp_jitdump_grow(load_size) != 0)
		return;

	struct jitdump_code_load load;
	memset(&load, 0, sizeof(load));
	load.header.id         = JIT_CODE_LOAD;
	load.header.total_size = load_size;
	load.header.timestamp  = ts;
	load.pid               = (uint32_t)getpid();
	load.tid               = (uint32_t)getpid();
	load.vma               = (uint64_t)(uintptr_t)code;
	load.code_addr         = (uint64_t)(uintptr_t)code;
	load.code_size         = code_size;
	load.code_index        = idx;

	memcpy(g_cnp_jitdump_map + g_cnp_jitdump_pos, &load, sizeof(load));
	g_cnp_jitdump_pos += sizeof(load);
	memcpy(g_cnp_jitdump_map + g_cnp_jitdump_pos, sym, sym_len);
	g_cnp_jitdump_pos += sym_len;
	memcpy(g_cnp_jitdump_map + g_cnp_jitdump_pos, code, code_size);
	g_cnp_jitdump_pos += code_size;

	/* --- JIT_CODE_DEBUG_INFO record --- */
	/* Each entry: jitdump_debug_entry + NUL-terminated opcode name */
	size_t debug_entries_size = 0;
	for (int i = 0; i < nOp; i++) {
		const char *name = sqlOpcodeName(aOp[i].opcode);
		debug_entries_size += sizeof(struct jitdump_debug_entry) +
				      strlen(name) + 1;
	}
	uint32_t debug_size = (uint32_t)(sizeof(struct jitdump_code_debug_info) +
					 debug_entries_size);
	if (cnp_jitdump_grow(debug_size) != 0)
		return;

	struct jitdump_code_debug_info dbg;
	memset(&dbg, 0, sizeof(dbg));
	dbg.header.id         = JIT_CODE_DEBUG_INFO;
	dbg.header.total_size = debug_size;
	dbg.header.timestamp  = ts;
	dbg.code_addr         = (uint64_t)(uintptr_t)code;
	dbg.nr_entry          = (uint64_t)nOp;

	memcpy(g_cnp_jitdump_map + g_cnp_jitdump_pos, &dbg, sizeof(dbg));
	g_cnp_jitdump_pos += sizeof(dbg);

	for (int i = 0; i < nOp; i++) {
		struct jitdump_debug_entry entry;
		entry.addr    = (uint64_t)(uintptr_t)(code + pc_offset[i]);
		entry.lineno  = (uint32_t)i;
		entry.discrim = 0;
		memcpy(g_cnp_jitdump_map + g_cnp_jitdump_pos,
		       &entry, sizeof(entry));
		g_cnp_jitdump_pos += sizeof(entry);

		const char *name = sqlOpcodeName(aOp[i].opcode);
		size_t nlen = strlen(name) + 1;
		memcpy(g_cnp_jitdump_map + g_cnp_jitdump_pos, name, nlen);
		g_cnp_jitdump_pos += nlen;
	}
}

/*
 * Code arena: one large RWX mmap shared across all CnP compilations.
 * Eliminates per-compile mmap+mprotect syscalls (~5 µs overhead each).
 *
 * Ring-buffer reset: when the bump pointer reaches the end, it wraps to
 * the beginning.  This is safe as long as no live stencil allocation spans
 * a wrap.  In practice, VDBE programs are prepared, executed, and released
 * before any wrap occurs.
 */
#define CNP_ARENA_SIZE (8 * 1024 * 1024)  /* 8 MB */

struct cnp_arena {
	uint8_t *base;
	size_t   size;
	size_t   pos;
};

static struct cnp_arena g_cnp_arena = {NULL, 0, 0};

static uint8_t *
cnp_arena_alloc(size_t nbytes)
{
	/* Align to 16 bytes so stencils start on a cache-line boundary. */
	nbytes = (nbytes + 15) & ~(size_t)15;

	if (g_cnp_arena.base == NULL) {
		g_cnp_arena.base =
			(uint8_t *)mmap(NULL, CNP_ARENA_SIZE,
					PROT_READ | PROT_WRITE | PROT_EXEC,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (g_cnp_arena.base == MAP_FAILED) {
			g_cnp_arena.base = NULL;
			return NULL;
		}
		g_cnp_arena.size = CNP_ARENA_SIZE;
		g_cnp_arena.pos = 0;
	}

	if (nbytes > g_cnp_arena.size)
		return NULL;

	if (g_cnp_arena.pos + nbytes > g_cnp_arena.size) {
		/*
		 * Arena wrap: position resets to 0.  All existing code in
		 * the arena will eventually be overwritten as the bump
		 * pointer advances.  Invalidate every compiled Vdbe so each
		 * is recompiled before its next execution.
		 */
		sql *db = sql_get();
		if (db != NULL) {
			uint8_t *lo = g_cnp_arena.base;
			uint8_t *hi = lo + g_cnp_arena.size;
			for (Vdbe *v = db->pVdbe; v != NULL; v = v->pNext) {
				if (v->cnp_compiled != CNP_COMPILED || v->cnp_code == NULL)
					continue;
				uint8_t *code = (uint8_t *)v->cnp_code;
				if (code >= lo && code < hi)
					cnp_invalidate_program(v);
			}
		}
		g_cnp_arena.pos = 0;  /* wrap */
	}

	uint8_t *ptr = g_cnp_arena.base + g_cnp_arena.pos;
	g_cnp_arena.pos += nbytes;
	return ptr;
}

/*
 * Stencil function type.  Each stencil returns the address of the
 * next stencil to execute, or a terminal/coroutine status code.
 */
typedef int64_t (*cnp_stencil_func_t)(struct Vdbe *p, Mem *aMem);

#if SQL_VDBE_OP_PROFILE
static int
cnp_find_pc_for_func(struct Vdbe *p, cnp_stencil_func_t func)
{
	void *target = (void *)func;
	for (int i = 0; i < p->cnp_nop; i++) {
		if (p->cnp_pc_stencil[i] == target)
			return i;
	}
	return -1;
}
#endif

/*
 * Threshold: return values below this are status/PC codes, not addresses.
 * Any valid mmap'd address will be well above this value.
 */
#define CNP_ADDR_THRESHOLD 4096

/*
 * Base added to coroutine target PCs in stencil return values.
 * Values 0 (error), 1 (SQL_ROW), 2 (SQL_DONE) are terminal codes.
 * Values CNP_PC_JUMP_BASE .. CNP_ADDR_THRESHOLD-1 encode PC jumps.
 */
#define CNP_PC_JUMP_BASE 3

/*
 * Signal function called by the OP_ResultRow stencil via HOLE_SIGNAL.
 * Sets p->cnp_row_ready so the exec loop knows to return SQL_ROW.
 */
static void
cnp_signal_row(struct Vdbe *p)
{
	p->cnp_row_ready = 1;
}

/*
 * OP_Init handler: set up the transaction, increment the OP_Once counter,
 * and return 1 (jump to P2).  Mirrors sql_vdbe_exec_init_for_jit().
 */
static int
vdbe_cnp_init_handler(struct Vdbe *p, struct VdbeOp *pOp, struct Mem *aMem)
{
	(void)aMem;
	if (p->pFrame == NULL && sql_vdbe_prepare(p) != 0)
		return -1;
	if (pOp->p1 >= sqlGlobalConfig.iOnceResetThreshold) {
		for (int i = 1; i < p->nOp; i++) {
			if (p->aOp[i].opcode == OP_Once)
				p->aOp[i].p1 = 0;
		}
		pOp->p1 = 0;
	}
	pOp->p1++;
	p->pc = pOp->p2;
	return 1;
}

/*
 * OP_Halt handler: commit/roll back the transaction via sqlVdbeHalt(),
 * then return 1 on success or -1 if the statement was aborted.
 */
static int
vdbe_cnp_halt_handler(struct Vdbe *p, struct VdbeOp *pOp, struct Mem *aMem)
{
	(void)aMem;
	if (pOp->p1 != 0)
		p->is_aborted = true;
	p->errorAction = (uint8_t)pOp->p2;
	sqlVdbeHalt(p);
	return p->is_aborted ? -1 : 1;
}

/*
 * OP_SetDiag handler: set the diagnostic error and return 1 if P2 != 0
 * (jump to P2), or 0 (fall through).
 */
static int
vdbe_cnp_setdiag_handler(struct Vdbe *p, struct VdbeOp *pOp, struct Mem *aMem)
{
	(void)p;
	(void)aMem;
	box_error_set(__FILE__, __LINE__, (uint32_t)pOp->p1, pOp->p4.z);
	return pOp->p2 != 0 ? 1 : 0;
}

/*
 * OP_TTransaction handler: start a Tarantool transaction if none is
 * active; otherwise create an anonymous savepoint.
 */
static int
vdbe_cnp_ttransaction_handler(struct Vdbe *p, struct VdbeOp *pOp,
			      struct Mem *aMem)
{
	(void)pOp;
	(void)aMem;
	if (!box_txn()) {
		if (txn_begin() == NULL)
			return -1;
	} else {
		p->anonymous_savepoint = txn_savepoint_new(in_txn(), NULL);
		if (p->anonymous_savepoint == NULL)
			return -1;
	}
	return 0;
}

/*
 * Return the C handler function address for HOLE_HANDLER patching.
 * Covers all 139 opcodes that have a HOLE_HANDLER hole (excludes
 * OP_Goto, OP_Jump which have no handler call, and OP_Program which
 * has no stencil).
 */
static uintptr_t
cnp_resolve_handler_by_opcode(int opcode)
{
	switch (opcode) {
	case OP_Concat:
		return (uintptr_t)vdbe_op_concat;
	case OP_Cast:
		return (uintptr_t)vdbe_op_cast;
	case OP_ApplyType:
		return (uintptr_t)vdbe_op_applytype;
	case OP_MakeRecord:
		return (uintptr_t)vdbe_op_makerecord;
	case OP_AggStep:
		return (uintptr_t)vdbe_op_aggstep;
	case OP_AggFinal:
		return (uintptr_t)vdbe_op_aggfinal;
	case OP_ResultRow:
		return (uintptr_t)vdbe_op_resultrow;
	case OP_Column:
		return (uintptr_t)vdbe_op_column;
	case OP_RowData:
		return (uintptr_t)vdbe_op_rowdata;
	case OP_Rewind:
		return (uintptr_t)vdbe_op_rewind;
	case OP_Last:
		return (uintptr_t)vdbe_op_last;
	/*
 * OP_Next/Prev/SorterNext: the raw handlers return 0=more rows
 * (jump to P2) and 1=exhausted (fall through). The dispatch
 * wrapper branches on rc==0, but the CnP jump stencil branches
 * on rc>0.  Use the _jit variants which invert the return value
 * and also maintain cacheStatus/nullRow (normally set by the
 * wrapper post-call).
 */
	case OP_Next:
		return (uintptr_t)vdbe_op_next_jit;
	case OP_NextIfOpen:
		return (uintptr_t)vdbe_op_nextifopen_jit;
	case OP_Prev:
		return (uintptr_t)vdbe_op_prev_jit;
	case OP_PrevIfOpen:
		return (uintptr_t)vdbe_op_previfopen_jit;
	case OP_SeekLE:
		return (uintptr_t)vdbe_op_seek_le_ge;
	case OP_SeekGT:
		return (uintptr_t)vdbe_op_seek_lt_gt;
	case OP_SeekGE:
		return (uintptr_t)vdbe_op_seek_le_ge;
	case OP_SeekLT:
		return (uintptr_t)vdbe_op_seek_lt_gt;
	case OP_IdxGE:
		return (uintptr_t)vdbe_op_idx_compare;
	case OP_IdxGT:
		return (uintptr_t)vdbe_op_idx_compare;
	case OP_IdxLE:
		return (uintptr_t)vdbe_op_idx_compare;
	case OP_IdxLT:
		return (uintptr_t)vdbe_op_idx_compare;
	case OP_Found:
		return (uintptr_t)vdbe_op_found_notfound_noconflict;
	case OP_NotFound:
		return (uintptr_t)vdbe_op_found_notfound_noconflict;
	case OP_NoConflict:
		return (uintptr_t)vdbe_op_found_notfound_noconflict;
	case OP_IdxInsert:
		return (uintptr_t)vdbe_op_idx_insert_replace;
	case OP_IdxReplace:
		return (uintptr_t)vdbe_op_idx_insert_replace;
	case OP_Delete:
		return (uintptr_t)vdbe_op_delete;
	case OP_Update:
		return (uintptr_t)vdbe_op_update;
	case OP_SInsert:
		return (uintptr_t)vdbe_op_sinsert;
	case OP_SDelete:
		return (uintptr_t)vdbe_op_sdelete;
	case OP_IdxDelete:
		return (uintptr_t)vdbe_op_idxdelete;
	case OP_Add:
		return (uintptr_t)vdbe_op_add;
	case OP_Subtract:
		return (uintptr_t)vdbe_op_sub;
	case OP_Multiply:
		return (uintptr_t)vdbe_op_multiply;
	case OP_Divide:
		return (uintptr_t)vdbe_op_divide;
	case OP_Remainder:
		return (uintptr_t)vdbe_op_remainder;
	case OP_Eq:
		return (uintptr_t)vdbe_op_eq;
	case OP_Ne:
		return (uintptr_t)vdbe_op_ne;
	case OP_Lt:
		return (uintptr_t)vdbe_op_lt;
	case OP_Le:
		return (uintptr_t)vdbe_op_le;
	case OP_Gt:
		return (uintptr_t)vdbe_op_gt;
	case OP_Ge:
		return (uintptr_t)vdbe_op_ge;
	case OP_And:
		return (uintptr_t)vdbe_op_and;
	case OP_Or:
		return (uintptr_t)vdbe_op_or;
	case OP_Not:
		return (uintptr_t)vdbe_op_not;
	case OP_BitAnd:
		return (uintptr_t)vdbe_op_bitand;
	case OP_BitOr:
		return (uintptr_t)vdbe_op_bitor;
	case OP_BitNot:
		return (uintptr_t)vdbe_op_bitnot;
	case OP_Integer:
		return (uintptr_t)vdbe_op_integer;
	case OP_Bool:
		return (uintptr_t)vdbe_op_bool;
	case OP_Int64:
		return (uintptr_t)vdbe_op_int64;
	case OP_Real:
		return (uintptr_t)vdbe_op_real;
	case OP_String:
		return (uintptr_t)vdbe_op_string;
	case OP_Null:
		return (uintptr_t)vdbe_op_null;
	case OP_Blob:
		return (uintptr_t)vdbe_op_blob;
	case OP_Variable:
		return (uintptr_t)vdbe_op_variable;
	case OP_Move:
		return (uintptr_t)vdbe_op_move;
	case OP_Copy:
		return (uintptr_t)vdbe_op_copy;
	case OP_SCopy:
		return (uintptr_t)vdbe_op_scopy;
	case OP_If:
		return (uintptr_t)vdbe_op_ifnot_inline;
	case OP_IfNot:
		return (uintptr_t)vdbe_op_ifnot_inline;
	case OP_Once:
		return (uintptr_t)vdbe_op_once_inline;
	case OP_Gosub:
		return (uintptr_t)vdbe_op_gosub_jit;
	case OP_Return:
		return (uintptr_t)vdbe_op_return_jit;
	case OP_InitCoroutine:
		return (uintptr_t)vdbe_op_initcoroutine_jit;
	case OP_Yield:
		return (uintptr_t)vdbe_op_yield_jit;
	case OP_EndCoroutine:
		return (uintptr_t)vdbe_op_endcoroutine_jit;
	case OP_ElseNotEq:
		return (uintptr_t)vdbe_op_elsenoteq_inline;
	case OP_MustBeInt:
		return (uintptr_t)vdbe_op_mustbeint;
	case OP_IfPos:
		return (uintptr_t)vdbe_op_ifpos_inline;
	case OP_IfNotZero:
		return (uintptr_t)vdbe_op_ifnotzero_inline;
	case OP_DecrJumpZero:
		return (uintptr_t)vdbe_op_decrjumpzero_inline;
	case OP_SetDiag:
		return (uintptr_t)vdbe_cnp_setdiag_handler;
	case OP_Halt:
		return (uintptr_t)vdbe_cnp_halt_handler;
	case OP_Init:
		return (uintptr_t)vdbe_cnp_init_handler;
	case OP_Savepoint:
		return (uintptr_t)vdbe_op_savepoint_inline;
	/* SorterNext same inverted semantics as OP_Next - use jit variant */
	case OP_SorterNext:
		return (uintptr_t)vdbe_op_sorternext_jit;
	case OP_String8:
		return (uintptr_t)vdbe_op_string8;
	case OP_SkipLoad:
		return (uintptr_t)vdbe_op_skipload_inline;
	case OP_BuiltinFunction:
		return (uintptr_t)vdbe_op_builtinfunction;
	case OP_FunctionByName:
		return (uintptr_t)vdbe_op_functionbyname;
	case OP_AddImm:
		return (uintptr_t)vdbe_op_addimm_inline;
	case OP_Array:
		return (uintptr_t)vdbe_op_array_inline;
	case OP_Map:
		return (uintptr_t)vdbe_op_map_inline;
	case OP_Getitem:
		return (uintptr_t)vdbe_op_getitem_inline;
	case OP_Permutation:
		return (uintptr_t)vdbe_op_permutation_inline;
	case OP_Compare:
		return (uintptr_t)vdbe_op_compare;
	case OP_FetchByName:
		return (uintptr_t)vdbe_op_fetchbyname_inline;
	case OP_Fetch:
		return (uintptr_t)vdbe_op_fetch_inline;
	case OP_Count:
		return (uintptr_t)vdbe_op_count_inline;
	case OP_CreateForeignKey:
		return (uintptr_t)vdbe_op_createforeignkey_inline;
	case OP_CreateCheck:
		return (uintptr_t)vdbe_op_createcheck_inline;
	case OP_DropTupleForeignKey:
		return (uintptr_t)vdbe_op_droptupleforeignkey_inline;
	case OP_DropTupleCheck:
		return (uintptr_t)vdbe_op_droptuplecheckundidocheck_inline;
	case OP_DropFieldForeignKey:
		return (uintptr_t)vdbe_op_dropfieldforeignkey_inline;
	case OP_DropFieldCheck:
		return (uintptr_t)vdbe_op_dropfieldcheck_inline;
	case OP_AddFuncDefault:
		return (uintptr_t)vdbe_op_addfuncdefault_inline;
	case OP_CheckViewReferences:
		return (uintptr_t)vdbe_op_checkviewreferences_inline;
	case OP_TransactionBegin:
		return (uintptr_t)vdbe_op_transactionbegin_inline;
	case OP_TransactionCommit:
		return (uintptr_t)vdbe_op_transactioncommit_inline;
	case OP_TransactionRollback:
		return (uintptr_t)vdbe_op_transactionrollback_inline;
	case OP_TTransaction:
		return (uintptr_t)vdbe_cnp_ttransaction_handler;
	case OP_IteratorOpen:
		return (uintptr_t)vdbe_op_iteratoropen;
	case OP_OpenSpace:
		return (uintptr_t)vdbe_op_openspace_inline;
	case OP_OpenTEphemeral:
		return (uintptr_t)vdbe_op_opentephemeral_inline;
	case OP_SorterOpen:
		return (uintptr_t)vdbe_op_sorteropen;
	case OP_SequenceTest:
		return (uintptr_t)vdbe_op_sequencetest_inline;
	case OP_OpenPseudo:
		return (uintptr_t)vdbe_op_openpseudo_inline;
	case OP_Close:
		return (uintptr_t)vdbe_op_close_inline;
	case OP_Sequence:
		return (uintptr_t)vdbe_op_sequence_inline;
	case OP_NextSystemSpaceId:
		return (uintptr_t)vdbe_op_nextsystemspaceid_inline;
	case OP_NextIdEphemeral:
		return (uintptr_t)vdbe_op_nextidephemeral_inline;
	case OP_FCopy:
		return (uintptr_t)vdbe_op_fcopy_inline;
	case OP_ResetCount:
		return (uintptr_t)vdbe_op_resetcount_inline;
	case OP_SorterCompare:
		return (uintptr_t)vdbe_op_sortercompare;
	case OP_SorterData:
		return (uintptr_t)vdbe_op_sorterdata;
	case OP_NullRow:
		return (uintptr_t)vdbe_op_nullrow_inline;
	case OP_SorterInsert:
		return (uintptr_t)vdbe_op_sorterinsert;
	case OP_Clear:
		return (uintptr_t)vdbe_op_clear_inline;
	case OP_ResetSorter:
		return (uintptr_t)vdbe_op_resetsorter_inline;
	case OP_RenameTable:
		return (uintptr_t)vdbe_op_renametable_inline;
	case OP_LoadAnalysis:
		return (uintptr_t)vdbe_op_loadanalysis_inline;
	case OP_Param:
		return (uintptr_t)vdbe_op_param_inline;
	case OP_OffsetLimit:
		return (uintptr_t)vdbe_op_offsetlimit;
	case OP_Expire:
		return (uintptr_t)vdbe_op_expire_inline;
	case OP_GenSpaceid:
		return (uintptr_t)vdbe_op_genspaceid_inline;
	case OP_SetSession:
		return (uintptr_t)vdbe_op_setsession;
	case OP_ShowCreateTable:
		return (uintptr_t)vdbe_op_showcreatettable_inline;
	case OP_Noop:
		return (uintptr_t)vdbe_op_noop_inline;
	case OP_Explain:
		return (uintptr_t)vdbe_op_explain_inline;
	case OP_IsNull:
		return (uintptr_t)vdbe_op_isnull_inline;
	case OP_NotNull:
		return (uintptr_t)vdbe_op_notnull_inline;
	case OP_Decimal:
		return (uintptr_t)vdbe_op_decimal_inline;
	case OP_Sort:
		/* OP_Sort is an alias for OP_Rewind: it positions the ephemeral
		 * B-tree cursor at the first row (smallest key). The interpreter
		 * implements this via a C-level fallthrough from OP_Sort into
		 * OP_Rewind. CnP must call vdbe_op_rewind explicitly. */
		return (uintptr_t)vdbe_op_rewind;
	case OP_SorterSort:
		return (uintptr_t)vdbe_op_sortersort;
	case OP_ShiftLeft:
		return (uintptr_t)vdbe_op_shiftleft_inline;
	case OP_ShiftRight:
		return (uintptr_t)vdbe_op_shiftright_inline;
	default:
		return 0;
	}
}

/*
 * Apply a single relocation patch to the code buffer.
 */
static void
cnp_patch(uint8_t *patch_addr, uintptr_t target, uint8_t reloc_type, int addend)
{
	switch (reloc_type) {
	case CNP_R_X86_64_64: {
		uint64_t val = (uint64_t)target + addend;
		memcpy(patch_addr, &val, 8);
		break;
	}
	case CNP_R_X86_64_PC32:
	case CNP_R_X86_64_PLT32: {
		uintptr_t P = (uintptr_t)patch_addr;
		int32_t val = (int32_t)((int64_t)target + addend - (int64_t)P);
		memcpy(patch_addr, &val, 4);
		break;
	}
	case CNP_R_X86_64_32: {
		uint32_t val = (uint32_t)((uint64_t)target + addend);
		memcpy(patch_addr, &val, 4);
		break;
	}
	case CNP_R_X86_64_32S: {
		int32_t val = (int32_t)((int64_t)target + addend);
		memcpy(patch_addr, &val, 4);
		break;
	}
	default:
		fprintf(stderr, "cnp: unsupported reloc type %d\n", reloc_type);
		break;
	}
}

int
vdbe_cnp_compile(struct Vdbe *p)
{
	if (p->cnp_compiled == CNP_COMPILED)
		return 0;
	if (p->cnp_compiled == CNP_COMPILE_FAILED)
		return -1;
	if (p->cnp_code != NULL || p->cnp_pc_stencil != NULL ||
	    p->cnp_ehframe != NULL || p->cnp_gdb_entry != NULL)
		cnp_invalidate_program(p);

	sql_cnp_compile_count++;

	int nOp = p->nOp;
	Op *aOp = p->aOp;

	/*
 * Phase 1+3 (merged): single pass over opcodes to:
 *   a) verify every opcode has a stencil,
 *   b) compute total code buffer size,
 *   c) detect coroutine opcodes (Gosub/Return/Yield/InitCoroutine/EndCoroutine)
 *      which are the only callers of the pc_stencil lookup table.
 *
 * pc_offset is a temporary array used only during compile to resolve
 * branch targets.  Allocate from the stack for small programs (covers
 * the vast majority of real queries) and fall back to heap for large.
 */
#define CNP_STACK_OP_LIMIT 128
	uint32_t pc_offset_stack[CNP_STACK_OP_LIMIT + 1];
	uint32_t *pc_offset;
	int pc_offset_heap = 0;

	uint32_t total_size = 0;
	for (int i = 0; i < nOp; i++) {
		int opcode = aOp[i].opcode;
		if (opcode > CNP_MAX_OPCODE ||
		    cnp_stencils[opcode].bytes == NULL) {
			p->cnp_compiled = CNP_COMPILE_FAILED;
			return -1;
		}
		total_size += cnp_stencils[opcode].size;
	}

	if (nOp + 1 <= CNP_STACK_OP_LIMIT + 1) {
		pc_offset = pc_offset_stack;
		memset(pc_offset, 0, (nOp + 1) * sizeof(uint32_t));
	} else {
		pc_offset = (uint32_t *)calloc(nOp + 1, sizeof(uint32_t));
		if (pc_offset == NULL)
			return -1;
		pc_offset_heap = 1;
	}

	/*
 * Phase 2: Allocate code buffer from the shared RWX arena.
 * This avoids a per-compile mmap+mprotect pair (~5 µs overhead).
 */
	uint8_t *code = cnp_arena_alloc(total_size);
	if (code == NULL) {
		if (pc_offset_heap)
			free(pc_offset);
		return -1;
	}

	/*
 * Phase 3: Copy stencils, build pc-to-offset map, and populate
 * pc_stencil so profiling, debugger helpers, and coroutine jumps can
 * map between pc and native address.
 */
	void **pc_stencil = (void **)calloc(nOp, sizeof(void *));
	if (pc_stencil == NULL) {
		if (pc_offset_heap)
			free(pc_offset);
		return -1;
	}

	uint32_t pos = 0;
	for (int i = 0; i < nOp; i++) {
		pc_offset[i] = pos;
		const struct cnp_stencil *st = &cnp_stencils[aOp[i].opcode];
		memcpy(code + pos, st->bytes, st->size);
		pos += st->size;
	}
	pc_offset[nOp] = pos;

	for (int i = 0; i < nOp; i++)
		pc_stencil[i] = code + pc_offset[i];

	/*
 * Phase 4: Patch holes.
 *
 * HOLE_OP        — pointer to real VdbeOp (replaces CnpOp)
 * HOLE_CPC       — current PC integer value (for coroutine ops)
 * HOLE_NEXT      — address of the next stencil (or SQL_DONE)
 * HOLE_BRANCH    — jump target (P2), or SQL_DONE for OP_Halt
 * HOLE_BRANCH_P1 — OP_Jump branch to stencil at P1
 * HOLE_BRANCH_P3 — OP_Jump branch to stencil at P3
 * HOLE_SKIP2     — address of stencil at i+2 (SeekGE/SeekLE)
 * HOLE_SIGNAL    — address of cnp_signal_row (OP_ResultRow)
 * HOLE_HANDLER   — address of opcode's C handler function
 * HOLE_ERROR_EXIT — SQL error code (-1 cast to uintptr_t)
 */
	for (int i = 0; i < nOp; i++) {
		const struct cnp_stencil *st = &cnp_stencils[aOp[i].opcode];
		uint32_t base = pc_offset[i];

		for (uint32_t h = 0; h < st->num_holes; h++) {
			const struct cnp_hole *hole = &st->holes[h];
			uint8_t *patch_addr = code + base + hole->offset;
			uintptr_t target = 0;

			switch (hole->kind) {
			case CNP_HOLE_P1:
				target = (uintptr_t)aOp[i].p1;
				break;
			case CNP_HOLE_P2:
				target = (uintptr_t)aOp[i].p2;
				break;
			case CNP_HOLE_P3:
				target = (uintptr_t)aOp[i].p3;
				break;
			case CNP_HOLE_P4:
				target = 0;
				break;
			case CNP_HOLE_P5:
				target = (uintptr_t)aOp[i].p5;
				break;
			case CNP_HOLE_OP:
				target = (uintptr_t)&aOp[i];
				break;
			case CNP_HOLE_CPC:
				target = (uintptr_t)(uintptr_t)i;
				break;
			case CNP_HOLE_NEXT:
				if (i + 1 < nOp)
					target = (uintptr_t)(code +
							     pc_offset[i + 1]);
				else
					target = (uintptr_t)SQL_DONE;
				break;
			case CNP_HOLE_BRANCH:
				if (aOp[i].opcode == OP_Halt) {
					target = (uintptr_t)SQL_DONE;
				} else if (aOp[i].p2 >= 0 && aOp[i].p2 < nOp) {
					target = (uintptr_t)(
						code + pc_offset[aOp[i].p2]);
				} else {
					target = (uintptr_t)SQL_DONE;
				}
				break;
			case CNP_HOLE_BRANCH_P1:
				if (aOp[i].p1 >= 0 && aOp[i].p1 < nOp)
					target = (uintptr_t)(
						code + pc_offset[aOp[i].p1]);
				else
					target = (uintptr_t)SQL_DONE;
				break;
			case CNP_HOLE_BRANCH_P3:
				if (aOp[i].p3 >= 0 && aOp[i].p3 < nOp)
					target = (uintptr_t)(
						code + pc_offset[aOp[i].p3]);
				else
					target = (uintptr_t)SQL_DONE;
				break;
			case CNP_HOLE_SKIP2:
				if (i + 2 < nOp)
					target = (uintptr_t)(code +
							     pc_offset[i + 2]);
				else
					target = (uintptr_t)SQL_DONE;
				break;
			case CNP_HOLE_SIGNAL:
				target = (uintptr_t)cnp_signal_row;
				break;
			case CNP_HOLE_HANDLER:
				target = cnp_resolve_handler_by_opcode(
					aOp[i].opcode);
				break;
			case CNP_HOLE_ERROR_EXIT:
				target = (uintptr_t)(intptr_t)(-1);
				break;
			default:
				break;
			}

			cnp_patch(patch_addr, target, hole->reloc_type,
				  hole->addend);
		}
	}

	/*
 * Phase 5: Flush instruction cache (no-op on x86_64; required on ARM).
 * No mprotect needed — the arena is already RWX.
 */
	__builtin___clear_cache((char *)code, (char *)(code + total_size));

	p->cnp_code = code;
	p->cnp_size = total_size;
	p->cnp_compiled = CNP_COMPILED;
	p->cnp_pc_stencil = pc_stencil;
	p->cnp_nop = nOp;
	sql_cnp_compile_success_count++;
	sql_cnp_compiled_bytes += total_size;
	cnp_perf_map_add(p, code, total_size);
	cnp_register_frame(p);
	cnp_gdb_register(p);
	/* jitdump_write uses pc_offset; call before freeing it */
	cnp_jitdump_write(p, nOp, aOp, pc_offset);

	if (pc_offset_heap)
		free(pc_offset);
	return 0;
}

int
vdbe_cnp_exec(struct Vdbe *p)
{
	if (p->cnp_compiled != CNP_COMPILED)
		return -1;

	sql_cnp_exec_count++;

	/*
 * Resume from the stencil after OP_ResultRow if the previous call
 * returned SQL_ROW.
 */
	cnp_stencil_func_t func;
#if SQL_VDBE_OP_PROFILE
	int current_pc = 0;
#endif
	if (p->cnp_resume_func != NULL) {
		sql_cnp_resume_count++;
		func = (cnp_stencil_func_t)p->cnp_resume_func;
		p->cnp_resume_func = NULL;
#if SQL_VDBE_OP_PROFILE
		current_pc = cnp_find_pc_for_func(p, func);
#endif
	} else {
		func = (cnp_stencil_func_t)p->cnp_code;
	}

	int64_t result;
	for (;;) {
#if SQL_VDBE_OP_PROFILE
		int opcode = (current_pc >= 0 && current_pc < p->nOp) ?
			     p->aOp[current_pc].opcode : -1;
		int64_t start_us = fiber_clock64();
#endif
		result = func(p, p->aMem);
		sql_cnp_step_count++;
#if SQL_VDBE_OP_PROFILE
		sql_vdbe_opcode_profile_record_cnp(opcode,
						   fiber_clock64() - start_us);
#endif

		if (result >= (int64_t)CNP_ADDR_THRESHOLD) {
			/* Normal: result is the address of the next stencil. */
			func = (cnp_stencil_func_t)result;
#if SQL_VDBE_OP_PROFILE
			current_pc = cnp_find_pc_for_func(p, func);
#endif
		} else if (result >= CNP_PC_JUMP_BASE) {
			sql_cnp_pc_jump_count++;
			/*
 * Coroutine PC jump: result encodes a target PC as
 * (pc + CNP_PC_JUMP_BASE).  Look up the stencil for
 * that PC from the compile-time array.
 */
			int pc = (int)(result - CNP_PC_JUMP_BASE);
			if (pc < 0 || pc >= p->cnp_nop ||
			    p->cnp_pc_stencil[pc] == NULL) {
				sql_cnp_error_return_count++;
				return -1;
			}
			func = (cnp_stencil_func_t)p->cnp_pc_stencil[pc];
#if SQL_VDBE_OP_PROFILE
			current_pc = pc;
#endif
		} else {
			/* Terminal status code (SQL_ROW, SQL_DONE, or -1). */
			if (result == SQL_DONE)
				sql_cnp_done_return_count++;
			else if (result < 0)
				sql_cnp_error_return_count++;
			return (int)result;
		}

		if (p->cnp_row_ready) {
			/*
 * OP_ResultRow fired the signal.  Save the next
 * stencil address and return SQL_ROW to the caller.
 */
			p->cnp_row_ready = 0;
			p->cnp_resume_func = (void *)func;
			sql_cnp_row_return_count++;
			return SQL_ROW;
		}
	}
}

void
vdbe_cnp_release(struct Vdbe *p)
{
	/*
	 * Code lives in the shared arena, so invalidation only drops metadata.
	 * The arena memory itself is reclaimed on wrap.
	 */
	cnp_invalidate_program(p);
}

int
vdbe_cnp_is_enabled(void)
{
	return 1;
}

#else /* !ENABLE_SQL_CNP */

int
vdbe_cnp_compile(struct Vdbe *p)
{
	(void)p;
	return -1;
}

int
vdbe_cnp_exec(struct Vdbe *p)
{
	(void)p;
	return -1;
}

void
vdbe_cnp_release(struct Vdbe *p)
{
	(void)p;
}

int
vdbe_cnp_is_enabled(void)
{
	return 0;
}

#endif /* ENABLE_SQL_CNP */
