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
 * This currently just returns an error - the actual old dispatcher
 * remains inline in vdbe.c's sqlVdbeExec() function.
 * In the future, we can extract the old dispatcher loop here.
 */
int
vdbe_exec_old_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem)
{
	/* For Phase 5.3: Old dispatcher remains inline in vdbe.c
	 * This function exists for interface compatibility
	 * and future extraction of the old dispatcher.
	 *
	 * Current design: sqlVdbeExec() handles everything
	 * This wrapper is placeholder for architectural flexibility.
	 */
	(void)p;
	(void)aOp;
	(void)aMem;
	return -1;  /* Not implemented as separate function yet */
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
