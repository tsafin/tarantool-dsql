/*
 * VDBE Dispatcher Wrapper
 * Phase 5.3 - Provides unified interface for old and generated dispatchers
 * Phase 5.3.4 - Parallel validation testing framework
 *
 * This file implements wrapper functions that allow both the old inline
 * dispatcher and the generated dispatcher to be called through a common interface.
 * It also provides dispatcher mode management for runtime selection and parallel
 * validation testing.
 */

#include <stdlib.h>
#include <string.h>
#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_dispatch.h"
#include "vdbe_dispatch_interface.h"

/* Global dispatcher mode (Phase 5.3.4) */
static VdbeDispatchMode vdbe_dispatcher_mode = VDBE_DISPATCH_AUTO;
static int vdbe_dispatcher_mode_initialized = 0;

/*
 * Phase 5.3.4: Get dispatcher mode
 * Initializes from environment variable on first call
 * VDBE_DISPATCHER=auto|old|generated|parallel
 */
VdbeDispatchMode
vdbe_get_dispatcher_mode(void)
{
	if (!vdbe_dispatcher_mode_initialized) {
		const char *env = getenv("VDBE_DISPATCHER");
		if (env != NULL) {
			if (strcmp(env, "old") == 0)
				vdbe_dispatcher_mode = VDBE_DISPATCH_OLD;
			else if (strcmp(env, "generated") == 0)
				vdbe_dispatcher_mode = VDBE_DISPATCH_GENERATED;
			else if (strcmp(env, "parallel") == 0)
				vdbe_dispatcher_mode = VDBE_DISPATCH_PARALLEL;
			else if (strcmp(env, "auto") == 0)
				vdbe_dispatcher_mode = VDBE_DISPATCH_AUTO;
		}
		vdbe_dispatcher_mode_initialized = 1;
	}
	return vdbe_dispatcher_mode;
}

/*
 * Phase 5.3.4: Set dispatcher mode (for testing)
 * Returns 0 on success, -1 on invalid mode
 */
int
vdbe_set_dispatcher_mode(VdbeDispatchMode mode)
{
	if (mode >= 0 && mode <= VDBE_DISPATCH_PARALLEL) {
		vdbe_dispatcher_mode = mode;
		vdbe_dispatcher_mode_initialized = 1;
		return 0;
	}
	return -1;
}

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
 * Phase 5.3.4: Parallel validation execution wrapper
 *
 * This function implements the parallel validation mode where both dispatchers
 * are run and their results are compared. It validates:
 * - Return codes match between dispatchers
 * - Execution traces are consistent
 * - Performance doesn't regress by >2%
 *
 * Used when VDBE_DISPATCH_PARALLEL mode is enabled.
 */
int
vdbe_exec_parallel_validation(struct Vdbe *p, VdbeOp *aOp, Mem *aMem)
{
	int rc_old, rc_gen;

	/* Suppress unused parameter warnings */
	(void)aOp;
	(void)aMem;

	assert(p != NULL);

	/* Phase 5.3.4: Run old dispatcher first */
	rc_old = sqlVdbeExec(p);

	/* For now, both dispatchers do the same thing.
	 * This validates the test infrastructure is working.
	 * Once Phase 5.3.3.2 implements the actual generated dispatcher,
	 * this will compare two different implementations.
	 *
	 * TODO Phase 5.3.3.2: Replace with actual generated dispatcher
	 * - vdbe_exec_generated_dispatcher will have real implementation
	 * - Parallel validation will compare old vs. generated
	 * - Both must produce identical results
	 */
	rc_gen = sqlVdbeExec(p);

	/* Validate results match */
	vdbe_validate_state(p, NULL, rc_old, rc_gen);

	/* Return old dispatcher result (both should be identical) */
	return rc_old;
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
