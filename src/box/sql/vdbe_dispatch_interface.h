/*
 * VDBE Dispatcher Interface
 * Phase 5.3 - Defines the interface for dispatcher implementations
 *
 * This header provides a common interface that both the old and generated
 * dispatchers must implement. It allows for runtime or compile-time selection
 * of dispatcher implementations.
 */

#ifndef VDBE_DISPATCH_INTERFACE_H
#define VDBE_DISPATCH_INTERFACE_H

#include "vdbeInt.h"

/*
 * Dispatcher function type
 *
 * Executes VDBE program starting from program counter pc.
 * The dispatcher will execute opcodes until it returns control.
 *
 * Parameters:
 *   p - VDBE execution context
 *   aOp - Array of operations
 *   aMem - Array of registers/memory cells
 *   pc - Initial program counter (0-based, will be set to p->pc)
 *
 * Returns:
 *   0 - Normal completion (SQLITE_OK)
 *   -1 - Error occurred
 *   SQL_ROW - A result row was generated
 *   SQL_DONE - Execution complete
 *   Other status values as needed
 */
typedef int (*VdbeDispatcher)(struct Vdbe *p, VdbeOp *aOp, Mem *aMem);

/*
 * Old dispatcher - inline implementation from vdbe.c
 * Uses the original switch-based or computed-goto dispatch
 */
int vdbe_exec_old_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem);

/*
 * Generated dispatcher - generated from opcodes.yaml
 * Uses generated dispatch table with extracted opcode handlers
 */
int vdbe_exec_generated_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem);

/*
 * Parallel validation dispatcher - Phase 5.3.4
 * Runs both dispatchers and compares results
 * Validates <2% performance regression and identical behavior
 */
int vdbe_exec_parallel_validation(struct Vdbe *p, VdbeOp *aOp, Mem *aMem);

/*
 * Dispatcher selection mode (Phase 5.3.4)
 * Controls which dispatcher is used and how validation is performed
 */
typedef enum {
	VDBE_DISPATCH_AUTO = 0,         /* Use compiled default */
	VDBE_DISPATCH_OLD = 1,          /* Use old dispatcher */
	VDBE_DISPATCH_GENERATED = 2,    /* Use generated dispatcher */
	VDBE_DISPATCH_PARALLEL = 3,     /* Run both and compare (validation mode) */
} VdbeDispatchMode;

/*
 * Get the active dispatcher mode
 * Can be overridden by environment variables for testing
 * VDBE_DISPATCHER=old|generated|parallel|auto
 */
VdbeDispatchMode vdbe_get_dispatcher_mode(void);

/*
 * Set the dispatcher mode (for testing)
 * Returns 0 on success, -1 on invalid mode
 */
int vdbe_set_dispatcher_mode(VdbeDispatchMode mode);

/*
 * Get the active dispatcher function
 * Returns the appropriate dispatcher based on configuration
 * Phase 5.3.4: Supports parallel validation mode
 *
 * Note: For now, always use compile-time dispatcher selection
 * to avoid potential initialization issues with getenv() calls.
 * Runtime mode selection via vdbe_set_dispatcher_mode() is available
 * for testing, but the default dispatcher is determined at compile time.
 */
static inline VdbeDispatcher
vdbe_get_dispatcher(void)
{
	/* Use compile-time default for safety
	 * Future: Call vdbe_get_dispatcher_mode() for runtime selection
	 */
#ifdef VDBE_USE_GENERATED_DISPATCH
	return vdbe_exec_generated_dispatcher;
#else
	return vdbe_exec_old_dispatcher;
#endif
}

#endif /* VDBE_DISPATCH_INTERFACE_H */
