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
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <elf.h>
#undef EV_NONE

#include "sqlInt.h"
#include "vdbeInt.h"
#include "vdbe.h"
#include "vdbe_cnp.h"
#include "vdbe_ops.h"
#include "mem.h"
#include "vdbe_helpers.h"
#include "box/error.h"
#include "box/field_def.h"
#include "box/tuple_format.h"
#include "box/space_cache.h"
#include "diag.h"
#include "say.h"
#include "vdbe_cnp_vdbe_view.h"
extern void
__assert_fail(const char *assertion, const char *file, unsigned int line,
	      const char *function) __attribute__((noreturn));

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
#include "generated/vdbe_cnp_fragments.h"

/*
 * R_X86_64_32 (unsigned 32-bit absolute) — not in the stencils header
 * by default because we defined only the signed variant.  Add it here.
 */
#ifndef CNP_R_X86_64_32
#define CNP_R_X86_64_32 10
#endif

enum {
	X86_MOVABS_RAX_PREFIX_0 = 0x48,
	X86_MOVABS_RAX_PREFIX_1 = 0xB8,
	X86_MOVABS_R14_PREFIX_0 = 0x49,
	X86_MOVABS_R14_PREFIX_1 = 0xBE,
	X86_CALL_REL32_OPCODE = 0xE8,
	X86_JMP_REL32_OPCODE = 0xE9,
	X86_JCC_REL32_PREFIX = 0x0F,
	X86_JCC_REL32_MIN_OPCODE = 0x80,
	X86_JCC_REL32_MAX_OPCODE = 0x8F,
	X86_JMP_RAX_OPCODE_0 = 0xFF,
	X86_JMP_RAX_OPCODE_1 = 0xE0,
	X86_POP_R64_MIN_OPCODE = 0x58,
	X86_POP_R64_MAX_OPCODE = 0x5F,
	X86_NOP_OPCODE = 0x90,
};

enum {
	X86_MOVABS_RAX_SIZE = 10,
	X86_MOVABS_R14_SIZE = 10,
	X86_JMP_REL32_SIZE = 5,
	X86_JMP_RAX_SIZE = 2,
};

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
 * Runtime live-ins for the stitched threaded-fragment pilot. The extracted
 * fragment bodies reference these via absolute relocations patched at compile
 * time. The state is process-global for now and is set up on entry to
 * vdbe_cnp_exec() before jumping into the copied fragment code.
 */
/*
 * Fragment live-ins are exported so the fragment-production object can
 * reference them directly instead of calling getter helpers in every opcode.
 */
struct Vdbe *cnp_frag_p;
VdbeOp *cnp_frag_aOp;
VdbeOp *cnp_frag_pOp;
Mem *cnp_frag_aMem;
void **cnp_frag_dispatch_table;
void *cnp_frag_error_target;
void *cnp_frag_row_target;
void *cnp_frag_done_target;

struct Vdbe *
cnp_frag_get_p(void)
{
	return cnp_frag_p;
}

VdbeOp *
cnp_frag_get_aOp(void)
{
	return cnp_frag_aOp;
}

VdbeOp *
cnp_frag_get_pOp(void)
{
	return cnp_frag_pOp;
}

Mem *
cnp_frag_get_aMem(void)
{
	return cnp_frag_aMem;
}

void
cnp_frag_set_pOp(VdbeOp *pOp)
{
	cnp_frag_pOp = pOp;
}

void **
cnp_frag_get_dispatch_table(void)
{
	return cnp_frag_dispatch_table;
}

void *
cnp_frag_get_error_target(void)
{
	return cnp_frag_error_target;
}

void *
cnp_frag_get_row_target(void)
{
	return cnp_frag_row_target;
}

void *
cnp_frag_get_done_target(void)
{
	return cnp_frag_done_target;
}

void
cnp_frag_get_p_pOp_aMem(struct Vdbe **pp, VdbeOp **ppOp, Mem **paMem)
{
	*pp    = cnp_frag_p;
	*ppOp  = cnp_frag_pOp;
	*paMem = cnp_frag_aMem;
}

/* Advance pOp by one and return the dispatch-table slot for the next op. */
void *
cnp_frag_dispatch_fallthrough(void)
{
	VdbeOp *next = cnp_frag_pOp + 1;
	cnp_frag_pOp = next;
	return cnp_frag_dispatch_table[(size_t)(next - cnp_frag_aOp)];
}

/* Jump to P2 and return the dispatch-table slot for the target op. */
void *
cnp_frag_dispatch_jump_p2(void)
{
	VdbeOp *next = &cnp_frag_aOp[cnp_frag_pOp->p2];
	cnp_frag_pOp = next;
	return cnp_frag_dispatch_table[(size_t)(next - cnp_frag_aOp)];
}

/* Return dispatch-table slot for the current pOp (initial / resume entry). */
void *
cnp_frag_get_initial_target(void)
{
	return cnp_frag_dispatch_table[(size_t)(cnp_frag_pOp - cnp_frag_aOp)];
}

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
	if (p->cnp_arith_imm != NULL) {
		free(p->cnp_arith_imm);
		p->cnp_arith_imm = NULL;
	}
	if (p->cnp_column_group != NULL) {
		free(p->cnp_column_group);
		p->cnp_column_group = NULL;
	}
	if (p->cnp_column_path != NULL) {
		free(p->cnp_column_path);
		p->cnp_column_path = NULL;
	}
	if (p->cnp_pc_stencil != NULL) {
		free(p->cnp_pc_stencil);
		p->cnp_pc_stencil = NULL;
	}
	p->cnp_code = NULL;
	p->cnp_size = 0;
	p->cnp_compiled = CNP_NOT_COMPILED;
	p->cnp_mode = CNP_MODE_STENCILS;
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
typedef int64_t (*cnp_fragment_func_t)(struct Vdbe *p, VdbeOp *aOp,
				       VdbeOp *pOp, Mem *aMem)
	__attribute__((preserve_none));

#if defined(__x86_64__)
/*
 * The copied fragment entry uses preserve_none live-ins in r12-r15, while
 * vdbe_cnp_exec() is ordinary SysV C code. Bridge the first call so the
 * stitched fragment domain sees the expected register assignment and the
 * surrounding C frame still gets its callee-saved registers back.
 */
static int64_t __attribute__((naked))
cnp_fragment_enter(void *target, struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp,
		   Mem *aMem)
{
	__asm__ volatile(
		"push %rbx\n\t"
		"push %rbp\n\t"
		"push %r12\n\t"
		"push %r13\n\t"
		"push %r14\n\t"
		"push %r15\n\t"
		"sub $8, %rsp\n\t"
		"mov %rsi, %r12\n\t"
		"mov %rdx, %r13\n\t"
		"mov %rcx, %r14\n\t"
		"mov %r8, %r15\n\t"
		"call *%rdi\n\t"
		"add $8, %rsp\n\t"
		"pop %r15\n\t"
		"pop %r14\n\t"
		"pop %r13\n\t"
		"pop %r12\n\t"
		"pop %rbp\n\t"
		"pop %rbx\n\t"
		"ret\n\t");
}
#endif

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

static int64_t __attribute__((preserve_none))
cnp_frag_terminal_row(struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp, Mem *aMem)
{
	(void)p;
	(void)aOp;
	(void)pOp;
	(void)aMem;
	return SQL_ROW;
}

static int64_t __attribute__((preserve_none))
cnp_frag_terminal_done(struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp, Mem *aMem)
{
	(void)aOp;
	(void)pOp;
	(void)aMem;
	return SQL_DONE;
}

static int64_t __attribute__((preserve_none))
cnp_frag_terminal_error(struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp, Mem *aMem)
{
	(void)p;
	(void)aOp;
	(void)pOp;
	(void)aMem;
	return -1;
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
	p->pc = (int)(pOp - p->aOp);
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
int
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
		return (uintptr_t)vdbe_op_bitand_inline;
	case OP_BitOr:
		return (uintptr_t)vdbe_op_bitor_inline;
	case OP_BitNot:
		return (uintptr_t)vdbe_op_bitnot_inline;
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

static inline size_t
cnp_abs_jmp_thunk_size(void)
{
	/* movabs $target,%rax + jmpq *%rax */
	return X86_MOVABS_RAX_SIZE + X86_JMP_RAX_SIZE;
}

static inline void
cnp_emit_movabs_rax(uint8_t *dst, uint64_t value)
{
	dst[0] = X86_MOVABS_RAX_PREFIX_0;
	dst[1] = X86_MOVABS_RAX_PREFIX_1;
	memcpy(dst + 2, &value, sizeof(value));
}

static inline void
cnp_emit_movabs_r14(uint8_t *dst, uint64_t value)
{
	dst[0] = X86_MOVABS_R14_PREFIX_0;
	dst[1] = X86_MOVABS_R14_PREFIX_1;
	memcpy(dst + 2, &value, sizeof(value));
}

static inline bool
cnp_can_emit_rel32_jump(uint8_t *jmp_insn, uintptr_t target)
{
	int64_t delta = (int64_t)target -
			(int64_t)(uintptr_t)(jmp_insn + X86_JMP_REL32_SIZE);
	return delta >= INT32_MIN && delta <= INT32_MAX;
}

static inline void
cnp_emit_jmp_rel32(uint8_t *dst, uintptr_t target)
{
	dst[0] = X86_JMP_REL32_OPCODE;
	int32_t delta = (int32_t)((int64_t)target -
				 (int64_t)(uintptr_t)(dst + X86_JMP_REL32_SIZE));
	memcpy(dst + 1, &delta, sizeof(delta));
}

static void
cnp_emit_abs_jmp_thunk(uint8_t *dst, uintptr_t target)
{
	cnp_emit_movabs_rax(dst, target);
	dst[X86_MOVABS_RAX_SIZE] = X86_JMP_RAX_OPCODE_0;
	dst[X86_MOVABS_RAX_SIZE + 1] = X86_JMP_RAX_OPCODE_1;
}

static bool
cnp_can_use_fragments(const struct Vdbe *p)
{
	for (int i = 0; i < p->nOp; i++) {
		int opcode = p->aOp[i].opcode;
		if (opcode > CNP_FRAG_MAX_OPCODE ||
		    cnp_fragments[opcode].bytes == NULL)
			return false;
	}
	return true;
}

static uintptr_t
cnp_resolve_fragment_symbol(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return 0;

	if (strcmp(name, "cnp_frag_p") == 0)
		return (uintptr_t)&cnp_frag_p;
	if (strcmp(name, "cnp_frag_aOp") == 0)
		return (uintptr_t)&cnp_frag_aOp;
	if (strcmp(name, "cnp_frag_pOp") == 0)
		return (uintptr_t)&cnp_frag_pOp;
	if (strcmp(name, "cnp_frag_aMem") == 0)
		return (uintptr_t)&cnp_frag_aMem;
	if (strcmp(name, "cnp_frag_dispatch_table") == 0)
		return (uintptr_t)&cnp_frag_dispatch_table;
	if (strcmp(name, "cnp_frag_error_target") == 0)
		return (uintptr_t)&cnp_frag_error_target;
	if (strcmp(name, "cnp_frag_row_target") == 0)
		return (uintptr_t)&cnp_frag_row_target;
	if (strcmp(name, "cnp_frag_done_target") == 0)
		return (uintptr_t)&cnp_frag_done_target;
	if (strcmp(name, "cnp_frag_get_p") == 0)
		return (uintptr_t)cnp_frag_get_p;
	if (strcmp(name, "cnp_frag_get_aOp") == 0)
		return (uintptr_t)cnp_frag_get_aOp;
	if (strcmp(name, "cnp_frag_get_pOp") == 0)
		return (uintptr_t)cnp_frag_get_pOp;
	if (strcmp(name, "cnp_frag_get_aMem") == 0)
		return (uintptr_t)cnp_frag_get_aMem;
	if (strcmp(name, "cnp_frag_get_dispatch_table") == 0)
		return (uintptr_t)cnp_frag_get_dispatch_table;
	if (strcmp(name, "cnp_frag_get_error_target") == 0)
		return (uintptr_t)cnp_frag_get_error_target;
	if (strcmp(name, "cnp_frag_get_row_target") == 0)
		return (uintptr_t)cnp_frag_get_row_target;
	if (strcmp(name, "cnp_frag_get_done_target") == 0)
		return (uintptr_t)cnp_frag_get_done_target;
	if (strcmp(name, "cnp_frag_get_p_pOp_aMem") == 0)
		return (uintptr_t)cnp_frag_get_p_pOp_aMem;
	if (strcmp(name, "cnp_frag_dispatch_fallthrough") == 0)
		return (uintptr_t)cnp_frag_dispatch_fallthrough;
	if (strcmp(name, "cnp_frag_dispatch_jump_p2") == 0)
		return (uintptr_t)cnp_frag_dispatch_jump_p2;
	if (strcmp(name, "cnp_frag_get_initial_target") == 0)
		return (uintptr_t)cnp_frag_get_initial_target;

	if (strcmp(name, "vdbe_op_integer") == 0)
		return (uintptr_t)vdbe_op_integer;
	if (strcmp(name, "vdbe_op_bool") == 0)
		return (uintptr_t)vdbe_op_bool;
	if (strcmp(name, "vdbe_op_int64") == 0)
		return (uintptr_t)vdbe_op_int64;
	if (strcmp(name, "vdbe_op_add") == 0)
		return (uintptr_t)vdbe_op_add;
	if (strcmp(name, "vdbe_op_sub") == 0)
		return (uintptr_t)vdbe_op_sub;
	if (strcmp(name, "vdbe_op_multiply") == 0)
		return (uintptr_t)vdbe_op_multiply;
	if (strcmp(name, "vdbe_op_divide") == 0)
		return (uintptr_t)vdbe_op_divide;
	if (strcmp(name, "vdbe_op_remainder") == 0)
		return (uintptr_t)vdbe_op_remainder;
	if (strcmp(name, "vdbe_op_ifnot_inline") == 0)
		return (uintptr_t)vdbe_op_ifnot_inline;
	if (strcmp(name, "vdbe_op_isnull_inline") == 0)
		return (uintptr_t)vdbe_op_isnull_inline;
	if (strcmp(name, "vdbe_op_once_inline") == 0)
		return (uintptr_t)vdbe_op_once_inline;
	if (strcmp(name, "vdbe_op_resultrow") == 0)
		return (uintptr_t)vdbe_op_resultrow;
	if (strcmp(name, "vdbe_op_bitand_inline") == 0)
		return (uintptr_t)vdbe_op_bitand_inline;
	if (strcmp(name, "vdbe_op_bitor_inline") == 0)
		return (uintptr_t)vdbe_op_bitor_inline;
	if (strcmp(name, "vdbe_op_bitnot_inline") == 0)
		return (uintptr_t)vdbe_op_bitnot_inline;
	if (strcmp(name, "vdbe_op_bitand_uint_fast") == 0)
		return (uintptr_t)vdbe_op_bitand_uint_fast;
	if (strcmp(name, "vdbe_op_bitor_uint_fast") == 0)
		return (uintptr_t)vdbe_op_bitor_uint_fast;
	if (strcmp(name, "vdbe_op_bitnot_uint_fast") == 0)
		return (uintptr_t)vdbe_op_bitnot_uint_fast;
	if (strcmp(name, "vdbe_op_bitand_p1_imm1023_fast") == 0)
		return (uintptr_t)vdbe_op_bitand_p1_imm1023_fast;
	if (strcmp(name, "vdbe_op_bitor_p1_imm255_fast") == 0)
		return (uintptr_t)vdbe_op_bitor_p1_imm255_fast;
	if (strcmp(name, "vdbe_op_shiftleft_uint_fast") == 0)
		return (uintptr_t)vdbe_op_shiftleft_uint_fast;
	if (strcmp(name, "vdbe_op_shiftright_uint_fast") == 0)
		return (uintptr_t)vdbe_op_shiftright_uint_fast;
	if (strcmp(name, "vdbe_op_shiftleft_imm1_fast") == 0)
		return (uintptr_t)vdbe_op_shiftleft_imm1_fast;
	if (strcmp(name, "vdbe_op_shiftleft_imm2_fast") == 0)
		return (uintptr_t)vdbe_op_shiftleft_imm2_fast;
	if (strcmp(name, "vdbe_op_shiftright_imm1_fast") == 0)
		return (uintptr_t)vdbe_op_shiftright_imm1_fast;
	if (strcmp(name, "vdbe_cnp_halt_handler") == 0)
		return (uintptr_t)vdbe_cnp_halt_handler;
	if (strcmp(name, "vdbe_cnp_init_handler") == 0)
		return (uintptr_t)vdbe_cnp_init_handler;
	/* agg_scan / builtin_scan fallthrough handlers */
	if (strcmp(name, "vdbe_op_null") == 0)
		return (uintptr_t)vdbe_op_null;
	if (strcmp(name, "vdbe_op_variable") == 0)
		return (uintptr_t)vdbe_op_variable;
	if (strcmp(name, "vdbe_op_copy") == 0)
		return (uintptr_t)vdbe_op_copy;
	if (strcmp(name, "vdbe_op_scopy") == 0)
		return (uintptr_t)vdbe_op_scopy;
	if (strcmp(name, "vdbe_op_addimm_inline") == 0)
		return (uintptr_t)vdbe_op_addimm_inline;
	if (strcmp(name, "vdbe_op_ttransaction_inline") == 0)
		return (uintptr_t)vdbe_op_ttransaction_inline;
	if (strcmp(name, "vdbe_op_iteratoropen") == 0)
		return (uintptr_t)vdbe_op_iteratoropen;
	if (strcmp(name, "vdbe_op_nullrow_inline") == 0)
		return (uintptr_t)vdbe_op_nullrow_inline;
	if (strcmp(name, "vdbe_op_column") == 0)
		return (uintptr_t)vdbe_op_column;
	if (strcmp(name, "vdbe_op_applytype") == 0)
		return (uintptr_t)vdbe_op_applytype;
	if (strcmp(name, "vdbe_op_openspace_inline") == 0)
		return (uintptr_t)vdbe_op_openspace_inline;
	if (strcmp(name, "vdbe_op_skipload_inline") == 0)
		return (uintptr_t)vdbe_op_skipload_inline;
	if (strcmp(name, "vdbe_op_aggstep") == 0)
		return (uintptr_t)vdbe_op_aggstep;
	if (strcmp(name, "vdbe_op_aggfinal") == 0)
		return (uintptr_t)vdbe_op_aggfinal;
	if (strcmp(name, "vdbe_op_builtinfunction") == 0)
		return (uintptr_t)vdbe_op_builtinfunction;
	if (strcmp(name, "vdbe_op_decrjumpzero_inline") == 0)
		return (uintptr_t)vdbe_op_decrjumpzero_inline;
	if (strcmp(name, "vdbe_cnp_setdiag_handler") == 0)
		return (uintptr_t)vdbe_cnp_setdiag_handler;
	if (strcmp(name, "vdbe_op_noop_inline") == 0)
		return (uintptr_t)vdbe_op_noop_inline;
	if (strcmp(name, "vdbe_op_eq") == 0)
		return (uintptr_t)vdbe_op_eq;
	if (strcmp(name, "vdbe_op_ge") == 0)
		return (uintptr_t)vdbe_op_ge;
	if (strcmp(name, "vdbe_op_mustbeint") == 0)
		return (uintptr_t)vdbe_op_mustbeint;
	if (strcmp(name, "vdbe_op_makerecord") == 0)
		return (uintptr_t)vdbe_op_makerecord;
	if (strcmp(name, "vdbe_op_initcoroutine_jit") == 0)
		return (uintptr_t)vdbe_op_initcoroutine_jit;
	if (strcmp(name, "vdbe_op_yield_jit") == 0)
		return (uintptr_t)vdbe_op_yield_jit;
	if (strcmp(name, "vdbe_op_endcoroutine_jit") == 0)
		return (uintptr_t)vdbe_op_endcoroutine_jit;
	if (strcmp(name, "vdbe_op_sorteropen") == 0)
		return (uintptr_t)vdbe_op_sorteropen;
	if (strcmp(name, "vdbe_op_openpseudo_inline") == 0)
		return (uintptr_t)vdbe_op_openpseudo_inline;
	if (strcmp(name, "vdbe_op_sorterinsert") == 0)
		return (uintptr_t)vdbe_op_sorterinsert;
	if (strcmp(name, "vdbe_op_sorterdata") == 0)
		return (uintptr_t)vdbe_op_sorterdata;
	if (strcmp(name, "vdbe_op_sortersort") == 0)
		return (uintptr_t)vdbe_op_sortersort;
	if (strcmp(name, "vdbe_op_sorternext_jit") == 0)
		return (uintptr_t)vdbe_op_sorternext_jit;
	/* agg_scan / builtin_scan JUMP_P2 cursor handlers */
	if (strcmp(name, "vdbe_op_rewind") == 0)
		return (uintptr_t)vdbe_op_rewind;
	if (strcmp(name, "vdbe_op_seekge") == 0)
		return (uintptr_t)vdbe_op_seekge;
	if (strcmp(name, "vdbe_op_seekle") == 0)
		return (uintptr_t)vdbe_op_seekle;
	if (strcmp(name, "vdbe_op_seeklt") == 0)
		return (uintptr_t)vdbe_op_seeklt;
	if (strcmp(name, "vdbe_op_seekgt") == 0)
		return (uintptr_t)vdbe_op_seekgt;
	if (strcmp(name, "vdbe_op_idx_compare") == 0)
		return (uintptr_t)vdbe_op_idx_compare;
	if (strcmp(name, "vdbe_op_next_jit") == 0)
		return (uintptr_t)vdbe_op_next_jit;
	if (strcmp(name, "vdbe_prepare_null_out") == 0)
		return (uintptr_t)vdbe_prepare_null_out;
	if (strcmp(name, "updateMaxBlobsize") == 0)
		return (uintptr_t)updateMaxBlobsize;
	if (strcmp(name, "box_error_set") == 0)
		return (uintptr_t)box_error_set;
	if (strcmp(name, "mem_set_int") == 0)
		return (uintptr_t)mem_set_int;
	if (strcmp(name, "mem_set_bool") == 0)
		return (uintptr_t)mem_set_bool;
	if (strcmp(name, "mem_set_null") == 0)
		return (uintptr_t)mem_set_null;
	if (strcmp(name, "mem_set_uint") == 0)
		return (uintptr_t)mem_set_uint;
	if (strcmp(name, "mem_to_int_precise") == 0)
		return (uintptr_t)mem_to_int_precise;
	if (strcmp(name, "mem_add") == 0)
		return (uintptr_t)mem_add;
	if (strcmp(name, "mem_sub") == 0)
		return (uintptr_t)mem_sub;
	if (strcmp(name, "mem_mul") == 0)
		return (uintptr_t)mem_mul;
	if (strcmp(name, "mem_div") == 0)
		return (uintptr_t)mem_div;
	if (strcmp(name, "mem_rem") == 0)
		return (uintptr_t)mem_rem;
	if (strcmp(name, "mem_bit_and") == 0)
		return (uintptr_t)mem_bit_and;
	if (strcmp(name, "mem_bit_or") == 0)
		return (uintptr_t)mem_bit_or;
	if (strcmp(name, "mem_bit_not") == 0)
		return (uintptr_t)mem_bit_not;
	if (strcmp(name, "mem_shift_left") == 0)
		return (uintptr_t)mem_shift_left;
	if (strcmp(name, "mem_shift_right") == 0)
		return (uintptr_t)mem_shift_right;
	if (strcmp(name, "mem_cast_implicit") == 0)
		return (uintptr_t)mem_cast_implicit;
	if (strcmp(name, "mem_cmp") == 0)
		return (uintptr_t)mem_cmp;
	if (strcmp(name, "mem_str") == 0)
		return (uintptr_t)mem_str;
	if (strcmp(name, "sqlVdbeMemTooBig") == 0)
		return (uintptr_t)sqlVdbeMemTooBig;
	if (strcmp(name, "field_type_strs") == 0)
		return (uintptr_t)field_type_strs;
	if (strcmp(name, "BuildClientError") == 0)
		return (uintptr_t)BuildClientError;
	if (strcmp(name, "diag_get") == 0)
		return (uintptr_t)diag_get;
	if (strcmp(name, "error_ref") == 0)
		return (uintptr_t)error_ref;
	if (strcmp(name, "error_unref") == 0)
		return (uintptr_t)error_unref;
	if (strcmp(name, "log_level") == 0)
		return (uintptr_t)&log_level;
	if (strcmp(name, "_say") == 0)
		return (uintptr_t)&_say;
	if (strcmp(name, "__errno_location") == 0)
		return (uintptr_t)__errno_location;
	if (strcmp(name, "__assert_fail") == 0)
		return (uintptr_t)__assert_fail;
	return 0;
}

static const Op *
cnp_find_last_opcode_before(const struct Vdbe *p, int pc, int opcode, int p1)
{
	for (int i = pc - 1; i >= 0; i--) {
		const Op *op = &p->aOp[i];
		if (op->opcode == opcode && op->p1 == p1)
			return op;
	}
	return NULL;
}

static const Op *
cnp_find_cursor_iterator(const struct Vdbe *p, int pc, int cursor_id)
{
	return cnp_find_last_opcode_before(p, pc, OP_IteratorOpen, cursor_id);
}

static struct space *
cnp_find_cursor_space(const struct Vdbe *p, int pc, int cursor_id)
{
	const Op *iter = cnp_find_cursor_iterator(p, pc, cursor_id);
	if (iter == NULL)
		return NULL;
	const Op *open = cnp_find_last_opcode_before(p, iter - p->aOp + 1,
						     OP_OpenSpace, iter->p3);
	if (open == NULL)
		return NULL;
	return space_by_id(open->p2);
}

enum {
	CNP_COLUMN_GROUP_MIN_COUNT = 3,
	CNP_COLUMN_GROUP_MAX_SPAN = 12,
	CNP_COLUMN_GROUP_MAX_SLACK = 3,
	CNP_COLUMN_PREFETCH_MIN_FOLLOWERS = 2,
};

static void
cnp_configure_column_group_metadata(struct cnp_column_group *group,
				       struct space *space,
				       uint32_t min_field, uint32_t max_field,
				       bool allow_static_offset_slot)
{
	group->enabled = true;
	group->min_field = min_field;
	group->max_field = max_field;
	group->min_offset_slot = TUPLE_OFFSET_SLOT_NIL;
	if (allow_static_offset_slot && space->format != NULL && min_field > 0 &&
	    min_field < tuple_format_field_count(space->format)) {
		struct tuple_field *field = tuple_format_field(space->format,
							       min_field);
		group->min_offset_slot = field->offset_slot;
	}
}

static void
cnp_try_configure_dense_column_group(struct Vdbe *p, int pc, struct space *space,
				     bool allow_static_offset_slot)
{
	struct cnp_column_group *group = &p->cnp_column_group[pc];
	Op *op = &p->aOp[pc];
	if (pc > 0) {
		const Op *prev = &p->aOp[pc - 1];
		if (prev->opcode == OP_Column && prev->p1 == op->p1)
			return;
	}

	uint32_t min_field = (uint32_t)op->p2;
	uint32_t max_field = (uint32_t)op->p2;
	int count = 0;
	for (int i = pc; i < p->nOp; i++) {
		const Op *it = &p->aOp[i];
		if (it->opcode != OP_Column || it->p1 != op->p1)
			break;
		if (it->p2 < 0 || (uint32_t)it->p2 >= space->def->field_count)
			return;
		uint32_t field = (uint32_t)it->p2;
		if (field < min_field)
			min_field = field;
		if (field > max_field)
			max_field = field;
		count++;
	}

	uint32_t span = max_field - min_field + 1;
	if (count < CNP_COLUMN_GROUP_MIN_COUNT || span > CNP_COLUMN_GROUP_MAX_SPAN ||
	    span > (uint32_t)(count + CNP_COLUMN_GROUP_MAX_SLACK))
		return;

	cnp_configure_column_group_metadata(group, space, min_field, max_field,
					       allow_static_offset_slot);
}

/*
 * A later OP_Column that jumps directly to a high field via offset-slot / hint
 * path does not seed the intermediate slots[] cache. If the same straight-line
 * column run later revisits smaller fields, mark the high field as a leader and
 * preload the useful envelope before decoding it.
 *
 * Example:
 *   1, 2, 11, 3, 5, 7
 *
 * Without this metadata, field 11 can be fetched as a direct jump and later
 * fields 3/5/7 re-enter the generic scan path. With the leader prefetch, the
 * field-11 site still fetches 11 directly, but also seeds the follower
 * envelope [3..7] once so the later back-edges become cache hits.
 */
static void
cnp_try_configure_column_prefetch(struct Vdbe *p, int pc, struct space *space,
				  bool allow_static_offset_slot)
{
	struct cnp_column_group *group = &p->cnp_column_group[pc];
	Op *op = &p->aOp[pc];
	if (op->p2 <= 0)
		return;

	uint32_t leader_field = (uint32_t)op->p2;
	uint32_t min_follow = leader_field;
	uint32_t max_follow = 0;
	int follower_count = 0;
	for (int i = pc + 1; i < p->nOp; i++) {
		const Op *it = &p->aOp[i];
		if (it->opcode != OP_Column || it->p1 != op->p1)
			break;
		if (it->p2 < 0 || (uint32_t)it->p2 >= space->def->field_count)
			return;
		uint32_t field = (uint32_t)it->p2;
		if (field >= leader_field)
			continue;
		if (field < min_follow)
			min_follow = field;
		if (follower_count == 0 || field > max_follow)
			max_follow = field;
		follower_count++;
	}
	if (follower_count < CNP_COLUMN_PREFETCH_MIN_FOLLOWERS)
		return;

	uint32_t span = max_follow - min_follow + 1;
	if (span > CNP_COLUMN_GROUP_MAX_SPAN ||
	    span > (uint32_t)(follower_count + CNP_COLUMN_GROUP_MAX_SLACK))
		return;

	cnp_configure_column_group_metadata(group, space, min_follow, max_follow,
					       allow_static_offset_slot);
}

static void
cnp_configure_column_group(struct Vdbe *p, int pc, struct space *space,
			   bool allow_static_offset_slot)
{
	struct cnp_column_group *group = &p->cnp_column_group[pc];
	memset(group, 0, sizeof(*group));
	cnp_try_configure_dense_column_group(p, pc, space,
						 allow_static_offset_slot);
	if (!group->enabled)
		cnp_try_configure_column_prefetch(p, pc, space,
						 allow_static_offset_slot);
}

static void
cnp_configure_column_path(struct Vdbe *p, int pc, struct space *space,
			  bool allow_static_offset_slot)
{
	struct cnp_column_path *path = &p->cnp_column_path[pc];
	memset(path, 0, sizeof(*path));

	Op *op = &p->aOp[pc];
	uint32_t fieldno = (uint32_t)op->p2;
	if (fieldno == 0 || space->format == NULL)
		return;
	if (fieldno < tuple_format_field_count(space->format)) {
		struct tuple_field *field = tuple_format_field(space->format,
							       fieldno);
		if (field != NULL && field->offset_slot != TUPLE_OFFSET_SLOT_NIL)
			return;
	}
	for (uint32_t anchor = fieldno - 1; anchor > 0; anchor--) {
		if (anchor >= tuple_format_field_count(space->format))
			continue;
		struct tuple_field *field = tuple_format_field(space->format,
							       anchor);
		if (field == NULL || field->offset_slot == TUPLE_OFFSET_SLOT_NIL)
			continue;
		path->enabled = true;
		path->anchor_field = anchor;
		path->hop_count = fieldno - anchor;
		path->offset_slot = allow_static_offset_slot ?
			field->offset_slot : TUPLE_OFFSET_SLOT_NIL;
		return;
	}
}

/*
 * Pick the CnP helper for one OP_Column site and populate any per-PC metadata
 * that the helper will need at runtime.
 *
 * The selection policy is:
 * 1. Bail out to generic vdbe_op_column() if we cannot recover the cursor's
 *    iterator or space, or if the field number is out of range.
 * 2. Precompute row-local metadata shared by the fast helpers:
 *    - cnp_column_group[pc] for contiguous OP_Column runs on one cursor;
 *    - cnp_column_path[pc] for "jump to hinted anchor, then hop right" paths.
 * 3. Decide whether this site should use the offset-slot family or the exact
 *    family of typed helpers.
 *
 * The offset-slot family covers two cases:
 * - primary-space scans where we can encode this field's offset slot directly
 *   into OP.p5;
 * - covering / secondary-index scans where the offset slot must be resolved
 *   from the runtime tuple format, but the helper family is still the same.
 *
 * If neither direct offset-slot access nor an anchored hint path is available,
 * use the exact typed helper instead.
 */
static uintptr_t
cnp_select_column_handler(struct Vdbe *p, int pc)
{
	Op *op = &p->aOp[pc];
	const Op *iter = cnp_find_cursor_iterator(p, pc, op->p1);
	struct space *space = cnp_find_cursor_space(p, pc, op->p1);
	op->p5 &= ~OPFLAG_CNP_COLUMN_OFFSET_SLOT_MASK;
	memset(&p->cnp_column_group[pc], 0, sizeof(p->cnp_column_group[pc]));
	memset(&p->cnp_column_path[pc], 0, sizeof(p->cnp_column_path[pc]));
	if (space == NULL || iter == NULL)
		return (uintptr_t)vdbe_op_column;
	if ((uint32_t)op->p2 >= space->def->field_count)
		return (uintptr_t)vdbe_op_column;
	bool allow_static_offset_slot = iter->p2 == 0;
	cnp_configure_column_group(p, pc, space, allow_static_offset_slot);
	cnp_configure_column_path(p, pc, space, allow_static_offset_slot);
	/*
	 * "offset-slot helper" means "use the family that can navigate from an
	 * offset slot or anchored hint path", not necessarily "the slot number is
	 * statically embedded in OP.p5". Covering scans also land here, but they
	 * resolve the slot from the runtime tuple format.
	 */
	bool use_offset_slot_helper = false;
	int32_t offset_slot = TUPLE_OFFSET_SLOT_NIL;
	if (!allow_static_offset_slot) {
		/* Secondary / covering scan: runtime tuple format decides the slot. */
		use_offset_slot_helper = true;
	} else if (space->format != NULL &&
	    (uint32_t)op->p2 < tuple_format_field_count(space->format) &&
	    op->p2 > 0) {
		struct tuple_field *field = tuple_format_field(space->format,
							       op->p2);
		offset_slot = field->offset_slot;
		if (offset_slot != TUPLE_OFFSET_SLOT_NIL &&
		    -offset_slot < (1 << (16 - OPFLAG_CNP_COLUMN_OFFSET_SLOT_SHIFT))) {
			/* Primary scan: stash the static slot directly in OP.p5. */
			op->p5 |= (uint16_t)(-offset_slot)
				<< OPFLAG_CNP_COLUMN_OFFSET_SLOT_SHIFT;
			use_offset_slot_helper = true;
		}
	}
	/* No direct slot, but we can still reach the field via anchor + hops. */
	if (!use_offset_slot_helper && p->cnp_column_path[pc].enabled)
		use_offset_slot_helper = true;
	switch (space->def->fields[op->p2].type) {
	case FIELD_TYPE_UNSIGNED:
		return (uintptr_t)(use_offset_slot_helper ?
			vdbe_op_column_unsigned_offset_slot_fast :
			vdbe_op_column_unsigned_exact_fast);
	case FIELD_TYPE_STRING:
		return (uintptr_t)(use_offset_slot_helper ?
			vdbe_op_column_string_offset_slot_fast :
			vdbe_op_column_string_exact_fast);
	case FIELD_TYPE_DOUBLE:
		return (uintptr_t)(use_offset_slot_helper ?
			vdbe_op_column_double_offset_slot_fast :
			vdbe_op_column_double_exact_fast);
	case FIELD_TYPE_INTEGER:
		return (uintptr_t)(use_offset_slot_helper ?
			vdbe_op_column_integer_offset_slot_fast :
			vdbe_op_column_integer_exact_fast);
	case FIELD_TYPE_BOOLEAN:
		return (uintptr_t)(use_offset_slot_helper ?
			vdbe_op_column_boolean_offset_slot_fast :
			vdbe_op_column_boolean_exact_fast);
	default:
		return (uintptr_t)vdbe_op_column;
	}
}

static uintptr_t
cnp_select_applytype_cast_helper(const struct Vdbe *p, int pc)
{
	const Op *op = &p->aOp[pc];
	if (op->opcode != OP_ApplyType || op->p2 != 1 || op->p4.types == NULL)
		return (uintptr_t)mem_cast_implicit;
	switch (op->p4.types[0]) {
	case FIELD_TYPE_UNSIGNED:
		return (uintptr_t)mem_cast_implicit_unsigned_fast;
	case FIELD_TYPE_STRING:
		return (uintptr_t)mem_cast_implicit_string_fast;
	case FIELD_TYPE_DOUBLE:
		return (uintptr_t)mem_cast_implicit_double_fast;
	case FIELD_TYPE_INTEGER:
		return (uintptr_t)mem_cast_implicit_integer_fast;
	case FIELD_TYPE_BOOLEAN:
		return (uintptr_t)mem_cast_implicit_boolean_fast;
	case FIELD_TYPE_NUMBER:
		return (uintptr_t)mem_cast_implicit_number_fast;
	default:
		return (uintptr_t)mem_cast_implicit;
	}
}

static bool
cnp_fragment_has_local_relocs(const struct cnp_fragment *frag)
{
	for (uint32_t i = 0; i < frag->num_relocs; i++) {
		const struct cnp_fragment_reloc *rel = &frag->relocs[i];
		if (rel->symbol_name == NULL || rel->symbol_name[0] == '\0')
			return true;
	}
	return false;
}

static const Op *
cnp_find_last_reg_writer(const struct Vdbe *p, int pc, int reg)
{
	for (int i = pc - 1; i >= 0; i--) {
		const Op *op = &p->aOp[i];
		switch (op->opcode) {
		case OP_Integer:
		case OP_Bool:
		case OP_Int64:
		case OP_Real:
		case OP_String:
		case OP_Null:
		case OP_Variable:
		case OP_Copy:
		case OP_SCopy:
		case OP_BitNot:
			if (op->p2 == reg)
				return op;
			break;
		case OP_Column:
		case OP_Add:
		case OP_Subtract:
		case OP_Multiply:
		case OP_Divide:
		case OP_Remainder:
		case OP_BitAnd:
		case OP_BitOr:
		case OP_ShiftLeft:
		case OP_ShiftRight:
			if (op->p3 == reg)
				return op;
			break;
		default:
			break;
		}
	}
	return NULL;
}

static const Op *
cnp_find_unique_reg_writer(const struct Vdbe *p, int reg)
{
	const Op *writer = NULL;
	for (int i = 0; i < p->nOp; i++) {
		const Op *op = &p->aOp[i];
		bool writes_reg = false;
		switch (op->opcode) {
		case OP_Integer:
		case OP_Bool:
		case OP_Int64:
		case OP_Real:
		case OP_String:
		case OP_Null:
		case OP_Variable:
		case OP_Copy:
		case OP_SCopy:
		case OP_BitNot:
			writes_reg = op->p2 == reg;
			break;
		case OP_Column:
		case OP_Add:
		case OP_Subtract:
		case OP_Multiply:
		case OP_Divide:
		case OP_Remainder:
		case OP_BitAnd:
		case OP_BitOr:
		case OP_ShiftLeft:
		case OP_ShiftRight:
			writes_reg = op->p3 == reg;
			break;
		default:
			break;
		}
		if (!writes_reg)
			continue;
		if (writer != NULL)
			return NULL;
		writer = op;
	}
	return writer;
}

static bool
cnp_reg_is_likely_uint(const struct Vdbe *p, int pc, int reg, int depth);

static bool
cnp_op_result_is_likely_uint(const struct Vdbe *p, int pc, const Op *op,
			       int depth)
{
	if (depth <= 0)
		return false;
	switch (op->opcode) {
	case OP_Integer:
		return op->p1 >= 0;
	case OP_Bool:
		return op->p1 >= 0;
	case OP_Copy:
	case OP_SCopy:
		return cnp_reg_is_likely_uint(p, pc, op->p1, depth - 1);
	case OP_Column: {
		struct space *space = cnp_find_cursor_space(p, pc, op->p1);
		if (space == NULL || (uint32_t)op->p2 >= space->def->field_count)
			return false;
		enum field_type type = space->def->fields[op->p2].type;
		return type == FIELD_TYPE_UNSIGNED || type == FIELD_TYPE_INTEGER;
	}
	case OP_BitAnd:
	case OP_BitOr:
	case OP_ShiftLeft:
	case OP_ShiftRight:
		return cnp_reg_is_likely_uint(p, pc, op->p1, depth - 1) &&
		       cnp_reg_is_likely_uint(p, pc, op->p2, depth - 1);
	case OP_BitNot:
		return cnp_reg_is_likely_uint(p, pc, op->p1, depth - 1);
	default:
		return false;
	}
}

static bool
cnp_reg_is_likely_int(const struct Vdbe *p, int pc, int reg, int depth);

static bool
cnp_op_result_is_likely_int(const struct Vdbe *p, int pc, const Op *op,
			    int depth)
{
	if (depth <= 0)
		return false;
	switch (op->opcode) {
	case OP_Integer:
	case OP_Bool:
	case OP_Int64:
		return true;
	case OP_Copy:
	case OP_SCopy:
		return cnp_reg_is_likely_int(p, pc, op->p1, depth - 1);
	case OP_Column: {
		struct space *space = cnp_find_cursor_space(p, pc, op->p1);
		if (space == NULL || (uint32_t)op->p2 >= space->def->field_count)
			return false;
		enum field_type type = space->def->fields[op->p2].type;
		return type == FIELD_TYPE_UNSIGNED || type == FIELD_TYPE_INTEGER;
	}
	case OP_Add:
	case OP_Subtract:
	case OP_Multiply:
	case OP_Divide:
	case OP_Remainder:
		return cnp_reg_is_likely_int(p, pc, op->p1, depth - 1) &&
		       cnp_reg_is_likely_int(p, pc, op->p2, depth - 1);
	case OP_BitAnd:
	case OP_BitOr:
	case OP_BitNot:
	case OP_ShiftLeft:
	case OP_ShiftRight:
		return cnp_op_result_is_likely_uint(p, pc, op, depth - 1);
	default:
		return false;
	}
}

static bool
cnp_reg_is_likely_uint(const struct Vdbe *p, int pc, int reg, int depth)
{
	if (depth <= 0)
		return false;
	const Op *def = cnp_find_last_reg_writer(p, pc, reg);
	if (def == NULL)
		return false;
	return cnp_op_result_is_likely_uint(p, pc, def, depth);
}

static bool
cnp_reg_is_likely_int(const struct Vdbe *p, int pc, int reg, int depth)
{
	if (depth <= 0)
		return false;
	const Op *def = cnp_find_last_reg_writer(p, pc, reg);
	if (def == NULL)
		return false;
	return cnp_op_result_is_likely_int(p, pc, def, depth);
}

static bool
cnp_resolve_uint_constant(const struct Vdbe *p, int pc, int reg,
			      uint64_t *value, int depth)
{
	if (depth <= 0)
		return false;
	const Op *def = cnp_find_last_reg_writer(p, pc, reg);
	if (def == NULL)
		return false;
	switch (def->opcode) {
	case OP_Integer:
		if (def->p1 < 0)
			return false;
		*value = (uint64_t)def->p1;
		return true;
	case OP_Bool:
		*value = (uint64_t)def->p1;
		return true;
	case OP_Copy:
	case OP_SCopy:
		return cnp_resolve_uint_constant(p, pc, def->p1, value,
						 depth - 1);
	default:
		return false;
	}
}

static bool
cnp_resolve_int_constant(const struct Vdbe *p, int pc, int reg, int64_t *value,
			 bool *is_signed, int depth)
{
	if (depth <= 0)
		return false;
	const Op *def = cnp_find_last_reg_writer(p, pc, reg);
	if (def == NULL)
		return false;
	switch (def->opcode) {
	case OP_Integer:
		*value = def->p1;
		*is_signed = def->p1 < 0;
		return true;
	case OP_Int64:
		if (def->p4.pI64 == NULL)
			return false;
		*value = *def->p4.pI64;
		*is_signed = def->p4type == P4_INT64;
		return true;
	case OP_Copy:
	case OP_SCopy:
		return cnp_resolve_int_constant(p, pc, def->p1, value,
						is_signed, depth - 1);
	default:
		return false;
	}
}

static bool
cnp_configure_arith_imm(struct Vdbe *p, int pc)
{
	const Op *op = &p->aOp[pc];
	struct cnp_arith_imm *imm = &p->cnp_arith_imm[pc];
	memset(imm, 0, sizeof(*imm));
	if (cnp_resolve_int_constant(p, pc, op->p1, &imm->p1_value,
				     &imm->p1_is_signed, 8))
		imm->mask |= CNP_ARITH_IMM_P1;
	if (cnp_resolve_int_constant(p, pc, op->p2, &imm->p2_value,
				     &imm->p2_is_signed, 8))
		imm->mask |= CNP_ARITH_IMM_P2;
	return imm->mask != 0;
}

static uintptr_t
cnp_select_arith_handler(struct Vdbe *p, int pc)
{
	const Op *op = &p->aOp[pc];
	bool p1_const = cnp_configure_arith_imm(p, pc) &&
			(p->cnp_arith_imm[pc].mask & CNP_ARITH_IMM_P1) != 0;
	bool p2_const = (p->cnp_arith_imm[pc].mask & CNP_ARITH_IMM_P2) != 0;
	bool p1_int = p1_const || cnp_reg_is_likely_int(p, pc, op->p1, 16);
	bool p2_int = p2_const || cnp_reg_is_likely_int(p, pc, op->p2, 16);
	if ((p1_const || p2_const) && p1_int && p2_int) {
		switch (op->opcode) {
		case OP_Add:
			return (uintptr_t)vdbe_op_add_const_fast;
		case OP_Subtract:
			return (uintptr_t)vdbe_op_sub_const_fast;
		case OP_Multiply:
			return (uintptr_t)vdbe_op_multiply_const_fast;
		case OP_Divide:
			return (uintptr_t)vdbe_op_divide_const_fast;
		case OP_Remainder:
			return (uintptr_t)vdbe_op_remainder_const_fast;
		default:
			break;
		}
	}
	if (!p1_int || !p2_int)
		return cnp_resolve_handler_by_opcode(op->opcode);
	switch (op->opcode) {
	case OP_Add:
		return (uintptr_t)vdbe_op_add_int_fast;
	case OP_Subtract:
		return (uintptr_t)vdbe_op_sub_int_fast;
	case OP_Multiply:
		return (uintptr_t)vdbe_op_multiply_int_fast;
	case OP_Divide:
		return (uintptr_t)vdbe_op_divide_int_fast;
	case OP_Remainder:
		return (uintptr_t)vdbe_op_remainder_int_fast;
	default:
		return cnp_resolve_handler_by_opcode(op->opcode);
	}
}

static uintptr_t
cnp_select_arith_fragment_handler(struct Vdbe *p, int pc)
{
	const Op *op = &p->aOp[pc];
	bool p1_const = cnp_configure_arith_imm(p, pc) &&
			(p->cnp_arith_imm[pc].mask & CNP_ARITH_IMM_P1) != 0;
	bool p2_const = (p->cnp_arith_imm[pc].mask & CNP_ARITH_IMM_P2) != 0;
	bool p1_int = p1_const || cnp_reg_is_likely_int(p, pc, op->p1, 16);
	bool p2_int = p2_const || cnp_reg_is_likely_int(p, pc, op->p2, 16);
	if ((p1_const || p2_const) && p1_int && p2_int) {
		switch (op->opcode) {
		case OP_Add:
			return (uintptr_t)vdbe_op_add_const_fast;
		case OP_Subtract:
			return (uintptr_t)vdbe_op_sub_const_fast;
		case OP_Multiply:
			return (uintptr_t)vdbe_op_multiply_const_fast;
		case OP_Divide:
			return (uintptr_t)vdbe_op_divide_const_fast;
		case OP_Remainder:
			return (uintptr_t)vdbe_op_remainder_const_fast;
		default:
			break;
		}
	}
	if (!p1_int || !p2_int) {
		switch (op->opcode) {
		case OP_Add:
			return (uintptr_t)vdbe_op_add_sysv_bridge;
		case OP_Subtract:
			return (uintptr_t)vdbe_op_sub_sysv_bridge;
		case OP_Multiply:
			return (uintptr_t)vdbe_op_multiply_sysv_bridge;
		case OP_Divide:
			return (uintptr_t)vdbe_op_divide_sysv_bridge;
		case OP_Remainder:
			return (uintptr_t)vdbe_op_remainder_sysv_bridge;
		default:
			return cnp_resolve_handler_by_opcode(op->opcode);
		}
	}
	switch (op->opcode) {
	case OP_Add:
		return (uintptr_t)vdbe_op_add_int_fast;
	case OP_Subtract:
		return (uintptr_t)vdbe_op_sub_int_fast;
	case OP_Multiply:
		return (uintptr_t)vdbe_op_multiply_int_fast;
	case OP_Divide:
		return (uintptr_t)vdbe_op_divide_int_fast;
	case OP_Remainder:
		return (uintptr_t)vdbe_op_remainder_int_fast;
	default:
		return cnp_resolve_handler_by_opcode(op->opcode);
	}
}

static uintptr_t
cnp_select_bitwise_handler(const struct Vdbe *p, int pc)
{
	const Op *op = &p->aOp[pc];
	uint64_t imm;
	switch (op->opcode) {
	case OP_BitAnd:
		if (cnp_resolve_uint_constant(p, pc, op->p1, &imm, 8) &&
		    imm == 1023)
			return (uintptr_t)vdbe_op_bitand_p1_imm1023_fast;
		if (cnp_reg_is_likely_uint(p, pc, op->p1, 16) &&
		    cnp_reg_is_likely_uint(p, pc, op->p2, 16))
			return (uintptr_t)vdbe_op_bitand_uint_fast;
		return (uintptr_t)vdbe_op_bitand_inline;
	case OP_BitOr:
		if (cnp_resolve_uint_constant(p, pc, op->p1, &imm, 8) &&
		    imm == 255)
			return (uintptr_t)vdbe_op_bitor_p1_imm255_fast;
		if (cnp_reg_is_likely_uint(p, pc, op->p1, 16) &&
		    cnp_reg_is_likely_uint(p, pc, op->p2, 16))
			return (uintptr_t)vdbe_op_bitor_uint_fast;
		return (uintptr_t)vdbe_op_bitor_inline;
	case OP_BitNot:
		if (cnp_reg_is_likely_uint(p, pc, op->p1, 16))
			return (uintptr_t)vdbe_op_bitnot_uint_fast;
		return (uintptr_t)vdbe_op_bitnot_inline;
	case OP_ShiftLeft:
		if (cnp_resolve_uint_constant(p, pc, op->p1, &imm, 8)) {
			if (imm == 1)
				return (uintptr_t)vdbe_op_shiftleft_imm1_fast;
			if (imm == 2)
				return (uintptr_t)vdbe_op_shiftleft_imm2_fast;
		}
		if (cnp_reg_is_likely_uint(p, pc, op->p1, 16) &&
		    cnp_reg_is_likely_uint(p, pc, op->p2, 16))
			return (uintptr_t)vdbe_op_shiftleft_uint_fast;
		return (uintptr_t)vdbe_op_shiftleft_inline;
	case OP_ShiftRight:
		if (cnp_resolve_uint_constant(p, pc, op->p1, &imm, 8) &&
		    imm == 1)
			return (uintptr_t)vdbe_op_shiftright_imm1_fast;
		if (cnp_reg_is_likely_uint(p, pc, op->p1, 16) &&
		    cnp_reg_is_likely_uint(p, pc, op->p2, 16))
			return (uintptr_t)vdbe_op_shiftright_uint_fast;
		return (uintptr_t)vdbe_op_shiftright_inline;
	default:
		return cnp_resolve_handler_by_opcode(op->opcode);
	}
}

static bool
cnp_fragment_find_hot_jmp(const struct cnp_fragment *frag, uint32_t *jmp_offset)
{
	for (uint32_t i = frag->transfer_offset; i + 1 < frag->size; i++) {
		if (frag->bytes[i] == X86_JMP_RAX_OPCODE_0 &&
		    frag->bytes[i + 1] == X86_JMP_RAX_OPCODE_1) {
			*jmp_offset = i;
			return true;
		}
	}
	return false;
}

static bool
cnp_fragment_can_compact_fallthrough(const struct cnp_fragment *frag, int pc,
				       int nOp, uint32_t *jmp_offset)
{
	/*
	 * Byte-compacting the middle dispatch block is only safe once we also
	 * remap internal control-flow targets that jump into the preserved cold
	 * tail. Scan fragments like OP_OpenSpace use a short js into the error
	 * block after the hot dispatch tail, so dropping the middle bytes without
	 * rewriting that branch corrupts the success/error split. Keep the
	 * original layout for now and patch the dispatch block in place.
	 */
	(void)frag;
	(void)pc;
	(void)nOp;
	(void)jmp_offset;
	return false;
}

static size_t
cnp_fragment_copy_size(const struct cnp_fragment *frag, int pc, int nOp)
{
	uint32_t jmp_offset;
	if (cnp_fragment_can_compact_fallthrough(frag, pc, nOp, &jmp_offset))
		return frag->dispatch_offset +
		       (jmp_offset - frag->transfer_offset) +
		       (frag->size - (jmp_offset + X86_JMP_RAX_SIZE));
	return frag->size;
}

static bool
cnp_fragment_map_offset(const struct cnp_fragment *frag, int pc, int nOp,
			      uint32_t orig_offset, uint32_t *mapped_offset)
{
	uint32_t jmp_offset;
	size_t fused_size = cnp_fragment_copy_size(frag, pc, nOp);
	if (fused_size == frag->size) {
		if (orig_offset >= frag->size)
			return false;
		*mapped_offset = orig_offset;
		return true;
	}

	if (!cnp_fragment_can_compact_fallthrough(frag, pc, nOp, &jmp_offset))
		return false;
	if (orig_offset < frag->dispatch_offset) {
		*mapped_offset = orig_offset;
		return true;
	}
	if (orig_offset >= frag->transfer_offset && orig_offset < jmp_offset) {
		*mapped_offset = frag->dispatch_offset +
				(orig_offset - frag->transfer_offset);
		return true;
	}
	if (orig_offset >= jmp_offset + X86_JMP_RAX_SIZE &&
	    orig_offset < frag->size) {
		*mapped_offset = frag->dispatch_offset +
				(jmp_offset - frag->transfer_offset) +
				(orig_offset - (jmp_offset + X86_JMP_RAX_SIZE));
		return true;
	}
	return false;
}

static void
cnp_fragment_copy_bytes(uint8_t *dst, const struct cnp_fragment *frag, int pc,
			int nOp)
{
	uint32_t jmp_offset;
	size_t copy_size = cnp_fragment_copy_size(frag, pc, nOp);
	if (copy_size == frag->size) {
		memcpy(dst, frag->bytes, frag->size);
		return;
	}

	if (!cnp_fragment_can_compact_fallthrough(frag, pc, nOp, &jmp_offset)) {
		memcpy(dst, frag->bytes, frag->size);
		return;
	}
	memcpy(dst, frag->bytes, frag->dispatch_offset);
	size_t pos = frag->dispatch_offset;
	size_t hot_tail_size = jmp_offset - frag->transfer_offset;
	memcpy(dst + pos, frag->bytes + frag->transfer_offset, hot_tail_size);
	pos += hot_tail_size;
	memcpy(dst + pos, frag->bytes + jmp_offset + X86_JMP_RAX_SIZE,
	       frag->size - (jmp_offset + X86_JMP_RAX_SIZE));
}

static bool
cnp_reloc_needs_thunk(const uint8_t *code, size_t size, uint32_t offset,
		       uint8_t reloc_type)
{
	if (offset >= size)
		return false;
	if (reloc_type != CNP_R_X86_64_PC32 &&
	    reloc_type != CNP_R_X86_64_PLT32)
		return false;
	/*
	 * Thunks are only needed for control-transfer rel32 relocations
	 * (call/jump). Data PC-relative loads such as
	 * `mov cnp_frag_dispatch_table(%rip), %rax` must keep addressing the
	 * data symbol directly instead of a code thunk.
	 */
	if (offset >= 1 && code[offset - 1] == X86_CALL_REL32_OPCODE)
		return true;
	if (offset >= 1 && code[offset - 1] == X86_JMP_REL32_OPCODE)
		return true;
	if (offset >= 2 && code[offset - 2] == X86_JCC_REL32_PREFIX &&
	    code[offset - 1] >= X86_JCC_REL32_MIN_OPCODE &&
	    code[offset - 1] <= X86_JCC_REL32_MAX_OPCODE)
		return true;
	return false;
}

static bool
cnp_fragment_find_dispatch_block(const struct cnp_fragment *frag, int dispatch_idx,
				 uint32_t *block_start, uint32_t *block_end,
				 uint8_t *pop_opcode)
{
	int dispatch_seen = 0;
	for (uint32_t i = 0; i < frag->num_relocs; i++) {
		const struct cnp_fragment_reloc *rel = &frag->relocs[i];
		if (strcmp(rel->symbol_name, "cnp_frag_dispatch_table") != 0)
			continue;
		if (dispatch_seen++ != dispatch_idx)
			continue;
		if (rel->offset < 2)
			return false;
		uint32_t start = rel->offset - 2;
		if (start + 2 > frag->size)
			return false;
		uint8_t rex = frag->bytes[start];
		uint8_t movabs = frag->bytes[start + 1];
		if ((rex != 0x48 && rex != 0x49) ||
		    movabs < 0xB8 || movabs > 0xBF)
			return false;
		for (uint32_t j = rel->offset + 8; j + 2 < frag->size; j++) {
			if (frag->bytes[j] >= X86_POP_R64_MIN_OPCODE &&
			    frag->bytes[j] <= X86_POP_R64_MAX_OPCODE &&
			    frag->bytes[j + 1] == X86_JMP_RAX_OPCODE_0 &&
			    frag->bytes[j + 2] == X86_JMP_RAX_OPCODE_1) {
				*block_start = start;
				*block_end = j + 3;
				*pop_opcode = frag->bytes[j];
				return true;
			}
		}
		return false;
	}
	return false;
}

static bool
cnp_fragment_patch_direct_jump(uint8_t *frag_code, uint32_t block_start,
			       uint32_t block_end, uint8_t pop_opcode,
			       uintptr_t next_pOp, uintptr_t target)
{
	uint32_t block_len = block_end - block_start;
	memset(frag_code + block_start, X86_NOP_OPCODE, block_len);

	uint32_t pos = block_start;
	if (next_pOp != 0) {
		if (block_len < X86_MOVABS_R14_SIZE + 1 + X86_JMP_REL32_SIZE &&
		    block_len < X86_MOVABS_R14_SIZE + X86_MOVABS_RAX_SIZE + 1 +
				X86_JMP_RAX_SIZE)
			return false;
		cnp_emit_movabs_r14(frag_code + pos, next_pOp);
		pos += X86_MOVABS_R14_SIZE;
	}

	if (pos + 1 + X86_JMP_REL32_SIZE <= block_start + block_len &&
	    cnp_can_emit_rel32_jump(frag_code + pos + 1, target)) {
		frag_code[pos] = pop_opcode;
		cnp_emit_jmp_rel32(frag_code + pos + 1, target);
		return true;
	}

	if (pos + X86_MOVABS_RAX_SIZE + 1 + X86_JMP_RAX_SIZE >
	    block_start + block_len)
		return false;
	cnp_emit_movabs_rax(frag_code + pos, target);
	pos += X86_MOVABS_RAX_SIZE;
	frag_code[pos++] = pop_opcode;
	frag_code[pos++] = X86_JMP_RAX_OPCODE_0;
	frag_code[pos++] = X86_JMP_RAX_OPCODE_1;
	return true;
}

static bool
cnp_fragment_opcode_allows_jump_p2_patch(const struct Vdbe *p, int pc)
{
	switch (p->aOp[pc].opcode) {
	case OP_Rewind:
	case OP_SeekGE:
	case OP_SeekLE:
	case OP_SeekLT:
	case OP_SeekGT:
	case OP_IdxLE:
	case OP_IdxGT:
	case OP_IdxGE:
	case OP_IdxLT:
	case OP_Next:
		return true;
	default:
		return false;
	}
}

static void
cnp_fragment_patch_jump_p2_transfers(uint8_t *frag_code,
				       const struct cnp_fragment *frag,
				       const struct Vdbe *p,
				       void **pc_stencil, int pc)
{
	if (frag->kind != CNP_FRAG_JUMP_P2 || !frag->tail_fallthrough)
		return;
	uint32_t jump_start, jump_end, fall_start, fall_end;
	uint8_t jump_pop, fall_pop;
	if (!cnp_fragment_opcode_allows_jump_p2_patch(p, pc))
		return;
	if (!cnp_fragment_find_dispatch_block(frag, 0, &jump_start, &jump_end,
					      &jump_pop) ||
	    !cnp_fragment_find_dispatch_block(frag, 1, &fall_start, &fall_end,
					      &fall_pop))
		return;
	int jump_pc = p->aOp[pc].p2;
	void *jump_target = (jump_pc >= 0 && jump_pc < p->nOp) ?
			    pc_stencil[jump_pc] :
			    pc_stencil[p->nOp + 1];
	void *fall_target = (pc + 1 < p->nOp) ? pc_stencil[pc + 1] :
			   pc_stencil[p->nOp];
	if (jump_target == NULL || fall_target == NULL)
		return;
	uintptr_t jump_pOp = (jump_pc >= 0 && jump_pc <= p->nOp) ?
			     (uintptr_t)&p->aOp[jump_pc] : 0;
	uintptr_t fall_pOp = (uintptr_t)&p->aOp[(pc + 1 <= p->nOp) ? pc + 1 :
						      p->nOp];
	if (!cnp_fragment_patch_direct_jump(frag_code, jump_start, jump_end,
					    jump_pop, jump_pOp,
					    (uintptr_t)jump_target))
		return;
	(void)cnp_fragment_patch_direct_jump(frag_code, fall_start, fall_end,
					     fall_pop, fall_pOp,
					     (uintptr_t)fall_target);
}

static bool
cnp_fragment_opcode_allows_fallthrough_patch(const struct Vdbe *p, int pc)
{
	switch (p->aOp[pc].opcode) {
	case OP_Integer:
	case OP_Variable:
	case OP_BitAnd:
	case OP_BitOr:
	case OP_BitNot:
	case OP_ShiftLeft:
	case OP_ShiftRight:
		return true;
	default:
		return false;
	}
}

static void
cnp_fragment_patch_fallthrough_transfer(uint8_t *frag_code,
				       const struct cnp_fragment *frag,
				       const struct Vdbe *p, void **pc_stencil,
				       int pc)
{
	/*
	 * Only patch the tiny straight-line fragments whose dispatch block is a
	 * normal tail transfer with no hidden ABI restore work in it. Wider
	 * fallthrough patching stays disabled for now because fragments such as
	 * ApplyType still place live-register restore code inside the metadata
	 * dispatch block.
	 */
	if (!frag->tail_fallthrough ||
	    !cnp_fragment_opcode_allows_fallthrough_patch(p, pc))
		return;
	uint32_t fall_start, fall_end;
	uint8_t fall_pop;
	if (!cnp_fragment_find_dispatch_block(frag, 0, &fall_start, &fall_end,
					      &fall_pop))
		return;
	void *fall_target = (pc + 1 < p->nOp) ? pc_stencil[pc + 1] :
			   pc_stencil[p->nOp];
	if (fall_target == NULL)
		return;
	(void)cnp_fragment_patch_direct_jump(frag_code, fall_start, fall_end,
					     fall_pop,
					     (uintptr_t)&p->aOp[(pc + 1 <= p->nOp) ?
							      pc + 1 : p->nOp],
					     (uintptr_t)fall_target);
}

/*
 * Fragment entry dispatcher emitted after the shared prologue.
 *
 * The extracted opcode bodies use a fixed ABI derived from the fragment
 * object file:
 *   r12 = magic reciprocal for division by sizeof(Op) (24 bytes)
 *   r13 = aOp
 *   r14 = dispatch table
 *   r15 = error target
 *   [rbp-0x40] = p
 *   [rbp-0x38] = aMem
 *   [rbp-0x30] = pOp
 *   [rbp-0x48] = row target
 *   [rbp-0x58] = done target
 *
 * Populate those live-ins once per native entry and then jump directly to the
 * current opcode fragment selected by pOp.
 */

static int
vdbe_cnp_compile_fragments(struct Vdbe *p)
{
	int nOp = p->nOp;
	Op *aOp = p->aOp;
	size_t total_size = 0;
	size_t thunk_count = 0;

	for (int i = 0; i < nOp; i++) {
		const struct cnp_fragment *frag = &cnp_fragments[aOp[i].opcode];
		size_t copy_size = cnp_fragment_copy_size(frag, i, nOp);
		total_size += copy_size;
		for (uint32_t r = 0; r < frag->num_relocs; r++) {
			uint32_t mapped_offset;
			if (!cnp_fragment_map_offset(frag, i, nOp,
						       frag->relocs[r].offset,
						       &mapped_offset))
				continue;
			if (cnp_reloc_needs_thunk(frag->bytes, frag->size,
						  frag->relocs[r].offset,
						  frag->relocs[r].reloc_type))
				thunk_count++;
		}
	}
	total_size += thunk_count * cnp_abs_jmp_thunk_size();

	uint8_t *code = cnp_arena_alloc(total_size);
	if (code == NULL) {
		sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
			"fragment compile: failed to allocate %zu bytes of code",
			total_size);
		return -1;
	}

	void **pc_stencil = (void **)calloc((size_t)nOp + 3, sizeof(void *));
	if (pc_stencil == NULL) {
		sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
			"fragment compile: failed to allocate pc_stencil for %d ops",
			nOp);
		return -1;
	}

	size_t pos = 0;
	for (int i = 0; i < nOp; i++) {
		const struct cnp_fragment *frag = &cnp_fragments[aOp[i].opcode];
		size_t copy_size = cnp_fragment_copy_size(frag, i, nOp);
		pc_stencil[i] = code + pos;
		cnp_fragment_copy_bytes(code + pos, frag, i, nOp);
		pos += copy_size;
	}
	pc_stencil[nOp] = (void *)cnp_frag_terminal_done;
	pc_stencil[nOp + 1] = (void *)cnp_frag_terminal_row;
	pc_stencil[nOp + 2] = (void *)cnp_frag_terminal_error;
	size_t thunk_pos = pos;

	for (int i = 0; i < nOp; i++) {
		const struct cnp_fragment *frag = &cnp_fragments[aOp[i].opcode];
		size_t frag_pos = (size_t)((uint8_t *)pc_stencil[i] - code);

		for (uint32_t r = 0; r < frag->num_relocs; r++) {
			const struct cnp_fragment_reloc *rel = &frag->relocs[r];
			uintptr_t target;
			uint32_t mapped_offset;
			if (!cnp_fragment_map_offset(frag, i, nOp, rel->offset,
						       &mapped_offset))
				continue;
			if (rel->symbol_name == NULL || rel->symbol_name[0] == '\0')
				target = (uintptr_t)(code + frag_pos);
			else if (aOp[i].opcode == OP_Column &&
				 strcmp(rel->symbol_name, "vdbe_op_column") == 0)
				target = cnp_select_column_handler(p, i);
			else if (((aOp[i].opcode == OP_Add &&
				   strcmp(rel->symbol_name, "vdbe_op_add_sysv_bridge") == 0) ||
				  (aOp[i].opcode == OP_Subtract &&
				   strcmp(rel->symbol_name, "vdbe_op_sub_sysv_bridge") == 0) ||
				  (aOp[i].opcode == OP_Multiply &&
				   strcmp(rel->symbol_name, "vdbe_op_multiply_sysv_bridge") == 0) ||
				  (aOp[i].opcode == OP_Divide &&
				   strcmp(rel->symbol_name, "vdbe_op_divide_sysv_bridge") == 0) ||
				  (aOp[i].opcode == OP_Remainder &&
				   strcmp(rel->symbol_name, "vdbe_op_remainder_sysv_bridge") == 0)))
				target = cnp_select_arith_fragment_handler(p, i);
			else if (aOp[i].opcode == OP_ApplyType &&
				 strcmp(rel->symbol_name, "mem_cast_implicit") == 0)
				target = cnp_select_applytype_cast_helper(p, i);
			else if ((aOp[i].opcode == OP_BitAnd &&
				  (strcmp(rel->symbol_name, "vdbe_op_bitand") == 0 ||
				   strcmp(rel->symbol_name, "vdbe_op_bitand_inline") == 0)) ||
				 (aOp[i].opcode == OP_BitOr &&
				  (strcmp(rel->symbol_name, "vdbe_op_bitor") == 0 ||
				   strcmp(rel->symbol_name, "vdbe_op_bitor_inline") == 0)) ||
				 (aOp[i].opcode == OP_BitNot &&
				  (strcmp(rel->symbol_name, "vdbe_op_bitnot") == 0 ||
				   strcmp(rel->symbol_name, "vdbe_op_bitnot_inline") == 0)) ||
				 (aOp[i].opcode == OP_ShiftLeft &&
				  strcmp(rel->symbol_name, "vdbe_op_shiftleft_inline") == 0) ||
				 (aOp[i].opcode == OP_ShiftRight &&
				  strcmp(rel->symbol_name, "vdbe_op_shiftright_inline") == 0))
				target = cnp_select_bitwise_handler(p, i);
			else
				target = cnp_resolve_fragment_symbol(rel->symbol_name);
			if (target == 0) {
				sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
					"fragment compile: unresolved symbol '%s' for %s at pc %d",
					rel->symbol_name, sqlOpcodeName(aOp[i].opcode), i);
				free(pc_stencil);
				p->cnp_compiled = CNP_COMPILE_FAILED;
				return -1;
			}
			if (cnp_reloc_needs_thunk(frag->bytes, frag->size,
						  rel->offset, rel->reloc_type)) {
				cnp_emit_abs_jmp_thunk(code + thunk_pos, target);
				target = (uintptr_t)(code + thunk_pos);
				thunk_pos += cnp_abs_jmp_thunk_size();
			}
			cnp_patch(code + frag_pos + mapped_offset, target,
				  rel->reloc_type, rel->addend);
		}
		cnp_fragment_patch_jump_p2_transfers(code + frag_pos, frag,
						     p, pc_stencil, i);
		cnp_fragment_patch_fallthrough_transfer(code + frag_pos, frag,
						       p, pc_stencil, i);
	}

	__builtin___clear_cache((char *)code, (char *)(code + thunk_pos));

	p->cnp_code = code;
	p->cnp_size = (uint32_t)thunk_pos;
	p->cnp_compiled = CNP_COMPILED;
	p->cnp_mode = CNP_MODE_FRAGMENTS;
	p->cnp_pc_stencil = pc_stencil;
	p->cnp_nop = nOp;
	sql_clear_last_compile_error(SQL_NATIVE_COMPILE_CNP);
	sql_cnp_compile_success_count++;
	sql_cnp_compiled_bytes += thunk_pos;

	cnp_perf_map_add(p, code, (uint32_t)thunk_pos);
	cnp_register_frame(p);
	cnp_gdb_register(p);

	return 0;
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
	sql_clear_last_compile_error(SQL_NATIVE_COMPILE_CNP);

	int nOp = p->nOp;
	Op *aOp = p->aOp;
	p->cnp_arith_imm = (struct cnp_arith_imm *)calloc(nOp,
						       sizeof(*p->cnp_arith_imm));
	if (p->cnp_arith_imm == NULL) {
		sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
			"compile: failed to allocate arithmetic metadata for %d ops",
			nOp);
		return -1;
	}
	p->cnp_column_group = (struct cnp_column_group *)calloc(
		nOp, sizeof(*p->cnp_column_group));
	if (p->cnp_column_group == NULL) {
		sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
			"compile: failed to allocate column-group metadata for %d ops",
			nOp);
		free(p->cnp_arith_imm);
		p->cnp_arith_imm = NULL;
		return -1;
	}
	p->cnp_column_path = (struct cnp_column_path *)calloc(
		nOp, sizeof(*p->cnp_column_path));
	if (p->cnp_column_path == NULL) {
		sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
			"compile: failed to allocate column-path metadata for %d ops",
			nOp);
		free(p->cnp_column_group);
		p->cnp_column_group = NULL;
		free(p->cnp_arith_imm);
		p->cnp_arith_imm = NULL;
		return -1;
	}
	bool use_fragments = cnp_can_use_fragments(p);

	if (use_fragments)
		return vdbe_cnp_compile_fragments(p);

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
			sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
				"stencil compile: no stencil for %s at pc %d",
				sqlOpcodeName(opcode), i);
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
		if (pc_offset == NULL) {
			sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
				"stencil compile: failed to allocate pc offsets for %d ops",
				nOp);
			return -1;
		}
		pc_offset_heap = 1;
	}

	/*
 * Phase 2: Allocate code buffer from the shared RWX arena.
 * This avoids a per-compile mmap+mprotect pair (~5 µs overhead).
 */
	uint8_t *code = cnp_arena_alloc(total_size);
	if (code == NULL) {
		sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
			"stencil compile: failed to allocate %u bytes of code",
			total_size);
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
		sql_set_last_compile_error(SQL_NATIVE_COMPILE_CNP,
			"stencil compile: failed to allocate pc_stencil for %d ops",
			nOp);
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
				if (aOp[i].opcode == OP_Column) {
					target = cnp_select_column_handler(p, i);
				} else if (aOp[i].opcode == OP_Add ||
					   aOp[i].opcode == OP_Subtract ||
					   aOp[i].opcode == OP_Multiply ||
					   aOp[i].opcode == OP_Divide ||
					   aOp[i].opcode == OP_Remainder) {
					target = cnp_select_arith_handler(p, i);
				} else if (aOp[i].opcode == OP_BitAnd ||
					   aOp[i].opcode == OP_BitOr ||
					   aOp[i].opcode == OP_BitNot ||
					   aOp[i].opcode == OP_ShiftLeft ||
					   aOp[i].opcode == OP_ShiftRight) {
					target = cnp_select_bitwise_handler(p, i);
				} else {
					target = cnp_resolve_handler_by_opcode(
						aOp[i].opcode);
				}
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
	p->cnp_mode = CNP_MODE_STENCILS;
	p->cnp_pc_stencil = pc_stencil;
	p->cnp_nop = nOp;
	sql_clear_last_compile_error(SQL_NATIVE_COMPILE_CNP);
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

	if (p->cnp_mode == CNP_MODE_FRAGMENTS) {
		cnp_fragment_func_t func;
		VdbeOp *entry_pOp;
		VdbeOp *current_pOp;

		cnp_frag_p = p;
		cnp_frag_aOp = p->aOp;
		cnp_frag_aMem = p->aMem;
		cnp_frag_dispatch_table = p->cnp_pc_stencil;
		cnp_frag_done_target = p->cnp_pc_stencil[p->nOp];
		cnp_frag_row_target = p->cnp_pc_stencil[p->nOp + 1];
		cnp_frag_error_target = p->cnp_pc_stencil[p->nOp + 2];

		if (p->cnp_resume_func != NULL) {
			sql_cnp_resume_count++;
			cnp_frag_pOp = (VdbeOp *)p->cnp_resume_func;
			p->cnp_resume_func = NULL;
		} else {
			cnp_frag_pOp = &p->aOp[p->pc];
		}
		entry_pOp = cnp_frag_pOp;
		/*
		 * Common single-row tail: OP_ResultRow sets p->pc to the next
		 * opcode, and many scalar / aggregate statements resume directly at
		 * OP_Halt. The terminal fragment handles the halt cleanup itself.
		 */
		func = (cnp_fragment_func_t)
			p->cnp_pc_stencil[(size_t)(entry_pOp - p->aOp)];
		current_pOp = entry_pOp;

		int64_t result = cnp_fragment_enter((void *)func, p, p->aOp,
						      entry_pOp, p->aMem);
		if (result == SQL_ROW) {
			sql_cnp_row_return_count++;
			if (p->pc >= 0 && p->pc < p->nOp)
				p->cnp_resume_func = &p->aOp[p->pc];
			else
				p->cnp_resume_func = NULL;
			return SQL_ROW;
		}
		if (result == SQL_DONE) {
			if (current_pOp->opcode == OP_Halt) {
				int rc = vdbe_cnp_halt_handler(p, current_pOp,
							      p->aMem);
				if (rc > 0) {
					sql_cnp_done_return_count++;
					return SQL_DONE;
				}
				sql_cnp_error_return_count++;
				return -1;
			}
			sql_cnp_done_return_count++;
		} else if (result < 0) {
			sql_cnp_error_return_count++;
		}
		return (int)result;
	}

	/*
 * Resume from the stencil after OP_ResultRow if the previous call
 * returned SQL_ROW.
 */
	cnp_stencil_func_t func;
	int current_pc = p->pc;
	if (p->cnp_resume_func != NULL) {
		sql_cnp_resume_count++;
		func = (cnp_stencil_func_t)p->cnp_resume_func;
		p->cnp_resume_func = NULL;
		current_pc = cnp_find_pc_for_func(p, func);
	} else {
		func = (cnp_stencil_func_t)p->cnp_pc_stencil[current_pc];
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
			current_pc = cnp_find_pc_for_func(p, func);
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
			current_pc = pc;
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

int
vdbe_cnp_disassemble(struct Vdbe *p, char **out)
{
	(void)p;
	(void)out;
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
