/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 */

/*
 * This file contains the implementation of the VDBE JIT compiler using LLVM.
 * It provides ahead-of-time (AOT) compilation of SQL prepared statements by
 * linking and inlining LLVM bitcode from pre-compiled handler files.
 *
 * Key components:
 * - vdbe_jit_init(): Loads handler bitcode modules at startup
 * - vdbe_jit_compile(): Compiles a VDBE program into native code
 * - vdbe_jit_cleanup(): Releases JIT resources
 *
 * The JIT compiler operates at PREPARE time and generates specialized native
 * code for arithmetic, comparison, and logical operations while maintaining
 * seamless fallback to the interpreter for I/O operations.
 */

#include "sqlInt.h"
#include "vdbeInt.h"

#ifdef ENABLE_SQL_JIT

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/Transforms/Scalar.h>
#include <llvm-c/Transforms/Utils.h>
#include <llvm-c/OrcBindings.h>

/*
 * Global JIT state - initialized once at startup
 */
struct sql_jit_state {
	/** LLVM execution engine for JIT compilation */
	LLVMExecutionEngineRef engine;
	/** Array of loaded handler bitcode modules */
	LLVMModuleRef *handler_modules;
	/** Number of loaded handler modules */
	int module_count;
	/** Whether JIT is initialized (0 = false, 1 = true) */
	int initialized;
};

static struct sql_jit_state jit_state = {0};

/*
 * List of handler bitcode files to load at initialization
 */
static const char *handler_bitcode_files[] = {
	"vdbe_ops_arith.bc",
	"vdbe_ops_compare.bc",
	"vdbe_ops_logical.bc",
	"vdbe_ops_data.bc",
	"vdbe_ops_cursor_data.bc",
	"vdbe_ops_index.bc",
	"vdbe_ops_string.bc",
	"vdbe_ops_type.bc",
	NULL
};

/**
 * Initialize the JIT compiler subsystem.
 * Loads all handler bitcode modules into memory for later use.
 * 
 * @return 0 on success, -1 on error
 */
int
vdbe_jit_init(void)
{
	if (jit_state.initialized)
		return 0;

	/* Initialize LLVM native target */
	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	/* Count handler modules */
	int count = 0;
	while (handler_bitcode_files[count] != NULL)
		count++;
	
	jit_state.module_count = count;
	jit_state.handler_modules = calloc(count, sizeof(LLVMModuleRef));
	if (jit_state.handler_modules == NULL) {
		diag_set(OutOfMemory, count * sizeof(LLVMModuleRef),
			 "calloc", "jit_state.handler_modules");
		return -1;
	}

	/*
	 * TODO: Load bitcode files from installation directory.
	 * Path: ${CMAKE_INSTALL_DATAROOTDIR}/tarantool/sql_handlers/
	 * For now, leave modules uninitialized - will be implemented in next iteration.
	 */
	
	char *error_msg = NULL;
	LLVMLinkInMCJIT();
	
	/* Create a dummy module for the execution engine */
	LLVMModuleRef dummy_module = LLVMModuleCreateWithName("vdbe_jit_dummy");
	if (LLVMCreateExecutionEngineForModule(&jit_state.engine, dummy_module, &error_msg) != 0) {
		diag_set(ClientError, ER_SQL_EXECUTE, error_msg);
		LLVMDisposeMessage(error_msg);
		free(jit_state.handler_modules);
		LLVMDisposeModule(dummy_module);
		return -1;
	}

	jit_state.initialized = 1;
	return 0;
}

/**
 * Compile a VDBE program into native code using JIT.
 * 
 * This function analyzes the VDBE program, identifies opcodes suitable for
 * inlining, generates LLVM IR by linking handler functions, applies
 * optimizations, and compiles to native code.
 * 
 * @param p  VDBE program to compile
 * @return 0 on success, -1 on error
 */
int
vdbe_jit_compile(struct Vdbe *p)
{
	if (!jit_state.initialized) {
		if (vdbe_jit_init() != 0)
			return -1;
	}

	/*
	 * TODO: Implement JIT compilation logic (Step 3 of plan)
	 * 1. Create LLVM function: i32 @vdbe_jit_exec_%d(ptr %vdbe_ptr, i32 %start_pc)
	 * 2. Scan p->aOp[] and classify opcodes (jitable/callable/unsupported)
	 * 3. For jitable opcodes: clone handler from bitcode and inline
	 * 4. For callable opcodes: emit call instruction
	 * 5. For unsupported: emit return instruction
	 * 6. Apply LLVM optimization passes
	 * 7. Compile to native code and store in p->jit_func
	 */

	/* Mark as not compiled for now */
	p->jit_compiled = 0;
	p->jit_func = NULL;
	p->jit_module = NULL;

	return 0;
}

/**
 * Clean up JIT resources for a VDBE program.
 * Called when the VDBE is being freed.
 * 
 * @param p  VDBE program to clean up
 */
void
vdbe_jit_cleanup(struct Vdbe *p)
{
	if (p->jit_module != NULL) {
		LLVMDisposeModule((LLVMModuleRef)p->jit_module);
		p->jit_module = NULL;
	}
	p->jit_func = NULL;
	p->jit_compiled = 0;
}

/**
 * Shut down the JIT compiler subsystem.
 * Releases all loaded handler modules and execution engine.
 */
void
vdbe_jit_shutdown(void)
{
	if (!jit_state.initialized)
		return;

	if (jit_state.handler_modules != NULL) {
		for (int i = 0; i < jit_state.module_count; i++) {
			if (jit_state.handler_modules[i] != NULL) {
				LLVMDisposeModule(jit_state.handler_modules[i]);
			}
		}
		free(jit_state.handler_modules);
		jit_state.handler_modules = NULL;
	}

	if (jit_state.engine != NULL) {
		LLVMDisposeExecutionEngine(jit_state.engine);
		jit_state.engine = NULL;
	}

	jit_state.initialized = 0;
	jit_state.module_count = 0;
}

/**
 * Check if JIT compilation is available and enabled.
 * 
 * @return 1 if JIT is available and enabled in configuration, 0 otherwise
 */
int
vdbe_jit_is_enabled(void)
{
	/*
	 * TODO: Check box.cfg{sql = {jit = {enable = false}}} setting
	 * For now, return 0 (disabled by default for safety)
	 */
	return 0;
}

#else /* !ENABLE_SQL_JIT */

/* Stub implementations when JIT is disabled */

int
vdbe_jit_init(void)
{
	return 0;
}

int
vdbe_jit_compile(struct Vdbe *p)
{
	(void)p;
	return 0;
}

void
vdbe_jit_cleanup(struct Vdbe *p)
{
	(void)p;
}

void
vdbe_jit_shutdown(void)
{
}

int
vdbe_jit_is_enabled(void)
{
	return 0;
}

#endif /* ENABLE_SQL_JIT */
