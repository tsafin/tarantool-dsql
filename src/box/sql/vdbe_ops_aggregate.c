/* Aggregate function opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"

/* Opcode: AggStep P1 P2 P3 P4 *
 * Synopsis: accum=r[P3] step(r[P2@P1])
 *
 * Execute the step function for an aggregate.  The
 * function has P1 arguments.   P4 is a pointer to an sql_context
 * object that is used to run the function.  Register P3 is
 * as the accumulator.
 *
 * The P1 arguments are taken from register P2 and its
 * successors.
 */
int vdbe_op_aggstep(Vdbe *p, Op *pOp, Mem *aMem)
{
	int argc = pOp->p1;
	sql_context *pCtx;
	Mem *pMem;

	assert(pOp->p4type == P4_FUNCCTX);
	pCtx = pOp->p4.pCtx;
	pMem = &aMem[pOp->p3];

	if (pCtx->pOut != pMem)
		pCtx->pOut = pMem;

#ifdef SQL_DEBUG
	for(int i = 0; i < argc; i++) {
		assert(memIsValid(&aMem[pOp->p2 + i]));
		REGISTER_TRACE(p, pOp->p2 + i, &aMem[pOp->p2 + i]);
	}
#endif

	pCtx->skipFlag = 0;
	assert(pCtx->func->def->language == FUNC_LANGUAGE_SQL_BUILTIN);
	struct func_sql_builtin *func = (struct func_sql_builtin *)pCtx->func;
	func->call(pCtx, argc, &aMem[pOp->p2]);
	if (pCtx->is_aborted)
		return -1;
	if (pCtx->skipFlag) {
		assert(pOp[-1].opcode == OP_SkipLoad);
		int i = pOp[-1].p1;
		if (i)	mem_set_bool(&aMem[i], true);
	}
	return 0;
}

/* Opcode: AggFinal P1 * * P4 *
 * Synopsis: accum=r[P1]
 *
 * Execute the finalizer function for an aggregate. P1 is the memory location
 * that is the accumulator for the aggregate. P4 is a pointer to the function.
 */
int vdbe_op_aggfinal(Vdbe *p, Op *pOp, Mem *aMem)
{
	assert(pOp->p1 > 0 && pOp->p1 <= (p->nMem + 1 - p->nCursor));
	struct func_sql_builtin *func = (struct func_sql_builtin *)pOp->p4.func;
	struct Mem *pIn1 = &aMem[pOp->p1];

	if (func->finalize != NULL && func->finalize(pIn1) != 0)
		return -1;
	UPDATE_MAX_BLOBSIZE(pIn1);
	if (sqlVdbeMemTooBig(pIn1) != 0) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob too big");
		return -1;
	}
	return 0;
}
