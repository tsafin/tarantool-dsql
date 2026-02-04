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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Function type for JIT-compiled VDBE execution */
typedef int (*VdbeJitFunc)(struct Vdbe *p, int start_pc);

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
	 * Load bitcode files from build directory.
	 * In production, these would be in: ${CMAKE_INSTALL_DATAROOTDIR}/tarantool/sql_handlers/
	 * For now, loading is deferred - modules initialized as NULL.
	 * 
	 * TODO: Implement actual bitcode loading:
	 * for (int i = 0; i < count; i++) {
	 *     char path[PATH_MAX];
	 *     snprintf(path, sizeof(path), "%s/sql_handlers/%s",
	 *              CMAKE_INSTALL_DATAROOTDIR, handler_bitcode_files[i]);
	 *     LLVMMemoryBufferRef mem_buf;
	 *     if (LLVMCreateMemoryBufferWithContentsOfFile(path, &mem_buf, &error_msg) != 0) {
	 *         // Handle error
	 *     }
	 *     if (LLVMParseBitcode2(mem_buf, &jit_state.handler_modules[i]) != 0) {
	 *         // Handle error
	 *     }
	 *     LLVMDisposeMemoryBuffer(mem_buf);
	 * }
	 */
	for (int i = 0; i < count; i++) {
		jit_state.handler_modules[i] = NULL;
	}
	
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

	/* Don't compile empty programs */
	if (p->nOp == 0) {
		p->jit_compiled = 0;
		p->jit_func = NULL;
		p->jit_module = NULL;
		return 0;
	}

	/*
	 * Step 3 Implementation - Phase 1: Basic infrastructure
	 * 
	 * For now, we just create a minimal LLVM module and function
	 * to validate the compilation pipeline. Full implementation
	 * of handler linking and inlining will follow in subsequent phases.
	 * 
	 * The JIT-compiled function has signature:
	 *   int vdbe_jit_exec(struct Vdbe *p, int start_pc)
	 * 
	 * It returns:
	 *   >= 0: PC value where execution should continue in interpreter
	 *   -1: Execution completed successfully
	 *   < -1: Error occurred
	 */

	/* Create a new module for this VDBE program */
	char module_name[64];
	snprintf(module_name, sizeof(module_name), "vdbe_jit_%p", (void *)p);
	LLVMModuleRef module = LLVMModuleCreateWithName(module_name);
	if (module == NULL) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			 "Failed to create LLVM module for JIT compilation");
		return -1;
	}

	/* Create function type: int(struct Vdbe *, int) */
	LLVMTypeRef param_types[2] = {
		LLVMPointerType(LLVMInt8Type(), 0),  /* struct Vdbe * (as opaque pointer) */
		LLVMInt32Type()                       /* int start_pc */
	};
	LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);

	/* Create the JIT function */
	char func_name[64];
	snprintf(func_name, sizeof(func_name), "vdbe_jit_exec_%p", (void *)p);
	LLVMValueRef func = LLVMAddFunction(module, func_name, func_type);
	LLVMSetFunctionCallConv(func, LLVMCCallConv);

	/* Create entry basic block */
	LLVMBasicBlockRef entry_block = LLVMAppendBasicBlock(func, "entry");
	LLVMBuilderRef builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(builder, entry_block);

	/*
	 * Phase 1: Minimal implementation - just return -1 (execution complete)
	 * TODO Phase 2: Analyze opcodes and generate IR for each
	 * TODO Phase 3: Link handler functions from bitcode modules
	 * TODO Phase 4: Inline jitable operations
	 * TODO Phase 5: Apply optimization passes
	 */

	/* For now: return -1 to indicate execution completed */
	LLVMBuildRet(builder, LLVMConstInt(LLVMInt32Type(), -1, 0));
	LLVMDisposeBuilder(builder);

	/* Verify the module */
	char *error_msg = NULL;
	if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error_msg) != 0) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			 tt_sprintf("JIT module verification failed: %s", error_msg));
		LLVMDisposeMessage(error_msg);
		LLVMDisposeModule(module);
		return -1;
	}

	/* Add module to execution engine */
	LLVMModuleRef old_module;
	LLVMGetExecutionEngineTargetMachine(jit_state.engine);
	LLVMAddModule(jit_state.engine, module);

	/* Get pointer to compiled function */
	uint64_t func_addr = LLVMGetFunctionAddress(jit_state.engine, func_name);
	if (func_addr == 0) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			 "Failed to get JIT function address");
		/* Module is owned by engine, will be cleaned up */
		return -1;
	}

	/* Store JIT compilation results */
	p->jit_func = (void *)(uintptr_t)func_addr;
	p->jit_module = module;
	p->jit_compiled = 1;

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
	/*
	 * Note: jit_module is owned by the execution engine after LLVMAddModule,
	 * so we don't dispose it here. The engine will clean it up on shutdown.
	 */
	if (p->jit_module != NULL) {
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
