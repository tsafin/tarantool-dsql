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
#include <llvm-c/Transforms/IPO.h>
#include <llvm-c/Transforms/Scalar.h>
#include <llvm-c/Transforms/Utils.h>
#include <llvm-c/OrcBindings.h>
#include <llvm-c/Linker.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>

/*
 * Struct layout constants computed at compile time.
 * These allow the JIT to generate correct pointer arithmetic
 * for accessing Vdbe fields through opaque i8* pointers.
 */
enum {
	/** Byte offset of aOp field within struct Vdbe. */
	VDBE_OFFSET_AOP = offsetof(struct Vdbe, aOp),
	/** Byte offset of aMem field within struct Vdbe. */
	VDBE_OFFSET_AMEM = offsetof(struct Vdbe, aMem),
	/** Byte offset of pc field within struct Vdbe. */
	VDBE_OFFSET_PC = offsetof(struct Vdbe, pc),
	/** Size of a single VdbeOp (Op) structure in bytes. */
	VDBE_SIZEOF_OP = sizeof(Op),
};

/* Function type for JIT-compiled VDBE execution */
typedef int (*VdbeJitFunc)(struct Vdbe *p, int start_pc);

/*
 * Opcode JIT mode classification
 */
enum vdbe_jit_mode {
	/** Opcode should be inlined for best performance */
	JIT_MODE_INLINE = 0,
	/** Opcode should be called via FFI (I/O operations) */
	JIT_MODE_CALL = 1,
	/** Opcode not supported by JIT - return to interpreter */
	JIT_MODE_UNSUPPORTED = 2
};

/*
 * Classify opcodes for JIT compilation.
 * This table maps opcode numbers to JIT modes.
 *
 * Inline: Arithmetic, comparison, logical, simple data movement
 * Call: I/O operations, cursor operations, aggregates
 * Unsupported: Control flow, special operations
 */
static const enum vdbe_jit_mode opcode_jit_modes[] = {
	[OP_Savepoint] = JIT_MODE_UNSUPPORTED,
	[OP_SorterNext] = JIT_MODE_CALL,
	[OP_PrevIfOpen] = JIT_MODE_CALL,
	[OP_NextIfOpen] = JIT_MODE_CALL,
	[OP_Prev] = JIT_MODE_CALL,
	[OP_Or] = JIT_MODE_INLINE,           /* Logical operation */
	[OP_And] = JIT_MODE_INLINE,          /* Logical operation */
	[OP_Not] = JIT_MODE_INLINE,          /* Logical operation */
	[OP_Next] = JIT_MODE_CALL,
	[OP_Goto] = JIT_MODE_UNSUPPORTED,    /* Control flow */
	[OP_SetDiag] = JIT_MODE_UNSUPPORTED,
	[OP_Gosub] = JIT_MODE_UNSUPPORTED,   /* Control flow */
	[OP_InitCoroutine] = JIT_MODE_UNSUPPORTED,
	[OP_Ne] = JIT_MODE_INLINE,           /* Comparison */
	[OP_Eq] = JIT_MODE_INLINE,           /* Comparison */
	[OP_Gt] = JIT_MODE_INLINE,           /* Comparison */
	[OP_Le] = JIT_MODE_INLINE,           /* Comparison */
	[OP_Lt] = JIT_MODE_INLINE,           /* Comparison */
	[OP_Ge] = JIT_MODE_INLINE,           /* Comparison */
	[OP_ElseNotEq] = JIT_MODE_UNSUPPORTED,
	[OP_BitAnd] = JIT_MODE_INLINE,       /* Bitwise operation */
	[OP_BitOr] = JIT_MODE_INLINE,        /* Bitwise operation */
	[OP_ShiftLeft] = JIT_MODE_INLINE,    /* Bitwise operation */
	[OP_ShiftRight] = JIT_MODE_INLINE,   /* Bitwise operation */
	[OP_Add] = JIT_MODE_INLINE,          /* Arithmetic */
	[OP_Subtract] = JIT_MODE_INLINE,     /* Arithmetic */
	[OP_Multiply] = JIT_MODE_INLINE,     /* Arithmetic */
	[OP_Divide] = JIT_MODE_INLINE,       /* Arithmetic */
	[OP_Remainder] = JIT_MODE_INLINE,    /* Arithmetic */
	[OP_Concat] = JIT_MODE_CALL,         /* String operation */
	[OP_Yield] = JIT_MODE_UNSUPPORTED,
	[OP_BitNot] = JIT_MODE_INLINE,       /* Bitwise operation */
	[OP_MustBeInt] = JIT_MODE_UNSUPPORTED,
	[OP_Jump] = JIT_MODE_UNSUPPORTED,    /* Control flow */
	[OP_Once] = JIT_MODE_UNSUPPORTED,
	[OP_IfNot] = JIT_MODE_UNSUPPORTED,   /* Control flow */
	[OP_SeekLT] = JIT_MODE_CALL,
	[OP_SeekGT] = JIT_MODE_CALL,
	[OP_SeekGE] = JIT_MODE_CALL,
	[OP_Found] = JIT_MODE_CALL,
	[OP_Last] = JIT_MODE_CALL,
	[OP_SorterSort] = JIT_MODE_CALL,
	[OP_Sort] = JIT_MODE_CALL,
	[OP_Rewind] = JIT_MODE_CALL,
	[OP_IdxGE] = JIT_MODE_CALL,
	[OP_Program] = JIT_MODE_UNSUPPORTED,
	[OP_IfPos] = JIT_MODE_UNSUPPORTED,   /* Control flow */
	[OP_IfNotZero] = JIT_MODE_UNSUPPORTED, /* Control flow */
	[OP_String8] = JIT_MODE_INLINE,      /* Constant load */
	[OP_DecrJumpZero] = JIT_MODE_UNSUPPORTED, /* Control flow */
	[OP_Init] = JIT_MODE_UNSUPPORTED,    /* Control flow */
	[OP_Return] = JIT_MODE_UNSUPPORTED,  /* Control flow */
	[OP_EndCoroutine] = JIT_MODE_UNSUPPORTED,
	[OP_Halt] = JIT_MODE_UNSUPPORTED,    /* Control flow */
	[OP_Integer] = JIT_MODE_INLINE,      /* Constant load */
	[OP_Bool] = JIT_MODE_INLINE,         /* Constant load */
	[OP_Int64] = JIT_MODE_INLINE,        /* Constant load */
	[OP_String] = JIT_MODE_INLINE,       /* Constant load */
	[OP_Null] = JIT_MODE_INLINE,         /* Constant load */
	[OP_Blob] = JIT_MODE_INLINE,         /* Constant load */
	[OP_Variable] = JIT_MODE_CALL,
	[OP_Move] = JIT_MODE_INLINE,         /* Register move */
	[OP_Copy] = JIT_MODE_INLINE,         /* Register copy */
	[OP_SCopy] = JIT_MODE_INLINE,        /* Register copy */
	[OP_ResultRow] = JIT_MODE_UNSUPPORTED,  /* Returns SQL_ROW to caller */
	[OP_SkipLoad] = JIT_MODE_UNSUPPORTED,
	[OP_BuiltinFunction] = JIT_MODE_CALL,
	[OP_FunctionByName] = JIT_MODE_CALL,
	[OP_AddImm] = JIT_MODE_INLINE,       /* Arithmetic */
	[OP_Cast] = JIT_MODE_UNSUPPORTED,
	[OP_Array] = JIT_MODE_CALL,
	[OP_Map] = JIT_MODE_CALL,
	[OP_Getitem] = JIT_MODE_CALL,
	[OP_Permutation] = JIT_MODE_UNSUPPORTED,
	[OP_Compare] = JIT_MODE_CALL,
	[OP_If] = JIT_MODE_UNSUPPORTED,      /* Control flow */
	[OP_Column] = JIT_MODE_UNSUPPORTED,
	[OP_FetchByName] = JIT_MODE_CALL,
	[OP_Fetch] = JIT_MODE_CALL,
	[OP_ApplyType] = JIT_MODE_UNSUPPORTED,
	[OP_MakeRecord] = JIT_MODE_UNSUPPORTED,
	[OP_Count] = JIT_MODE_CALL,
	[OP_CreateForeignKey] = JIT_MODE_UNSUPPORTED,
	[OP_CreateCheck] = JIT_MODE_UNSUPPORTED,
	[OP_DropTupleForeignKey] = JIT_MODE_UNSUPPORTED,
	[OP_DropTupleCheck] = JIT_MODE_UNSUPPORTED,
	[OP_DropFieldForeignKey] = JIT_MODE_UNSUPPORTED,
	[OP_DropFieldCheck] = JIT_MODE_UNSUPPORTED,
	[OP_AddFuncDefault] = JIT_MODE_UNSUPPORTED,
	[OP_CheckViewReferences] = JIT_MODE_UNSUPPORTED,
	[OP_TransactionBegin] = JIT_MODE_UNSUPPORTED,
	[OP_TransactionCommit] = JIT_MODE_UNSUPPORTED,
	[OP_TransactionRollback] = JIT_MODE_UNSUPPORTED,
	[OP_TTransaction] = JIT_MODE_UNSUPPORTED,
	[OP_IteratorOpen] = JIT_MODE_CALL,
	[OP_OpenSpace] = JIT_MODE_CALL,
	[OP_OpenTEphemeral] = JIT_MODE_CALL,
	[OP_SorterOpen] = JIT_MODE_CALL,
	[OP_SequenceTest] = JIT_MODE_UNSUPPORTED,
	[OP_OpenPseudo] = JIT_MODE_CALL,
	[OP_Close] = JIT_MODE_CALL,
	[OP_SeekLE] = JIT_MODE_CALL,
	[OP_NoConflict] = JIT_MODE_CALL,
	[OP_NotFound] = JIT_MODE_CALL,
	[OP_Sequence] = JIT_MODE_CALL,
	[OP_NextSystemSpaceId] = JIT_MODE_CALL,
	[OP_NextIdEphemeral] = JIT_MODE_CALL,
	[OP_FCopy] = JIT_MODE_INLINE,        /* Register copy */
	[OP_Delete] = JIT_MODE_CALL,
	[OP_ResetCount] = JIT_MODE_CALL,
	[OP_SorterCompare] = JIT_MODE_CALL,
	[OP_SorterData] = JIT_MODE_CALL,
	[OP_RowData] = JIT_MODE_UNSUPPORTED,
	[OP_NullRow] = JIT_MODE_CALL,
	[OP_SorterInsert] = JIT_MODE_CALL,
	[OP_IdxReplace] = JIT_MODE_CALL,
	[OP_IdxInsert] = JIT_MODE_CALL,
	[OP_Update] = JIT_MODE_CALL,
	[OP_SInsert] = JIT_MODE_CALL,
	[OP_SDelete] = JIT_MODE_CALL,
	[OP_IdxDelete] = JIT_MODE_CALL,
	[OP_IdxLE] = JIT_MODE_CALL,
	[OP_IdxGT] = JIT_MODE_CALL,
	[OP_Real] = JIT_MODE_INLINE,         /* Constant load */
	[OP_Decimal] = JIT_MODE_INLINE,      /* Constant load */
	[OP_IdxLT] = JIT_MODE_CALL,
	[OP_Clear] = JIT_MODE_CALL,
	[OP_ResetSorter] = JIT_MODE_CALL,
	[OP_RenameTable] = JIT_MODE_UNSUPPORTED,
	[OP_LoadAnalysis] = JIT_MODE_UNSUPPORTED,
	[OP_Param] = JIT_MODE_UNSUPPORTED,
	[OP_OffsetLimit] = JIT_MODE_INLINE,  /* Arithmetic */
	[OP_AggStep] = JIT_MODE_CALL,
	[OP_AggFinal] = JIT_MODE_CALL,
	[OP_Expire] = JIT_MODE_UNSUPPORTED,
	[OP_GenSpaceid] = JIT_MODE_CALL,
	[OP_SetSession] = JIT_MODE_UNSUPPORTED,
	[OP_ShowCreateTable] = JIT_MODE_CALL,
	[OP_Noop] = JIT_MODE_INLINE,         /* No-op can be inlined */
	[OP_Explain] = JIT_MODE_UNSUPPORTED,
	[OP_IsNull] = JIT_MODE_UNSUPPORTED,
	[OP_NotNull] = JIT_MODE_UNSUPPORTED,
};

/*
 * Mapping from opcode number to handler function name in bitcode.
 * NULL means the opcode has no handler in the bitcode files.
 * Function names match the symbols in the compiled .bc files exactly.
 */
static const char *opcode_handler_names[] = {
	[OP_Or] = "vdbe_op_or",
	[OP_And] = "vdbe_op_and",
	[OP_Not] = "vdbe_op_not",
	[OP_Ne] = "vdbe_op_ne",
	[OP_Eq] = "vdbe_op_eq",
	[OP_Gt] = "vdbe_op_gt",
	[OP_Le] = "vdbe_op_le",
	[OP_Lt] = "vdbe_op_lt",
	[OP_Ge] = "vdbe_op_ge",
	[OP_BitAnd] = "vdbe_op_bitand",
	[OP_BitOr] = "vdbe_op_bitor",
	[OP_Add] = "vdbe_op_add",
	[OP_Subtract] = "vdbe_op_sub",
	[OP_Multiply] = "vdbe_op_multiply",
	[OP_Divide] = "vdbe_op_divide",
	[OP_Remainder] = "vdbe_op_remainder",
	[OP_Concat] = "vdbe_op_concat",
	[OP_BitNot] = "vdbe_op_bitnot",
	[OP_Integer] = "vdbe_op_integer",
	[OP_Bool] = "vdbe_op_bool",
	[OP_Int64] = "vdbe_op_int64",
	[OP_String] = "vdbe_op_string",
	[OP_Null] = "vdbe_op_null",
	[OP_Blob] = "vdbe_op_blob",
	[OP_Variable] = "vdbe_op_variable",
	[OP_Move] = "vdbe_op_move",
	[OP_Copy] = "vdbe_op_copy",
	[OP_SCopy] = "vdbe_op_scopy",
	[OP_ResultRow] = "vdbe_op_resultrow",
	[OP_Cast] = "vdbe_op_cast",
	[OP_Column] = "vdbe_op_column",
	[OP_ApplyType] = "vdbe_op_applytype",
	[OP_MakeRecord] = "vdbe_op_makerecord",
	[OP_RowData] = "vdbe_op_rowdata",
	[OP_Real] = "vdbe_op_real",
	[OP_Noop] = "vdbe_op_noop",
};

/** Maximum opcode value for the handler name table. */
#define OPCODE_HANDLER_MAX \
	(int)(sizeof(opcode_handler_names) / sizeof(opcode_handler_names[0]))

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
	/** Number of modules that were successfully loaded */
	int modules_loaded;
};

static struct sql_jit_state jit_state = {0};

/*
 * List of handler bitcode files to load at initialization.
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

enum jit_handler_module {
	JIT_MODULE_ARITH = 0,
	JIT_MODULE_COMPARE,
	JIT_MODULE_LOGICAL,
	JIT_MODULE_DATA,
	JIT_MODULE_CURSOR_DATA,
	JIT_MODULE_INDEX,
	JIT_MODULE_STRING,
	JIT_MODULE_TYPE,
};

/**
 * Load a single bitcode file into an LLVM module.
 *
 * @param path     Full path to the .bc file
 * @param[out] mod Pointer to store the loaded module
 * @return 0 on success, -1 on error
 */
static int
jit_load_bitcode_file(const char *path, LLVMModuleRef *mod)
{
	LLVMMemoryBufferRef mem_buf = NULL;
	char *error_msg = NULL;

	if (LLVMCreateMemoryBufferWithContentsOfFile(path, &mem_buf,
						     &error_msg) != 0) {
		say_warn("JIT: failed to read bitcode file %s: %s",
			 path, error_msg ? error_msg : "unknown error");
		if (error_msg != NULL)
			LLVMDisposeMessage(error_msg);
		return -1;
	}

	if (LLVMParseBitcode2(mem_buf, mod) != 0) {
		say_warn("JIT: failed to parse bitcode file %s", path);
		LLVMDisposeMemoryBuffer(mem_buf);
		return -1;
	}

	/*
	 * LLVMParseBitcode2 takes ownership of the memory buffer,
	 * so we do not dispose it on success.
	 */
	return 0;
}

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
	 * Load bitcode files from the build directory.
	 * SQL_JIT_BITCODE_DIR is set by CMake at compile time.
	 */
#ifndef SQL_JIT_BITCODE_DIR
#define SQL_JIT_BITCODE_DIR "."
#endif
	int loaded = 0;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s",
			 SQL_JIT_BITCODE_DIR, handler_bitcode_files[i]);
		if (jit_load_bitcode_file(path,
					  &jit_state.handler_modules[i]) == 0) {
			loaded++;
			say_info("JIT: loaded bitcode module %s",
				 handler_bitcode_files[i]);
		} else {
			jit_state.handler_modules[i] = NULL;
		}
	}
	jit_state.modules_loaded = loaded;

	if (loaded == 0) {
		say_warn("JIT: no bitcode modules loaded, "
			 "JIT compilation will be limited");
	} else {
		say_info("JIT: loaded %d/%d bitcode modules from %s",
			 loaded, count, SQL_JIT_BITCODE_DIR);
	}

	char *error_msg = NULL;
	LLVMLinkInMCJIT();

	/* Create a dummy module for the execution engine */
	LLVMModuleRef dummy_module =
		LLVMModuleCreateWithName("vdbe_jit_engine");
	if (LLVMCreateExecutionEngineForModule(&jit_state.engine,
					       dummy_module,
					       &error_msg) != 0) {
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
 * Clone a handler bitcode module for linking into a JIT module.
 *
 * We clone rather than link the original because LLVMLinkModules2
 * takes ownership (destroys) the source module. We need the originals
 * to remain available for future compilations.
 *
 * @param src_index  Index into jit_state.handler_modules
 * @return Cloned module, or NULL on error. Caller owns the result.
 */
static LLVMModuleRef
jit_clone_handler_module(int src_index)
{
	if (src_index < 0 || src_index >= jit_state.module_count)
		return NULL;
	LLVMModuleRef src = jit_state.handler_modules[src_index];
	if (src == NULL)
		return NULL;
	return LLVMCloneModule(src);
}

/**
 * Find the handler function name for a given opcode.
 *
 * @param opcode  The VDBE opcode number
 * @return Handler function name, or NULL if no handler in bitcode
 */
static const char *
jit_handler_name_for_opcode(int opcode)
{
	if (opcode < 0 || opcode >= OPCODE_HANDLER_MAX)
		return NULL;
	return opcode_handler_names[opcode];
}

static int
jit_module_index_for_opcode(int opcode)
{
	switch (opcode) {
	case OP_Add:
	case OP_Subtract:
	case OP_Multiply:
	case OP_Divide:
	case OP_Remainder:
	case OP_Noop:
		return JIT_MODULE_ARITH;
	case OP_Ne:
	case OP_Eq:
	case OP_Gt:
	case OP_Le:
	case OP_Lt:
	case OP_Ge:
		return JIT_MODULE_COMPARE;
	case OP_Or:
	case OP_And:
	case OP_Not:
	case OP_BitAnd:
	case OP_BitOr:
	case OP_BitNot:
		return JIT_MODULE_LOGICAL;
	case OP_Integer:
	case OP_Bool:
	case OP_Int64:
	case OP_Real:
	case OP_String:
	case OP_Null:
	case OP_Blob:
	case OP_Variable:
	case OP_Move:
	case OP_Copy:
	case OP_SCopy:
		return JIT_MODULE_DATA;
	case OP_ResultRow:
	case OP_Column:
	case OP_RowData:
		return JIT_MODULE_CURSOR_DATA;
	case OP_Concat:
		return JIT_MODULE_STRING;
	case OP_Cast:
	case OP_ApplyType:
	case OP_MakeRecord:
		return JIT_MODULE_TYPE;
	default:
		return -1;
	}
}

/**
 * Emit LLVM IR to load a pointer field from a struct at a given
 * byte offset. Returns an i8* value.
 *
 * Equivalent to: *(void **)((char *)base_ptr + byte_offset)
 *
 * @param builder  LLVM IR builder positioned in a basic block
 * @param base_ptr LLVM value of type i8* pointing to the struct
 * @param offset   Byte offset of the pointer field
 * @param name     Name for the loaded value
 * @return LLVM value of type i8* with the loaded pointer
 */
static LLVMValueRef
jit_load_ptr_field(LLVMBuilderRef builder, LLVMValueRef base_ptr,
		   int offset, const char *name)
{
	LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
	/* GEP to advance base_ptr by offset bytes */
	LLVMValueRef offset_val =
		LLVMConstInt(LLVMInt32Type(), (unsigned)offset, 0);
	LLVMValueRef field_addr =
		LLVMBuildGEP(builder, base_ptr, &offset_val, 1, "field_addr");
	/* Cast to pointer-to-pointer so we can load the contained pointer */
	LLVMTypeRef ptr_ptr_type = LLVMPointerType(ptr_type, 0);
	LLVMValueRef field_ptr =
		LLVMBuildBitCast(builder, field_addr, ptr_ptr_type,
				 "field_ptr");
	return LLVMBuildLoad(builder, field_ptr, name);
}

/**
 * Emit LLVM IR to compute &base[index] given a base pointer and
 * element size. Returns an i8* value.
 *
 * Equivalent to: (char *)base + index * elem_size
 *
 * @param builder   LLVM IR builder
 * @param base_ptr  LLVM value of type i8*
 * @param index     Element index (i32 constant)
 * @param elem_size Size of each element in bytes
 * @param name      Name for the result
 * @return LLVM value of type i8* pointing to the element
 */
static LLVMValueRef
jit_array_element_ptr(LLVMBuilderRef builder, LLVMValueRef base_ptr,
		      int index, int elem_size, const char *name)
{
	LLVMValueRef byte_offset =
		LLVMConstInt(LLVMInt32Type(),
			     (unsigned)(index * elem_size), 0);
	return LLVMBuildGEP(builder, base_ptr, &byte_offset, 1, name);
}

/**
 * Emit LLVM IR to store an int field in struct Vdbe through an opaque i8 *.
 *
 * Equivalent to: *(int *)((char *)base_ptr + offset) = value
 *
 * @param builder   LLVM IR builder
 * @param base_ptr  LLVM value of type i8*
 * @param offset    Byte offset of the int field
 * @param value     Integer value to store
 */
static void
jit_store_int_field(LLVMBuilderRef builder, LLVMValueRef base_ptr,
		       int offset, int value)
{
	LLVMValueRef offset_val =
		LLVMConstInt(LLVMInt32Type(), (unsigned)offset, 0);
	LLVMValueRef field_addr =
		LLVMBuildGEP(builder, base_ptr, &offset_val, 1, "field_addr");
	LLVMTypeRef int_ptr_type = LLVMPointerType(LLVMInt32Type(), 0);
	LLVMValueRef field_ptr =
		LLVMBuildBitCast(builder, field_addr, int_ptr_type,
				 "field_ptr");
	LLVMBuildStore(builder,
		       LLVMConstInt(LLVMInt32Type(), (unsigned)value, 0),
		       field_ptr);
}

/**
 * Link all handler bitcode modules into the JIT module.
 *
 * Clones each loaded handler module and links it into the destination
 * module. After linking, all handler functions are available by name
 * in the destination module.
 *
 * @param dest  Destination module to link handlers into
 * @return 0 on success, -1 on error
 */
static int
jit_link_handler_modules(LLVMModuleRef dest, const bool *needed_modules)
{
	for (int i = 0; i < jit_state.module_count; i++) {
		if (!needed_modules[i] || jit_state.handler_modules[i] == NULL)
			continue;
		LLVMModuleRef clone = jit_clone_handler_module(i);
		if (clone == NULL) {
			say_warn("JIT: failed to clone handler module %s",
				 handler_bitcode_files[i]);
			continue;
		}

		/*
		 * LLVMLinkModules2 destroys the source module on
		 * success and returns 0. On failure it returns
		 * non-zero and destroys the source module anyway.
		 */
		if (LLVMLinkModules2(dest, clone) != 0) {
			say_warn("JIT: failed to link handler module %s",
				 handler_bitcode_files[i]);
			return -1;
		}
	}
	return 0;
}

/**
 * Compile a VDBE program into native code using JIT.
 *
 * This function analyzes the VDBE program, identifies opcodes suitable for
 * inlining, links handler bitcode modules, generates LLVM IR that dispatches
 * to handler functions, and compiles to native code.
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
	 * Phase 2: Analyze VDBE program opcodes.
	 *
	 * Scan through the program and check if it's worth JIT compiling.
	 * If the program contains only unsupported opcodes or calls,
	 * there's no benefit from JIT compilation.
	 */
	int inline_count = 0;
	int call_count = 0;
	int unsupported_count = 0;

	for (int i = 0; i < p->nOp; i++) {
		VdbeOp *pOp = &p->aOp[i];
		int opcode = pOp->opcode;

		/* Validate opcode is within range */
		if (opcode < 0 ||
		    opcode >= (int)(sizeof(opcode_jit_modes) /
				    sizeof(opcode_jit_modes[0]))) {
			unsupported_count++;
			continue;
		}

		enum vdbe_jit_mode mode = opcode_jit_modes[opcode];
		if (mode == JIT_MODE_INLINE)
			inline_count++;
		else if (mode == JIT_MODE_CALL)
			call_count++;
		else
			unsupported_count++;
	}

	/*
	 * Skip JIT compilation if there are no inlinable operations.
	 * Pure I/O programs or control-flow heavy programs won't benefit.
	 */
	if (inline_count == 0) {
		p->jit_compiled = 0;
		p->jit_func = NULL;
		p->jit_module = NULL;
		return 0;
	}

	/* Create a new module for this VDBE program */
	char module_name[64];
	snprintf(module_name, sizeof(module_name), "vdbe_jit_%p", (void *)p);
	LLVMModuleRef module = LLVMModuleCreateWithName(module_name);
	if (module == NULL) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			 "Failed to create LLVM module for JIT compilation");
		return -1;
	}

	/*
	 * Phase 3: Link handler bitcode modules into the JIT module.
	 *
	 * This makes all handler functions available in the module so
	 * they can be referenced by the JIT-generated dispatch code.
	 */
	if (jit_state.modules_loaded > 0) {
		bool needed_modules[jit_state.module_count];
		memset(needed_modules, 0, sizeof(needed_modules));
		for (int i = 0; i < p->nOp; i++) {
			int opcode = p->aOp[i].opcode;
			const char *handler_name =
				jit_handler_name_for_opcode(opcode);
			if (handler_name == NULL ||
			    opcode_jit_modes[opcode] == JIT_MODE_UNSUPPORTED)
				continue;
			int module_idx = jit_module_index_for_opcode(opcode);
			if (module_idx >= 0 && module_idx < jit_state.module_count)
				needed_modules[module_idx] = true;
		}
		if (jit_link_handler_modules(module, needed_modules) != 0) {
			say_warn("JIT: handler module linking failed, "
				 "falling back to stub");
			LLVMDisposeModule(module);
			module = LLVMModuleCreateWithName(module_name);
		}
	}

	LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8Type(), 0);

	/*
	 * Create the JIT entry function:
	 *   int vdbe_jit_exec(struct Vdbe *p, int start_pc)
	 */
	LLVMTypeRef jit_param_types[2] = { ptr_type, LLVMInt32Type() };
	LLVMTypeRef jit_func_type =
		LLVMFunctionType(LLVMInt32Type(), jit_param_types, 2, 0);

	char func_name[64];
	snprintf(func_name, sizeof(func_name), "vdbe_jit_exec_%p", (void *)p);
	LLVMValueRef jit_func = LLVMAddFunction(module, func_name,
						 jit_func_type);
	LLVMSetFunctionCallConv(jit_func, LLVMCCallConv);

	LLVMValueRef param_vdbe = LLVMGetParam(jit_func, 0);
	LLVMValueRef param_start_pc = LLVMGetParam(jit_func, 1);

	/*
	 * Build basic blocks: entry, one per opcode, and exit.
	 *
	 * The entry block switches on start_pc to jump to the
	 * correct opcode block. Each opcode block either:
	 * - Calls the handler and falls through to the next block
	 * - Returns current PC for unsupported opcodes (interpreter fallback)
	 */
	LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlock(jit_func, "entry");
	LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlock(jit_func, "exit");

	/* Create per-opcode basic blocks */
	LLVMBasicBlockRef *op_blocks =
		calloc(p->nOp, sizeof(LLVMBasicBlockRef));
	if (op_blocks == NULL) {
		diag_set(OutOfMemory, p->nOp * sizeof(LLVMBasicBlockRef),
			 "calloc", "op_blocks");
		LLVMDisposeModule(module);
		return -1;
	}

	for (int i = 0; i < p->nOp; i++) {
		char bb_name[32];
		snprintf(bb_name, sizeof(bb_name), "op_%d", i);
		op_blocks[i] = LLVMAppendBasicBlock(jit_func, bb_name);
	}

	LLVMBuilderRef builder = LLVMCreateBuilder();

	/* Build entry block: switch on start_pc */
	LLVMPositionBuilderAtEnd(builder, entry_bb);
	LLVMValueRef sw = LLVMBuildSwitch(builder, param_start_pc,
					   op_blocks[0], p->nOp);
	for (int i = 0; i < p->nOp; i++) {
		LLVMAddCase(sw, LLVMConstInt(LLVMInt32Type(), i, 0),
			     op_blocks[i]);
	}

	/* Build exit block: return -1 (execution complete) */
	LLVMPositionBuilderAtEnd(builder, exit_bb);
	LLVMBuildRet(builder, LLVMConstInt(LLVMInt32Type(), (uint64_t)-1, 1));

	/*
	 * Build opcode blocks.
	 *
	 * For each opcode we check its JIT mode:
	 * - INLINE/CALL with handler: call the handler function,
	 *   check return value, fall through to next opcode on success.
	 * - UNSUPPORTED or no handler: return current PC to the
	 *   interpreter for fallback execution.
	 *
	 * Handler return convention:
	 *   0  = success, continue to next opcode
	 *   1  = jump to P2
	 *  -1  = error occurred
	 */
	for (int i = 0; i < p->nOp; i++) {
		LLVMPositionBuilderAtEnd(builder, op_blocks[i]);
		jit_store_int_field(builder, param_vdbe, VDBE_OFFSET_PC, i);

		VdbeOp *pOp = &p->aOp[i];
		int opcode = pOp->opcode;
		enum vdbe_jit_mode mode = JIT_MODE_UNSUPPORTED;

		if (opcode >= 0 &&
		    opcode < (int)(sizeof(opcode_jit_modes) /
				   sizeof(opcode_jit_modes[0])))
			mode = opcode_jit_modes[opcode];

		if (opcode == OP_Init) {
			LLVMTypeRef prep_arg_types[1] = {
				LLVMPointerType(LLVMInt8Type(), 0)
			};
			LLVMTypeRef prep_type =
				LLVMFunctionType(LLVMInt32Type(),
						 prep_arg_types, 1, 0);
			LLVMValueRef prep_fn =
				LLVMGetNamedFunction(module, "sql_vdbe_prepare");
			if (prep_fn == NULL)
				prep_fn = LLVMAddFunction(module,
							  "sql_vdbe_prepare",
							  prep_type);
			LLVMValueRef prep_args[1] = {param_vdbe};
			LLVMValueRef prep_rc =
				LLVMBuildCall(builder, prep_fn, prep_args, 1,
					      "init_rc");
			LLVMValueRef prep_failed =
				LLVMBuildICmp(builder, LLVMIntNE, prep_rc,
					      LLVMConstInt(LLVMInt32Type(), 0, 0),
					      "prep_failed");
			LLVMBasicBlockRef init_err_bb =
				LLVMAppendBasicBlock(jit_func, "init_err");
			LLVMBasicBlockRef init_ok_bb =
				LLVMAppendBasicBlock(jit_func, "init_ok");
			LLVMBuildCondBr(builder, prep_failed, init_err_bb,
					init_ok_bb);
			LLVMPositionBuilderAtEnd(builder, init_err_bb);
			LLVMBuildRet(builder, prep_rc);
			LLVMPositionBuilderAtEnd(builder, init_ok_bb);
			if (pOp->p2 >= 0 && pOp->p2 < p->nOp)
				LLVMBuildBr(builder, op_blocks[pOp->p2]);
			else
				LLVMBuildRet(builder,
					     LLVMConstInt(LLVMInt32Type(), i, 0));
			continue;
		}

		if (opcode == OP_Goto) {
			if (pOp->p2 >= 0 && pOp->p2 < p->nOp)
				LLVMBuildBr(builder, op_blocks[pOp->p2]);
			else
				LLVMBuildRet(builder,
					     LLVMConstInt(LLVMInt32Type(), i, 0));
			continue;
		}

		const char *handler_name = jit_handler_name_for_opcode(opcode);

		if (mode == JIT_MODE_UNSUPPORTED || handler_name == NULL) {
			/*
			 * Return current PC so the interpreter can
			 * handle this opcode and continue.
			 */
			LLVMBuildRet(builder,
				     LLVMConstInt(LLVMInt32Type(), i, 0));
			continue;
		}

		/*
		 * Look up handler function in the linked module.
		 * If not found (bitcode not loaded), fall back to
		 * returning the PC.
		 */
		LLVMValueRef handler_fn =
			LLVMGetNamedFunction(module, handler_name);
		if (handler_fn == NULL) {
			/*
			 * Handler not available in linked bitcode.
			 * Return PC for interpreter fallback.
			 */
			LLVMBuildRet(builder,
				     LLVMConstInt(LLVMInt32Type(), i, 0));
			continue;
		}

		/*
		 * Compute pointers to p->aOp[i] and p->aMem.
		 *
		 * We use offsetof() constants (computed at C compile
		 * time) to navigate the Vdbe struct through opaque
		 * i8* pointers.
		 *
		 * aOp_ptr = *(Op **)((char *)p + VDBE_OFFSET_AOP)
		 * pOp_ptr = (char *)aOp_ptr + i * sizeof(Op)
		 * aMem_ptr = *(Mem **)((char *)p + VDBE_OFFSET_AMEM)
		 */
		LLVMValueRef aOp_ptr =
			jit_load_ptr_field(builder, param_vdbe,
					   VDBE_OFFSET_AOP, "aOp");
		LLVMValueRef pOp_ptr =
			jit_array_element_ptr(builder, aOp_ptr, i,
					      VDBE_SIZEOF_OP, "pOp");
		LLVMValueRef aMem_ptr =
			jit_load_ptr_field(builder, param_vdbe,
					   VDBE_OFFSET_AMEM, "aMem");
		LLVMTypeRef handler_type =
			LLVMGetElementType(LLVMTypeOf(handler_fn));
		assert(LLVMCountParamTypes(handler_type) == 3);
		LLVMTypeRef handler_param_types[3];
		LLVMGetParamTypes(handler_type, handler_param_types);
		LLVMValueRef call_args[3] = {
			LLVMBuildBitCast(builder, param_vdbe,
					 handler_param_types[0], "vdbe_arg"),
			LLVMBuildBitCast(builder, pOp_ptr,
					 handler_param_types[1], "pOp_arg"),
			LLVMBuildBitCast(builder, aMem_ptr,
					 handler_param_types[2], "aMem_arg"),
		};
		LLVMValueRef ret_val =
			LLVMBuildCall(builder, handler_fn, call_args, 3,
				      "handler_rc");

		/*
		 * Check handler return value:
		 *   rc < 0  → error, return rc
		 *   rc == 1 → jump to P2
		 *   rc == 0 → continue to next opcode
		 */
		LLVMValueRef is_error =
			LLVMBuildICmp(builder, LLVMIntSLT, ret_val,
				      LLVMConstInt(LLVMInt32Type(), 0, 0),
				      "is_error");
		LLVMValueRef is_jump =
			LLVMBuildICmp(builder, LLVMIntEQ, ret_val,
				      LLVMConstInt(LLVMInt32Type(), 1, 0),
				      "is_jump");

		LLVMBasicBlockRef next_bb;
		if (i + 1 < p->nOp)
			next_bb = op_blocks[i + 1];
		else
			next_bb = exit_bb;
		LLVMBasicBlockRef jump_bb = NULL;
		if (pOp->p2 >= 0 && pOp->p2 < p->nOp)
			jump_bb = op_blocks[pOp->p2];
		else
			jump_bb = next_bb;

		char err_bb_name[32];
		snprintf(err_bb_name, sizeof(err_bb_name), "err_%d", i);
		LLVMBasicBlockRef err_bb =
			LLVMAppendBasicBlock(jit_func, err_bb_name);
		char cont_bb_name[32];
		snprintf(cont_bb_name, sizeof(cont_bb_name), "cont_%d", i);
		LLVMBasicBlockRef cont_bb =
			LLVMAppendBasicBlock(jit_func, cont_bb_name);

		LLVMBuildCondBr(builder, is_error, err_bb, cont_bb);

		LLVMPositionBuilderAtEnd(builder, cont_bb);
		LLVMBuildCondBr(builder, is_jump, jump_bb, next_bb);

		/* Error block: return the error code */
		LLVMPositionBuilderAtEnd(builder, err_bb);
		LLVMBuildRet(builder, ret_val);
	}

	LLVMDisposeBuilder(builder);

	/* Verify the module */
	char *error_msg = NULL;
	if (LLVMVerifyModule(module, LLVMReturnStatusAction,
			     &error_msg) != 0) {
		say_error("JIT: module verification failed: %s",
			  error_msg ? error_msg : "unknown");
		if (error_msg != NULL)
			LLVMDisposeMessage(error_msg);
		LLVMDisposeModule(module);
		free(op_blocks);
		return -1;
	}
	if (error_msg != NULL)
		LLVMDisposeMessage(error_msg);

	/*
	 * Phase 5: Apply LLVM optimization passes.
	 *
	 * Run a standard set of optimizations to inline handler
	 * functions, propagate constants, and eliminate dead code.
	 * This is critical for performance: without inlining, each
	 * handler call has function-call overhead.
	 */
	LLVMPassManagerRef fpm = LLVMCreateFunctionPassManagerForModule(module);
	/* Promote allocas to registers */
	LLVMAddPromoteMemoryToRegisterPass(fpm);
	/* Basic scalar optimizations */
	LLVMAddInstructionCombiningPass(fpm);
	/* Reassociate expressions for better constant folding */
	LLVMAddReassociatePass(fpm);
	/* Eliminate common subexpressions */
	LLVMAddGVNPass(fpm);
	/* Simplify the control flow graph */
	LLVMAddCFGSimplificationPass(fpm);

	LLVMInitializeFunctionPassManager(fpm);
	/* Run per-function passes on all functions in the module */
	LLVMValueRef fn = LLVMGetFirstFunction(module);
	while (fn != NULL) {
		if (!LLVMIsDeclaration(fn))
			LLVMRunFunctionPassManager(fpm, fn);
		fn = LLVMGetNextFunction(fn);
	}
	LLVMFinalizeFunctionPassManager(fpm);
	LLVMDisposePassManager(fpm);

	/* Module-level passes: inlining and global DCE */
	LLVMPassManagerRef mpm = LLVMCreatePassManager();
	/* Inline small functions (handler bodies) */
	LLVMAddFunctionInliningPass(mpm);
	/* Remove dead globals after inlining */
	LLVMAddGlobalDCEPass(mpm);
	/* Run scalar opts again after inlining */
	LLVMAddInstructionCombiningPass(mpm);
	LLVMAddCFGSimplificationPass(mpm);
	LLVMRunPassManager(mpm, module);
	LLVMDisposePassManager(mpm);

	/* Add module to execution engine and compile */
	LLVMAddModule(jit_state.engine, module);

	uint64_t func_addr =
		LLVMGetFunctionAddress(jit_state.engine, func_name);
	if (func_addr == 0) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			 "Failed to get JIT function address");
		free(op_blocks);
		return -1;
	}

	/* Store JIT compilation results */
	p->jit_func = (void *)(uintptr_t)func_addr;
	p->jit_module = module;
	p->jit_compiled = 1;
	say_debug("JIT: compiled VDBE %p (%d ops: %d inline, %d call, %d unsupported)",
		  (void *)p, p->nOp, inline_count, call_count,
		  unsupported_count);

	free(op_blocks);
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
	 * Note: jit_module is owned by the execution engine after
	 * LLVMAddModule, so we don't dispose it here.
	 */
	if (p->jit_module != NULL)
		p->jit_module = NULL;
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
			if (jit_state.handler_modules[i] != NULL)
				LLVMDisposeModule(jit_state.handler_modules[i]);
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
	jit_state.modules_loaded = 0;
}

/**
 * Check if JIT compilation is available and enabled.
 *
 * @return 1 if JIT is available and enabled in configuration, 0 otherwise
 */
int
vdbe_jit_is_enabled(void)
{
	char env[16];
	const char *value = getenv_safe("SQL_JIT_ENABLE", env, sizeof(env));
	if (value == NULL)
		return 0;
	static const char *const true_values[] = {
		"1", "true", "yes", "on", NULL
	};
	static const char *const false_values[] = {
		"0", "false", "no", "off", NULL
	};
	if (strindex(true_values, value, lengthof(true_values)) !=
	    lengthof(true_values))
		return 1;
	if (strindex(false_values, value, lengthof(false_values)) !=
	    lengthof(false_values))
		return 0;
	say_warn("Ignoring SQL_JIT_ENABLE=%s: expected one of "
		 "0/1/false/true/no/yes/off/on", value);
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
