/*
 * Embedded LLVM-backed disassembler for CnP native code.
 *
 * Keep this in a separate translation unit so the cold EXPLAIN/disassembly
 * path does not perturb the layout of the hot CnP runtime in vdbe_cnp.c.
 */

#include "sqlInt.h"
#include "vdbeInt.h"
#include "vdbe.h"
#include "vdbe_cnp.h"

#ifdef ENABLE_SQL_CNP

#if defined(__x86_64__) || defined(__i386__)
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>

static const char *
cnp_llvm_triple(void)
{
#if defined(__x86_64__)
	return "x86_64-pc-linux-gnu";
#elif defined(__i386__)
	return "i386-pc-linux-gnu";
#else
	return NULL;
#endif
}

struct cnp_disasm_insn {
	uint64_t addr;
	size_t size;
	bool has_local_target;
	uint64_t local_target;
	char text[256];
};

static bool
cnp_x86_decode_rel_target(const uint8_t *bytes, size_t avail, uint64_t addr,
			  size_t insn_size, uint64_t *target)
{
	if (avail == 0 || insn_size == 0)
		return false;
	if (bytes[0] == 0xe8 || bytes[0] == 0xe9) {
		if (avail < 5 || insn_size < 5)
			return false;
		int32_t disp;
		memcpy(&disp, bytes + 1, sizeof(disp));
		*target = addr + 5 + disp;
		return true;
	}
	if (bytes[0] == 0xeb || (bytes[0] >= 0x70 && bytes[0] <= 0x7f)) {
		if (avail < 2 || insn_size < 2)
			return false;
		int8_t disp = (int8_t)bytes[1];
		*target = addr + 2 + disp;
		return true;
	}
	if (bytes[0] == 0x0f &&
	    avail >= 6 && insn_size >= 6 &&
	    bytes[1] >= 0x80 && bytes[1] <= 0x8f) {
		int32_t disp;
		memcpy(&disp, bytes + 2, sizeof(disp));
		*target = addr + 6 + disp;
		return true;
	}
	return false;
}

static void
cnp_format_insn_text(char *dst, size_t dst_size, const char *src)
{
	while (*src == ' ' || *src == '\t')
		++src;
	const char *mn_end = src;
	while (*mn_end != '\0' && *mn_end != ' ' && *mn_end != '\t')
		++mn_end;
	if (*mn_end == '\0') {
		snprintf(dst, dst_size, "%s", src);
		return;
	}
	int mnemonic_len = (int)(mn_end - src);
	const char *sep = mn_end;
	while (*sep == ' ' || *sep == '\t')
		++sep;
	snprintf(dst, dst_size, "%-*.*s %s", 8, mnemonic_len, src, sep);
}

static int
cnp_llvm_disassemble(struct Vdbe *p, char **out) __attribute__((cold));

static int
cnp_llvm_disassemble(struct Vdbe *p, char **out)
{
	*out = NULL;
	const char *triple = cnp_llvm_triple();
	if (triple == NULL)
		return -1;

	LLVMInitializeX86TargetInfo();
	LLVMInitializeX86Target();
	LLVMInitializeX86TargetMC();
	LLVMInitializeX86Disassembler();

	LLVMDisasmContextRef dc =
		LLVMCreateDisasmCPU(triple, "", NULL, 0, NULL, NULL);
	if (dc == NULL)
		return -1;

	(void)LLVMSetDisasmOptions(dc,
				   LLVMDisassembler_Option_AsmPrinterVariant |
				   LLVMDisassembler_Option_PrintImmHex);

	char zBase[256];
	StrAccum acc;
	sqlStrAccumInit(&acc, zBase, sizeof(zBase), SQL_MAX_LENGTH);

	uint64_t pc = (uint64_t)(uintptr_t)p->cnp_code;
	struct cnp_disasm_insn *insns =
		sql_xmalloc(sizeof(*insns) * p->cnp_size);
	uint64_t *labels = sql_xmalloc(sizeof(*labels) * p->cnp_size);
	size_t insn_count = 0;
	size_t label_count = 0;
	size_t offset = 0;
	while (offset < p->cnp_size) {
		char line[256];
		size_t insn_size = LLVMDisasmInstruction(dc,
			(uint8_t *)p->cnp_code + offset, p->cnp_size - offset,
			pc + offset, line, sizeof(line));
		if (insn_size == 0) {
			LLVMDisasmDispose(dc);
			sql_xfree(insns);
			sql_xfree(labels);
			sqlStrAccumReset(&acc);
			return -1;
		}
		for (char *cur = line; *cur != '\0'; ++cur) {
			if (*cur == '\t')
				*cur = ' ';
		}
		insns[insn_count].addr = pc + offset;
		insns[insn_count].size = insn_size;
		insns[insn_count].has_local_target = false;
		insns[insn_count].local_target = 0;
		snprintf(insns[insn_count].text,
			 sizeof(insns[insn_count].text), "%s", line);
		uint64_t target;
		if (cnp_x86_decode_rel_target((uint8_t *)p->cnp_code + offset,
					      p->cnp_size - offset,
					      insns[insn_count].addr,
					      insn_size, &target) &&
		    target >= pc && target < pc + p->cnp_size) {
			bool seen = false;
			for (size_t i = 0; i < label_count; ++i) {
				if (labels[i] == target) {
					seen = true;
					break;
				}
			}
			if (!seen)
				labels[label_count++] = target;
			insns[insn_count].has_local_target = true;
			insns[insn_count].local_target = target;
		}
		++insn_count;
		offset += insn_size;
	}

	LLVMDisasmDispose(dc);
	for (size_t i = 0; i < insn_count; ++i) {
		for (size_t j = 0; j < label_count; ++j) {
			if (labels[j] == insns[i].addr) {
				sqlXPrintf(&acc, "%llx: L%04llx:\n",
					   (unsigned long long)(insns[i].addr -
								pc),
					   (unsigned long long)(insns[i].addr -
								pc));
				break;
			}
		}
		char formatted[256];
		if (insns[i].has_local_target) {
			const char *src = insns[i].text;
			while (*src == ' ' || *src == '\t')
				++src;
			const char *mn_end = src;
			while (*mn_end != '\0' && *mn_end != ' ' &&
			       *mn_end != '\t')
				++mn_end;
			int mnemonic_len = (int)(mn_end - src);
			snprintf(formatted, sizeof(formatted), "%-*.*s L%04llx",
				 8, mnemonic_len, src,
				 (unsigned long long)(insns[i].local_target -
						      pc));
		} else {
			cnp_format_insn_text(formatted, sizeof(formatted),
					     insns[i].text);
		}
		sqlXPrintf(&acc, "%llx: %s\n",
			   (unsigned long long)(insns[i].addr - pc),
			   formatted);
	}
	sql_xfree(insns);
	sql_xfree(labels);
	*out = sqlStrAccumFinish(&acc);
	return *out != NULL ? 0 : -1;
}
#endif

int
vdbe_cnp_disassemble(struct Vdbe *p, char **out)
{
	*out = NULL;
	if (vdbe_cnp_compile(p) != 0 || p->cnp_compiled != CNP_COMPILED)
		return -1;
#if defined(__x86_64__) || defined(__i386__)
	return cnp_llvm_disassemble(p, out);
#else
	(void)p;
	(void)out;
	return -1;
#endif
}

#else /* ENABLE_SQL_CNP */

int
vdbe_cnp_disassemble(struct Vdbe *p, char **out)
{
	(void)p;
	(void)out;
	return -1;
}

#endif /* ENABLE_SQL_CNP */
