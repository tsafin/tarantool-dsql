/*
 * Copy-and-Patch JIT compiler for VDBE.
 *
 * Pre-extracted machine code stencils are patched at prepare time
 * with concrete operand values and jump targets, then executed
 * directly as native code.
 */
#ifndef VDBE_CNP_H
#define VDBE_CNP_H

struct Vdbe;

/*
 * Values for Vdbe.cnp_compiled:
 *
 *   CNP_NOT_COMPILED   — no compile attempt yet (or reset after schema change)
 *   CNP_COMPILED       — compiled successfully; cnp_code is valid
 *   CNP_COMPILE_FAILED — permanent failure (e.g. opcode with no stencil);
 *                        do not retry until schema changes
 *
 * Transient failures (OOM, arena full) leave the state as CNP_NOT_COMPILED
 * so the next call can retry.
 */
#define CNP_NOT_COMPILED   0
#define CNP_COMPILED       1
#define CNP_COMPILE_FAILED (-1)

/**
 * Compile the VDBE program to native code using copy-and-patch.
 * Returns 0 on success, -1 if any opcode lacks a stencil (caller
 * should fall back to the interpreter).
 */
int
vdbe_cnp_compile(struct Vdbe *p);

/**
 * Execute the CnP-compiled native code.
 * Returns SQL_ROW, SQL_DONE, or -1 on error.
 */
int
vdbe_cnp_exec(struct Vdbe *p);

/**
 * Release the mmap'd code buffer.
 */
void
vdbe_cnp_release(struct Vdbe *p);

/**
 * Returns non-zero if CnP compilation is available.
 */
int
vdbe_cnp_is_enabled(void);

#endif /* VDBE_CNP_H */
