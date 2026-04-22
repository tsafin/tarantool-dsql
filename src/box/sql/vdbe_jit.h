/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */

/*
 * Interface for VDBE JIT compiler.
 * Provides functions to initialize, compile, and manage JIT-compiled VDBE programs.
 */

#pragma once

#include <limits.h>

struct Vdbe;

/*
 * JIT function return contract:
 *   >= 0               - fallback PC for interpreter resume;
 *   VDBE_JIT_RC_DONE   - statement finished fully in JIT;
 *   VDBE_JIT_RC_ROW    - statement produced a row fully in JIT;
 *   other negative     - SQL error, already recorded in diagnostics.
 */
#define VDBE_JIT_RC_DONE INT_MIN
#define VDBE_JIT_RC_ROW (INT_MIN + 1)

#ifdef ENABLE_SQL_JIT

/**
 * Initialize the JIT compiler subsystem.
 * Must be called before any other JIT functions.
 * 
 * @return 0 on success, -1 on error
 */
int
vdbe_jit_init(void);

/**
 * Compile a VDBE program into native code using JIT.
 * 
 * @param p  VDBE program to compile
 * @return 0 on success, -1 on error
 */
int
vdbe_jit_compile(struct Vdbe *p);

/**
 * Clean up JIT resources for a VDBE program.
 * 
 * @param p  VDBE program to clean up
 */
void
vdbe_jit_cleanup(struct Vdbe *p);

/**
 * Shut down the JIT compiler subsystem.
 * Releases all JIT resources.
 */
void
vdbe_jit_shutdown(void);

/**
 * Check if JIT compilation is available and enabled.
 * 
 * @return 1 if JIT is available and enabled in configuration, 0 otherwise
 */
int
vdbe_jit_is_enabled(void);

/**
 * Remember that a one-shot statement fell back from JIT at the given PC,
 * so equivalent future statements can skip JIT compilation altogether.
 */
void
vdbe_jit_note_fallback(struct Vdbe *p, int fallback_pc);

/**
 * Remember that a one-shot statement produced a row under JIT, so equivalent
 * future statements can skip recompiling a row-at-a-time shape.
 */
void
vdbe_jit_note_row(struct Vdbe *p);

/**
 * Compile a VDBE program for use in the automatic stmt cache.
 *
 * The auto cache reuses a stmt across many box.execute() calls, making it
 * semantically equivalent to a prepared statement.  Unlike vdbe_jit_compile(),
 * this function bypasses the trivial-program filter (inline_count < 8) that
 * suppresses JIT for short non-prepared stmts, since a cached stmt will be
 * executed many times and benefits from native code.
 *
 * No-op if JIT is disabled or the stmt was already compiled (jit_compiled != 0).
 *
 * @param p  VDBE program to compile
 * @return 0 on success, -1 on error
 */
int
vdbe_jit_compile_cached(struct Vdbe *p);

#else /* !ENABLE_SQL_JIT */

static inline int vdbe_jit_init(void) { return 0; }
static inline int vdbe_jit_compile(struct Vdbe *p) { (void)p; return 0; }
static inline void vdbe_jit_cleanup(struct Vdbe *p) { (void)p; }
static inline void vdbe_jit_shutdown(void) {}
static inline int vdbe_jit_is_enabled(void) { return 0; }
static inline void vdbe_jit_note_fallback(struct Vdbe *p, int pc)
	{ (void)p; (void)pc; }
static inline void vdbe_jit_note_row(struct Vdbe *p) { (void)p; }
static inline int vdbe_jit_compile_cached(struct Vdbe *p) { (void)p; return 0; }

#endif /* ENABLE_SQL_JIT */
