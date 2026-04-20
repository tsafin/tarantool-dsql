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
