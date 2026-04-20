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
 * Resume model (M2): OP_ResultRow calls cnp_signal_row() via HOLE_SIGNAL
 * to set p->cnp_row_ready.  The exec loop detects this flag, saves the
 * next stencil address in p->cnp_resume_func, and returns SQL_ROW to the
 * caller.  On the next call to vdbe_cnp_exec(), execution resumes from
 * p->cnp_resume_func.
 */

#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "sqlInt.h"
#include "vdbeInt.h"
#include "vdbe.h"
#include "vdbe_cnp.h"
#include "vdbe_ops.h"

#ifdef ENABLE_SQL_CNP
#include "generated/vdbe_cnp_stencils.h"

/*
 * R_X86_64_32 (unsigned 32-bit absolute) — not in the stencils header
 * by default because we defined only the signed variant.  Add it here.
 */
#ifndef CNP_R_X86_64_32
#define CNP_R_X86_64_32 10
#endif

/* Execution counter exposed via box.stat.sql() */
extern int64_t sql_cnp_exec_count;

/*
 * Stencil function type.  Each stencil returns the address of the
 * next stencil to execute, or a terminal status code.
 */
typedef int64_t (*cnp_stencil_func_t)(struct Vdbe *p, Mem *aMem);

/*
 * Threshold: return values below this are status codes, not addresses.
 * Any valid mmap'd address will be well above this.
 */
#define CNP_ADDR_THRESHOLD  4096

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
 * and return 1 (jump to P2).  Mirrors sql_vdbe_exec_init_for_jit() in vdbe.c.
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
 *
 * sqlVdbeHalt() requires p->pc >= 0 to run cleanup.  The OP_Init handler
 * already set p->pc = P2 (>= 1), so the guard is satisfied.
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
 * Resolve the external handler address for opcodes that use HOLE_HANDLER.
 */
static uintptr_t
cnp_resolve_call_by_opcode(int opcode)
{
	switch (opcode) {
	case OP_Add:       return (uintptr_t)vdbe_op_add;
	case OP_Integer:   return (uintptr_t)vdbe_op_integer;
	case OP_Copy:      return (uintptr_t)vdbe_op_copy;
	case OP_ResultRow: return (uintptr_t)vdbe_op_resultrow;
	case OP_Init:      return (uintptr_t)vdbe_cnp_init_handler;
	case OP_Halt:      return (uintptr_t)vdbe_cnp_halt_handler;
	default:           return 0;
	}
}

/*
 * Apply a single relocation patch to the code buffer.
 */
static void
cnp_patch(uint8_t *patch_addr, uintptr_t target,
	  uint8_t reloc_type, int addend)
{
	switch (reloc_type) {
	case CNP_R_X86_64_64: {
		/* Absolute 64-bit (movabs immediate) */
		uint64_t val = (uint64_t)target + addend;
		memcpy(patch_addr, &val, 8);
		break;
	}
	case CNP_R_X86_64_PC32:
	case CNP_R_X86_64_PLT32: {
		/* PC-relative 32-bit: S + A - P */
		uintptr_t P = (uintptr_t)patch_addr;
		int32_t val = (int32_t)((int64_t)target +
					addend - (int64_t)P);
		memcpy(patch_addr, &val, 4);
		break;
	}
	case CNP_R_X86_64_32: {
		/* Unsigned 32-bit absolute */
		uint32_t val = (uint32_t)((uint64_t)target + addend);
		memcpy(patch_addr, &val, 4);
		break;
	}
	case CNP_R_X86_64_32S: {
		/* Signed 32-bit absolute */
		int32_t val = (int32_t)((int64_t)target + addend);
		memcpy(patch_addr, &val, 4);
		break;
	}
	default:
		fprintf(stderr, "cnp: unsupported reloc type %d\n",
			reloc_type);
		break;
	}
}

int
vdbe_cnp_compile(struct Vdbe *p)
{
	if (p->cnp_compiled)
		return 0;

	int nOp = p->nOp;
	Op *aOp = p->aOp;

	/*
	 * Phase 1: Check that every opcode has a stencil.
	 */
	uint32_t total_size = 0;
	for (int i = 0; i < nOp; i++) {
		int opcode = aOp[i].opcode;
		if (opcode > CNP_MAX_OPCODE ||
		    cnp_stencils[opcode].bytes == NULL) {
			return -1;
		}
		total_size += cnp_stencils[opcode].size;
	}

	/*
	 * Phase 2: Allocate RW buffer.
	 */
	uint8_t *code = (uint8_t *)mmap(NULL, total_size,
					 PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS,
					 -1, 0);
	if (code == MAP_FAILED)
		return -1;

	/*
	 * Phase 3: Copy stencils and build pc-to-offset map.
	 */
	uint32_t *pc_offset = (uint32_t *)calloc(nOp + 1,
						  sizeof(uint32_t));
	if (pc_offset == NULL) {
		munmap(code, total_size);
		return -1;
	}

	uint32_t pos = 0;
	for (int i = 0; i < nOp; i++) {
		pc_offset[i] = pos;
		const struct cnp_stencil *st =
			&cnp_stencils[aOp[i].opcode];
		memcpy(code + pos, st->bytes, st->size);
		pos += st->size;
	}
	pc_offset[nOp] = pos;

	/*
	 * Phase 4: Patch holes.
	 *
	 * HOLE_NEXT   — address of next stencil (or SQL_DONE at end of program)
	 * HOLE_BRANCH — jump target address, or SQL_DONE for OP_Halt
	 * HOLE_SIGNAL — address of cnp_signal_row (called by OP_ResultRow)
	 * HOLE_HANDLER — address of the opcode's C handler function
	 * HOLE_ERROR_EXIT — SQL error code (-1)
	 * HOLE_P1..P5 — operand values
	 */
	for (int i = 0; i < nOp; i++) {
		const struct cnp_stencil *st =
			&cnp_stencils[aOp[i].opcode];
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
			case CNP_HOLE_P5:
				target = (uintptr_t)aOp[i].p5;
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
					/* OP_Halt: rc>0 means done */
					target = (uintptr_t)SQL_DONE;
				} else if (aOp[i].p2 >= 0 &&
					   aOp[i].p2 < nOp) {
					target = (uintptr_t)(code +
						pc_offset[aOp[i].p2]);
				} else {
					target = (uintptr_t)SQL_DONE;
				}
				break;
			case CNP_HOLE_SIGNAL:
				target = (uintptr_t)cnp_signal_row;
				break;
			case CNP_HOLE_HANDLER:
				target = cnp_resolve_call_by_opcode(
					aOp[i].opcode);
				break;
			case CNP_HOLE_ERROR_EXIT:
				target = (uintptr_t)(intptr_t)(-1);
				break;
			default:
				break;
			}

			cnp_patch(patch_addr, target,
				  hole->reloc_type, hole->addend);
		}
	}

	free(pc_offset);

	/*
	 * Phase 5: Make executable.
	 */
	if (mprotect(code, total_size, PROT_READ | PROT_EXEC) != 0) {
		munmap(code, total_size);
		return -1;
	}
	__builtin___clear_cache((char *)code,
				(char *)(code + total_size));

	p->cnp_code = code;
	p->cnp_size = total_size;
	p->cnp_compiled = 1;
	return 0;
}

int
vdbe_cnp_exec(struct Vdbe *p)
{
	if (!p->cnp_compiled)
		return -1;

	sql_cnp_exec_count++;

	/*
	 * Resume from the stencil after OP_ResultRow if the previous call
	 * returned SQL_ROW.
	 */
	cnp_stencil_func_t func;
	if (p->cnp_resume_func != NULL) {
		func = (cnp_stencil_func_t)p->cnp_resume_func;
		p->cnp_resume_func = NULL;
	} else {
		func = (cnp_stencil_func_t)p->cnp_code;
	}

	int64_t result;
	for (;;) {
		result = func(p, p->aMem);
		if (result < (int64_t)CNP_ADDR_THRESHOLD) {
			/* Terminal status code */
			return (int)result;
		}
		/* result is the address of the next stencil */
		func = (cnp_stencil_func_t)result;
		if (p->cnp_row_ready) {
			/*
			 * OP_ResultRow fired the signal.  Save the next
			 * stencil address and return SQL_ROW to the caller.
			 * The next exec call will resume from here.
			 */
			p->cnp_row_ready = 0;
			p->cnp_resume_func = (void *)func;
			return SQL_ROW;
		}
	}
}

void
vdbe_cnp_release(struct Vdbe *p)
{
	if (p->cnp_code != NULL) {
		munmap(p->cnp_code, p->cnp_size);
		p->cnp_code = NULL;
		p->cnp_size = 0;
		p->cnp_compiled = 0;
	}
	p->cnp_resume_func = NULL;
	p->cnp_row_ready = 0;
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
