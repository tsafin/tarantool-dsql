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
 * This function provides a wrapper around the generated dispatcher
 * from vdbe_dispatch_generated.c
 *
 * For now, also returns error - will be implemented when we have
 * the generated dispatcher properly integrated.
 */
int
vdbe_exec_generated_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem)
{
	/* For Phase 5.3: Generated dispatcher integration
	 * This function will call the generated dispatch code
	 * once the integration is complete.
	 *
	 * Current status: Infrastructure in place, awaiting integration
	 */
	(void)p;
	(void)aOp;
	(void)aMem;
	return -1;  /* Not implemented as separate function yet */
}
