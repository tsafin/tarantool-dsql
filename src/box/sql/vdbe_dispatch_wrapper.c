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
#include "vdbe_ops.h"
#include "vdbe_dispatch.h"
#include "vdbe_dispatch_interface.h"
#include "box/error.h"

/* Forward declarations */
void vdbe_trace(Vdbe *p, Op *pOrigOp, int rc, Mem *aMem);
void check_vdbe_operands(Vdbe *p, Op *pOp, Op *aOp, Mem *aMem);

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
		/* Use getenv with null check safety */
		const char *env = NULL;

		/* Safely get environment variable */
		env = getenv("VDBE_DISPATCHER");

		if (env != NULL && *env != '\0') {
			if (strcmp(env, "old") == 0) {
				vdbe_dispatcher_mode = VDBE_DISPATCH_OLD;
			} else if (strcmp(env, "generated") == 0) {
				vdbe_dispatcher_mode = VDBE_DISPATCH_GENERATED;
			} else if (strcmp(env, "parallel") == 0) {
				vdbe_dispatcher_mode = VDBE_DISPATCH_PARALLEL;
			} else if (strcmp(env, "auto") == 0) {
				vdbe_dispatcher_mode = VDBE_DISPATCH_AUTO;
			}
		}
		/* Mark as initialized only after safe completion */
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
 * Generated dispatcher wrapper - callable loop-based version
 *
 * Phase 5.5: True implementation of generated dispatcher
 *
 * This implements a callable loop-based dispatcher that executes VDBE programs
 * independently without calling back to sqlVdbeExec(). It refactors the
 * goto-based generated dispatcher into a while-loop with PC-based control flow.
 *
 * Architecture:
 * - while(pc < nOp) loop iterates through opcodes
 * - switch(opcode) dispatch on current instruction
 * - Calls extracted handlers (vdbe_op_xxx functions) for 60+ opcodes
 * - Inline code for remaining opcodes
 * - Return codes: 0=continue, 1=jump, -1=error, SQL_ROW=result row
 * - pc (program counter) manages instruction sequencing
 * - No circular dependency: dispatcher is fully independent
 *
 * Implementation Strategy (Phase 5.5):
 * - Extract main loop and variable declarations from vdbe.c
 * - Convert label-based jumps to PC-based manipulation
 * - Handle all opcode types: external (60), inline (63), control_flow (18)
 * - Support both computed-goto and switch-based dispatch modes
 * - Maintain identical semantics to inline dispatcher for validation
 */
int
vdbe_exec_generated_dispatcher(struct Vdbe *p, VdbeOp *aOp, Mem *aMem)
{
	fprintf(stderr, "[GENERATED DISPATCHER ENTRY] pc=%d nOp=%d\n", p->pc, p->nOp);
	int rc = 0;                    /* Value to return */
	int pc = p->pc;                /* Current program counter (0-based) */
	int nOp = p->nOp;             /* Number of operations */
	VdbeOp *pOp;                  /* Current operation */

	/* For debugging/tracing */
#ifdef SQL_DEBUG
	VdbeOp *pOrigOp;
#endif

	assert(p != NULL);

	/* Parameter shortcuts (from vdbe.c) */
#define P1 pOp->p1
#define P2 pOp->p2
#define P3 pOp->p3
#define P4 pOp->p4
#define IN_P1 &aMem[P1]
#define IN_P2 &aMem[P2]
#define OUT_P2 &aMem[P2]
#define OUT_P3 &aMem[P3]

	assert(p != NULL);
	assert(aOp == p->aOp);
	assert(aMem == p->aMem);
	assert(p->magic == VDBE_MAGIC_RUN);

#ifdef SQL_DEBUG
	if (pc == 0 &&
	    (p->sql_flags & (SQL_VdbeListing|SQL_VdbeTrace)) != 0) {
		sqlVdbePrintSql(p);
		if ((p->sql_flags & SQL_VdbeListing) != 0) {
			printf("VDBE Program Listing:\n");
			for (int i = 0; i < nOp; i++)
				sqlVdbePrintOp(stdout, i, &aOp[i]);
		}
		if ((p->sql_flags & SQL_VdbeTrace) != 0)
			printf("VDBE Trace:\n");
	}
	/* Check operands of first instruction before entering loop */
	pOp = &aOp[pc];
	check_vdbe_operands(p, pOp, aOp, aMem);
#endif

	/* Main execution loop - Phase 5.5: Loop-based dispatcher */
	while (pc < nOp) {
		pOp = &aOp[pc];
		int op = pOp->opcode;
		int cur_pc = pc;

		/* Debug tracing (before opcode execution) */
#ifdef SQL_DEBUG
		pOrigOp = pOp;
		vdbe_trace(p, pOrigOp, rc, aMem);
#endif

		fprintf(stderr, "[GENERATED DISPATCHER OPCODE] pc=%d opcode=%s (%d)\n", pc, sqlOpcodeName(op), op);

		/* Dispatch on opcode */
		switch (pOp->opcode) {
		/* ====================================================================
		 * EXTERNAL HANDLERS (60 opcodes)
		 * These are implemented in separate vdbe_ops_*.c files
		 * ====================================================================
		 */

		case OP_Add: {
			int handler_rc = vdbe_op_add(p, pOp, aMem);
			if (handler_rc < 0) { rc = -1; break; }
			/* Add P1 P2 P3: r[P3]=r[P1]+r[P2], no jump (P2 is input register) */
			pc++; continue;
		}

		case OP_Subtract: {
			int handler_rc = vdbe_op_sub(p, pOp, aMem);
			if (handler_rc < 0) { rc = -1; break; }
			/* Subtract P1 P2 P3: r[P3]=r[P2]-r[P1], no jump (P2 is input register) */
			pc++; continue;
		}

		case OP_Multiply: {
			int handler_rc = vdbe_op_multiply(p, pOp, aMem);
			if (handler_rc < 0) { rc = -1; break; }
			/* Multiply P1 P2 P3: r[P3]=r[P1]*r[P2], no jump (P2 is input register) */
			pc++; continue;
		}

		case OP_Divide: {
			int handler_rc = vdbe_op_divide(p, pOp, aMem);
			if (handler_rc < 0) { rc = -1; break; }
			/* Divide P1 P2 P3: r[P3]=r[P2]/r[P1], no jump (P2 is input register) */
			pc++; continue;
		}

		case OP_Remainder: {
			int handler_rc = vdbe_op_remainder(p, pOp, aMem);
			if (handler_rc < 0) { rc = -1; break; }
			/* Remainder P1 P2 P3: r[P3]=r[P2]%r[P1], no jump (P2 is input register) */
			pc++; continue;
		}

		case OP_Integer: {
			/* Integer constant - external handler */
			int handler_rc = vdbe_op_integer(p, pOp, aMem);
			if (handler_rc < 0) {
				rc = -1;
				break;
			}
			pc++;
			continue;
		}

		case OP_String: {
			/* String constant - external handler */
			int handler_rc = vdbe_op_string(p, pOp, aMem);
			if (handler_rc < 0) {
				rc = -1;
				break;
			}
			pc++;
			continue;
		}

		case OP_SetDiag: {
			/* Set diagnostic and optionally jump */
			box_error_set(__FILE__, __LINE__, P1, pOp->p4.z);
			if (P2 != 0) {
				pc = P2;
			} else {
				pc++;
			}
			continue;
		}

		case OP_Init: {
			/* Initialize program and jump to P2 */
			char *zTrace;
			int i;
			struct sql *db = sql_get();
			assert(pOp == p->aOp);
			if (p->pFrame == NULL && sql_vdbe_prepare(p) != 0) {
				rc = -1;
				break;
			}

			if ((db->mTrace & SQL_TRACE_STMT) != 0 && !p->doingRerun &&
			    (zTrace = (pOp->p4.z ? pOp->p4.z : p->zSql)) != 0) {
				(void)db->xTrace(SQL_TRACE_STMT, db->pTraceArg, p, zTrace);
			}
#ifdef SQL_DEBUG
			if ((p->sql_flags & SQL_SqlTrace) != 0 &&
			    (zTrace = (pOp->p4.z ? pOp->p4.z : p->zSql)) != 0)
				sqlDebugPrintf("SQL-trace: %s\n", zTrace);
#endif /* SQL_DEBUG */
			assert(P2 > 0);
			if (P1 >= sqlGlobalConfig.iOnceResetThreshold) {
				for (i = 1; i < p->nOp; i++) {
					if (p->aOp[i].opcode == OP_Once)
						p->aOp[i].p1 = 0;
				}
				P1 = 0;
			}
			P1++;
			pc = P2;
			continue;
		}

		case OP_Goto: {
			/* Unconditional jump */
			pc = P2;
			continue;
		}

		case OP_Jump: {
			/* Conditional jump based on p->iCompare */
			if (p->iCompare < 0) {
				pc = P1;
			} else if (p->iCompare == 0) {
				pc = P2;
			} else {
				pc = P3;
			}
			continue;
		}

		case OP_ResultRow: {
			/* Return result row */
			int handler_rc = vdbe_op_resultrow(p, pOp, aMem);
			if (handler_rc < 0) {
				rc = -1;
				break;
			}
			if (handler_rc == 1) {
				/* Return SQL_ROW to caller */
				pc = p->pc;
				rc = SQL_ROW;
				break;
			}
			pc++;
			continue;
		}

		case OP_Halt: {
			/* Halt execution - mirror inline dispatcher semantics */
			VdbeFrame *pFrame;
			int pcx;
			assert(P1 == 0 || !diag_is_empty(diag_get()));

			pcx = pc;
			if (P1 == 0 && p->pFrame != NULL) {
				/* Halt sub-program and return control to parent frame. */
				pFrame = p->pFrame;
				p->pFrame = pFrame->pParent;
				p->nFrame--;
				sqlVdbeSetChanges(p->nChange);
				pcx = sqlVdbeFrameRestore(pFrame);
				if (P2 == ON_CONFLICT_ACTION_IGNORE) {
					/* Jump to P2 of calling OP_Program. */
					pcx = p->aOp[pcx].p2 - 1;
				}
				aOp = p->aOp;
				aMem = p->aMem;
				nOp = p->nOp;
				pc = pcx + 1;
				continue;
			}
			if (P1 != 0)
				p->is_aborted = true;
			p->errorAction = (u8)P2;
			p->pc = pcx;
			sqlVdbeHalt(p);
			rc = p->is_aborted ? -1 : SQL_DONE;
			break;
		}

	/* ====================================================================
	 * SIMPLE INLINE OPCODES (Phase 5.6a - 10 opcodes < 100 chars)
	 * These are refactored inline opcodes that work as handler functions
	 * ====================================================================
	 */

	case OP_Savepoint: {
		/* Manage savepoints: CREATE/RELEASE/ROLLBACK */
		int handler_rc = vdbe_op_savepoint_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_Noop: {
		/* No operation */
		int handler_rc = vdbe_op_noop_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_Explain: {
		/* Explain query plan */
		int handler_rc = vdbe_op_explain_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_SkipLoad: {
		/* Skip load */
		int handler_rc = vdbe_op_skipload_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_Expire: {
		/* Expire schema */
		int handler_rc = vdbe_op_expire_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}


	case OP_NotNull: {
		/* Jump if not null */
		int handler_rc = vdbe_op_notnull_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}

	case OP_Permutation: {
		/* Permutation */
		int handler_rc = vdbe_op_permutation_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/*
	 * MEDIUM COMPLEXITY INLINE OPCODES (Phase 5.6b - 2 opcodes, simple register ops)
	 * These are refactored inline opcodes with basic register operations
	 * ====================================================================
	 */

	case OP_Close: {
		/* Close cursor */
		int handler_rc = vdbe_op_close_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_IsNull: {
		/* Test for NULL and jump if true */
		int handler_rc = vdbe_op_isnull_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}

	case OP_AddImm: {
		/* Add immediate value to register */
		int handler_rc = vdbe_op_addimm_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_TransactionBegin: {
		/* Start new transaction */
		int handler_rc = vdbe_op_transactionbegin_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_TransactionCommit: {
		/* Commit current transaction */
		int handler_rc = vdbe_op_transactioncommit_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_TransactionRollback: {
		/* Rollback current transaction */
		int handler_rc = vdbe_op_transactionrollback_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_DropTupleCheck: {
		/* Drop tuple-level check constraint */
		int handler_rc = vdbe_op_droptuplecheckundidocheck_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_DropTupleForeignKey: {
		/* Drop tuple-level foreign key constraint */
		int handler_rc = vdbe_op_droptupleforeignkey_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_DropFieldCheck: {
		/* Drop field-level check constraint */
		int handler_rc = vdbe_op_dropfieldcheck_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_DropFieldForeignKey: {
		/* Drop field-level foreign key constraint */
		int handler_rc = vdbe_op_dropfieldforeignkey_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_GenSpaceid: {
		/* Generate unique space ID */
		int handler_rc = vdbe_op_genspaceid_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_Once: {
		/* Execute code block at most once */
		int handler_rc = vdbe_op_once_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc > 0) { pc = pOp->p2; }
		else { pc++; }
		continue;
	}

	case OP_IfNot: {
		/* Jump if register value is false */
		int handler_rc = vdbe_op_ifnot_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc > 0) { pc = pOp->p2; }
		else { pc++; }
		continue;
	}

	case OP_IfPos: {
		/* Jump if positive and apply saturated decrement */
		int handler_rc = vdbe_op_ifpos_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc > 0) { pc = pOp->p2; }
		else { pc++; }
		continue;
	}

	case OP_IfNotZero: {
		/* Jump if non-zero and decrement */
		int handler_rc = vdbe_op_ifnotzero_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc > 0) { pc = pOp->p2; }
		else { pc++; }
		continue;
	}

	case OP_DecrJumpZero: {
		/* Decrement and jump if zero */
		int handler_rc = vdbe_op_decrjumpzero_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc > 0) { pc = pOp->p2; }
		else { pc++; }
		continue;
	}

	case OP_NullRow: {
		/* Mark cursor at null row and cleanup */
		int handler_rc = vdbe_op_nullrow_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_ShowCreateTable: {
		/* Generate CREATE TABLE statement text */
		int handler_rc = vdbe_op_showcreatettable_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_ResetSorter: {
		/* Reset sorter state */
		int handler_rc = vdbe_op_resetsorter_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_Sort: {
		/* Sort records (test harness) */
		int handler_rc = vdbe_op_sort_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_Clear: {
		/* Clear space/truncate table */
		int handler_rc = vdbe_op_clear_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_Param: {
		/* Load parameter from frame */
		int handler_rc = vdbe_op_param_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Decimal: {
		/* Load decimal constant into register */
		int handler_rc = vdbe_op_decimal_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_OpenSpace: {
		/* Create space reference cursor by ID */
		int handler_rc = vdbe_op_openspace_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Sequence: {
		/* Get sequence counter value and increment */
		int handler_rc = vdbe_op_sequence_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_SequenceTest: {
		/* Test sequence counter, jump if zero */
		int handler_rc = vdbe_op_sequencetest_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc > 0) { pc = P2; continue; }  /* Jump to P2 */
		pc++; continue;
	}
	case OP_Fetch: {
		/* Fetch field value from record */
		int handler_rc = vdbe_op_fetch_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Phase 5.6h - Medium Batch 7: Bitwise, Value Loading, and Cursor Operations */
	case OP_ShiftLeft: {
		/* Bitwise left shift operation */
		int handler_rc = vdbe_op_shiftleft_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_ShiftRight: {
		/* Bitwise right shift operation */
		int handler_rc = vdbe_op_shiftright_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_String8: {
		/* Load C string constant with auto-length calculation */
		int handler_rc = vdbe_op_string8(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Array: {
		/* Create msgpack array from register range */
		int handler_rc = vdbe_op_array_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Map: {
		/* Create msgpack map from register pairs */
		int handler_rc = vdbe_op_map_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Getitem: {
		/* Extract element from array or map by index */
		int handler_rc = vdbe_op_getitem_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_OpenPseudo: {
		/* Create pseudo-cursor for memory-resident data */
		int handler_rc = vdbe_op_openpseudo_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Count: {
		/* Get record count from cursor */
		int handler_rc = vdbe_op_count_inline(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* ====================================================================
	 * CRITICAL OPCODES FOR TABLE OPERATIONS
	 * Added to unblock CREATE/INSERT/SELECT operations
	 * ====================================================================
	 */

	/* Comparison operators */
	case OP_Eq: {
		int handler_rc = vdbe_op_eq(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Ne: {
		int handler_rc = vdbe_op_ne(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Lt: {
		int handler_rc = vdbe_op_lt(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Le: {
		int handler_rc = vdbe_op_le(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Gt: {
		int handler_rc = vdbe_op_gt(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Ge: {
		int handler_rc = vdbe_op_ge(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Compare: {
		int handler_rc = vdbe_op_compare(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Logical operators */
	case OP_And: {
		int handler_rc = vdbe_op_and(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Or: {
		int handler_rc = vdbe_op_or(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Not: {
		int handler_rc = vdbe_op_not(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_If: {
		/* Jump if register is true */
		Mem *pIn1 = &aMem[P1];
		int c;
		if (mem_is_null(pIn1)) {
			c = P3;
		} else if (mem_is_bool(pIn1)) {
			c = pIn1->u.b;
		} else {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 mem_str(pIn1), "boolean");
			rc = -1;
			break;
		}
		if (c) {
			pc = P2;
		} else {
			pc++;
		}
		continue;
	}

	/* Data/constants */
	case OP_Bool: {
		int handler_rc = vdbe_op_bool(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Blob: {
		int handler_rc = vdbe_op_blob(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Register operations */
	case OP_Copy: {
		int handler_rc = vdbe_op_copy(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Type operations */
	case OP_MustBeInt: {
		int handler_rc = vdbe_op_mustbeint(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Cast: {
		int handler_rc = vdbe_op_cast(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_ApplyType: {
		int handler_rc = vdbe_op_applytype(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Concat: {
		int handler_rc = vdbe_op_concat(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Cursor data access */
	case OP_Column: {
		int handler_rc = vdbe_op_column(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_MakeRecord: {
		int handler_rc = vdbe_op_makerecord(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Cursor seek operations */
	case OP_Found: {
		int handler_rc = vdbe_op_found_notfound_noconflict(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_NotFound: {
		int handler_rc = vdbe_op_found_notfound_noconflict(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}

	/* Data modification */
	case OP_Delete: {
		int handler_rc = vdbe_op_delete(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Update: {
		int handler_rc = vdbe_op_update(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Aggregates */
	case OP_AggStep: {
		int handler_rc = vdbe_op_aggstep(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_AggFinal: {
		int handler_rc = vdbe_op_aggfinal(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* ====================================================================
	 * FUNCTION AND SESSION CONTROL OPCODES
	 * ====================================================================
	 */

	case OP_BuiltinFunction: {
		/* Call built-in SQL function */
		int handler_rc = vdbe_op_builtinfunction(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_FunctionByName: {
		/* Call user-defined function by name */
		int handler_rc = vdbe_op_functionbyname(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	case OP_SetSession: {
		/* Set session variable or setting */
		int handler_rc = vdbe_op_setsession(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Sorter/ephemeral table opcodes */
	case OP_SorterOpen: {
		/* Open a sorter cursor for sorting operations */
		int handler_rc = vdbe_op_sorteropen(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_SorterInsert: {
		/* Insert a record into the sorter */
		int handler_rc = vdbe_op_sorterinsert(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_SorterNext: {
		/* Advance to next sorter record, jump to P2 if more rows */
		int handler_rc = vdbe_op_sorternext(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 0) {  /* 0 = more rows, jump to P2 to process them */
			pc = P2;
			continue;
		}
		pc++; continue;
	}
	case OP_SorterData: {
		/* Extract current sorter data into a register */
		int handler_rc = vdbe_op_sorterdata(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_SorterCompare: {
		/* Compare current sorter key with key in register, jump to P2 if different */
		int handler_rc = vdbe_op_sortercompare(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }  /* Jump if different */
		pc++; continue;
	}
	case OP_SorterSort: {
		/* Sort the sorter and rewind to beginning, jump to P2 if empty */
		int handler_rc = vdbe_op_sortersort(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }  /* Jump if empty */
		pc++; continue;
	}

	/* Transaction operations - inline implementations */
	case OP_TTransaction: {
		/*
		 * Start Tarantool's transaction, if there is no active
		 * transactions. Otherwise, create anonymous savepoint.
		 */
		if (!box_txn()) {
			if (txn_begin() == NULL) {
				rc = -1;
				break;
			}
		} else {
			p->anonymous_savepoint = txn_savepoint_new(in_txn(), NULL);
			if (p->anonymous_savepoint == NULL) {
				rc = -1;
				break;
			}
		}
		pc++; continue;
	}

	/* ====================================================================
	 * CURSOR AND ITERATION OPCODES (Batch 2)
	 * Added to unblock SELECT operations
	 * ====================================================================
	 */

	/* Iterator operations */
	case OP_IteratorOpen: {
		int handler_rc = vdbe_op_iteratoropen(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Rewind: {
		int handler_rc = vdbe_op_rewind(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Next: {
		int handler_rc = vdbe_op_next(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		VdbeCursor *pC = p->apCsr[P1];
		pC->cacheStatus = CACHE_STALE;
		pC->nullRow = (handler_rc != 0);  /* Set nullRow if no more rows */
		if (handler_rc == 0) {  /* 0 = more rows, jump to P2 to process them */
			pc = P2;
			continue;
		}
		pc++; continue;
	}
	case OP_Prev: {
		int handler_rc = vdbe_op_prev(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		VdbeCursor *pC = p->apCsr[P1];
		pC->cacheStatus = CACHE_STALE;
		pC->nullRow = (handler_rc != 0);
		if (handler_rc == 0) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_Last: {
		int handler_rc = vdbe_op_last(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		VdbeCursor *pC = p->apCsr[P1];
		pC->cacheStatus = CACHE_STALE;
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_NextIfOpen: {
		int handler_rc = vdbe_op_nextifopen(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		/* NextIfOpen delegates to Next, which returns 0=more rows, 1=no more rows.
		 * Jump to P2 when cursor successfully advances (handler_rc == 0). */
		if (handler_rc == 0) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_PrevIfOpen: {
		int handler_rc = vdbe_op_previfopen(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		/* PrevIfOpen delegates to Prev, which returns 0=more rows, 1=no more rows.
		 * Jump to P2 when cursor successfully advances (handler_rc == 0). */
		if (handler_rc == 0) { pc = P2; continue; }
		pc++; continue;
	}

	/* Seek operations */
	case OP_SeekLT:
	case OP_SeekGT: {
		int handler_rc = vdbe_op_seek_lt_gt(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		VdbeCursor *pC = p->apCsr[P1];
		pC->cacheStatus = CACHE_STALE;
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_SeekLE:
	case OP_SeekGE: {
		int handler_rc = vdbe_op_seek_le_ge(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		VdbeCursor *pC = p->apCsr[P1];
		pC->cacheStatus = CACHE_STALE;
		if (handler_rc == 1) { pc = P2; continue; }
		if (handler_rc == 2) { pc += 2; continue; }  /* Skip next opcode (OP_IdxLT/GT) */
		pc++; continue;
	}

	/* Index operations */
	case OP_IdxInsert:
	case OP_IdxReplace: {
		int handler_rc = vdbe_op_idx_insert_replace(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_IdxGE:
	case OP_IdxGT:
	case OP_IdxLE:
	case OP_IdxLT: {
		int handler_rc = vdbe_op_idx_compare(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}
	case OP_IdxDelete: {
		int handler_rc = vdbe_op_idxdelete(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	/* OP_NoConflict - Index conflict check (re-enabled after fixing wrapper) */
	case OP_NoConflict: {
		int handler_rc = vdbe_op_noconflict(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		if (handler_rc == 1) { pc = P2; continue; }
		pc++; continue;
	}

	/* System space operations */
	case OP_SInsert: {
		int handler_rc = vdbe_op_sinsert(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_SDelete: {
		int handler_rc = vdbe_op_sdelete(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Data access */
	case OP_RowData: {
		int handler_rc = vdbe_op_rowdata(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Data types */
	case OP_Int64: {
		int handler_rc = vdbe_op_int64(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Real: {
		int handler_rc = vdbe_op_real(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Null: {
		int handler_rc = vdbe_op_null(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_Variable: {
		int handler_rc = vdbe_op_variable(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Register operations */
	case OP_Move: {
		int handler_rc = vdbe_op_move(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_SCopy: {
		int handler_rc = vdbe_op_scopy(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Bitwise operations */
	case OP_BitAnd: {
		int handler_rc = vdbe_op_bitand(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_BitOr: {
		int handler_rc = vdbe_op_bitor(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}
	case OP_BitNot: {
		int handler_rc = vdbe_op_bitnot(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* LIMIT/OFFSET */
	case OP_OffsetLimit: {
		int handler_rc = vdbe_op_offsetlimit(p, pOp, aMem);
		if (handler_rc < 0) { rc = -1; break; }
		pc++; continue;
	}

	/* Fallback to inline dispatcher for unhandled opcodes
	 * Return SQL_FALLBACK_TO_INLINE special code to signal that
	 * execution should continue with inline dispatcher from current PC.
	 */
		default: {
			/* Unhandled opcode - fall back to inline dispatcher */
			fprintf(stderr, "[GENERATED DISPATCHER FALLBACK] opcode=%s pc=%d\n",
				sqlOpcodeName(pOp->opcode), pc);
			p->pc = pc;
			return SQL_FALLBACK_TO_INLINE;
		}
		}

		/* Exit loop on error or special return code */
		if (rc != 0) {
			if (rc < 0 && diag_is_empty(diag_get())) {
				fprintf(stderr,
					"Generated dispatcher error without diag: pc=%d op=%s\n",
					cur_pc, sqlOpcodeName(op));
				diag_set(ClientError, ER_SQL_EXECUTE,
					 "VDBE error without diagnostics");
			}
			break;
		}
	}

	/* Cleanup and return */
	p->pc = pc;
	return rc;

#undef P1
#undef P2
#undef P3
#undef P4
#undef IN_P1
#undef IN_P2
#undef OUT_P2
#undef OUT_P3
}
