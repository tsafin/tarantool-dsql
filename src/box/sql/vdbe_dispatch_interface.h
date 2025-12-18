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
 * Get the active dispatcher function
 * Returns the appropriate dispatcher based on VDBE_USE_GENERATED_DISPATCH
 */
static inline VdbeDispatcher
vdbe_get_dispatcher(void)
{
#ifdef VDBE_USE_GENERATED_DISPATCH
	return vdbe_exec_generated_dispatcher;
#else
	return vdbe_exec_old_dispatcher;
#endif
}

#endif /* VDBE_DISPATCH_INTERFACE_H */
