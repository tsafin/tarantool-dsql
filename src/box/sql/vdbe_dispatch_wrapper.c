/*
 * VDBE Dispatcher Wrapper
 * Phase 5.3 - Provides unified interface for old and generated dispatchers
 *
 * This file implements wrapper functions that allow both the old inline
 * dispatcher and the generated dispatcher to be called through a common interface.
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_dispatch.h"
#include "vdbe_dispatch_interface.h"

/*
 * Old dispatcher wrapper
 *
 * Phase 5.3.2: Implementation
 * This wrapper calls the existing sqlVdbeExec() function which contains
 * the inline dispatcher loop. The parameters are provided for interface
 * consistency, though they're redundant since they're stored in the Vdbe
 * structure (p->aOp and p->aMem).
 *
 * This design allows both old and generated dispatchers to be called through
 * the same interface, enabling parallel validation testing in Phase 5.3.4.
 */
int
vdbe_exec_old_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem)
{
	/* Verify the provided parameters match what's in the Vdbe structure */
	assert(p != NULL);
	assert(aOp == p->aOp);
	assert(aMem == p->aMem);

	/* Call the main execution function which contains the inline dispatcher */
	return sqlVdbeExec(p);
}

/*
 * Generated dispatcher wrapper - callable version
 *
 * Phase 5.3.3: Temporary implementation
 *
 * For now, this simply delegates to the old dispatcher (sqlVdbeExec).
 * This unblocks Phase 5.3.4 (parallel validation testing).
 *
 * Phase 5.3.3.2 (future): Implement actual callable generated dispatcher
 * - Will refactor vdbe_dispatch_generated.c to be loop-based instead of goto-based
 * - Will handle control flow with return codes instead of labels
 * - Will integrate extracted handler functions
 *
 * For parallel validation in Phase 5.3.4, we need both dispatchers to be
 * callable. The old dispatcher is trivial (just calls sqlVdbeExec), and this
 * temporary implementation does the same. The generated dispatcher will be
 * fully implemented in a follow-up phase once we've validated the approach.
 */
int
vdbe_exec_generated_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem)
{
	/* Temporary: call old dispatcher
	 * This allows Phase 5.3.4 to test parallel validation with both
	 * dispatchers pointing to the same implementation.
	 *
	 * TODO Phase 5.3.3.2: Implement actual callable generated dispatcher
	 * - Generate dispatch loop that doesn't use goto labels
	 * - Handle all 176 opcodes through extracted handlers
	 * - Support both inline and extracted opcode handlers
	 * - Validate with test suite before enabling as default
	 */
	assert(p != NULL);
	assert(aOp == p->aOp);
	assert(aMem == p->aMem);

	/* For now, use the old dispatcher */
	return sqlVdbeExec(p);
}
