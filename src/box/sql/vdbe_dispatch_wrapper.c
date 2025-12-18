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
 * Generated dispatcher wrapper
 *
 * Phase 5.3.3: Integration point for generated dispatcher
 *
 * The generated dispatcher from vdbe_dispatch_generated.c currently uses
 * inline labels and goto-based control flow (abort_due_to_error, JUMP_P2, etc.)
 * which prevents it from being a standalone callable function.
 *
 * To implement this wrapper, we need to either:
 * 1. Refactor generated code to use return codes instead of goto labels
 * 2. Create a separate version of sqlVdbeExec that uses generated dispatcher
 * 3. Generate the dispatcher as a self-contained callable function
 *
 * Current approach: Placeholder that validates the interface
 * This allows infrastructure testing while generated dispatcher refactoring
 * is planned for a follow-up phase.
 */
int
vdbe_exec_generated_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem)
{
	/* For now, return error - indicates generated dispatcher not yet callable
	 * as a standalone function.
	 *
	 * TODO Phase 5.3.3: Implement callable generated dispatcher
	 * - Refactor generated dispatcher to convert label-based control flow
	 *   to return codes
	 * - Or, generate dispatcher as self-contained function from opcodes.yaml
	 * - Ensure interface matches vdbe_exec_old_dispatcher signature
	 * - Verify all 176 opcodes dispatch correctly
	 * - Test with parallel validation (Phase 5.3.4)
	 */
	assert(p != NULL);
	(void)aOp;
	(void)aMem;

	/* Return error - generated dispatcher not yet integrated */
	diag_set(ClientError, ER_SQL_EXECUTE,
	         "Generated dispatcher not yet integrated (Phase 5.3.3 pending)");
	return -1;
}
