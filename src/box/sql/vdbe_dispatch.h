/*
 * VDBE Dispatcher Selection and Validation Framework
 * Phase 5.3 - Parallel Dispatch Validation
 *
 * This header provides:
 * 1. Compile-time selection between old and generated dispatchers
 * 2. Validation infrastructure for comparing dispatcher outputs
 * 3. Performance measurement hooks
 */

#ifndef VDBE_DISPATCH_H
#define VDBE_DISPATCH_H

#include "vdbeInt.h"

/*
 * VDBE_USE_GENERATED_DISPATCH: Enable the Phase 5.2 generated dispatcher
 *
 * When enabled:
 * - Uses the new dispatcher from vdbe_dispatch_generated.c
 * - Calls extracted opcode handlers from vdbe_ops_*.c
 * - Can be tested in parallel with old dispatcher for validation
 *
 * When disabled (default):
 * - Uses original inline dispatcher from vdbe.c
 * - Original behavior and performance characteristics
 *
 * Currently DISABLED - Phase 5.3 integration still in progress
 * Uncomment to enable when dispatcher integration is complete:
 */
/* #define VDBE_USE_GENERATED_DISPATCH */

/*
 * VDBE_PARALLEL_VALIDATION: Run both dispatchers and compare results
 * Only works when both dispatchers are compiled in.
 * Enables detailed validation logging.
 *
 * WARNING: This makes execution MUCH slower - for debugging only
 */
/* #define VDBE_PARALLEL_VALIDATION */

/*
 * VDBE_VALIDATION_STATS: Collect statistics on dispatch validation
 * Tracks how many opcodes executed, matches, mismatches, etc.
 */
#define VDBE_VALIDATION_STATS

/*
 * Validation result codes used in parallel mode
 */
typedef enum {
	VDBE_RESULT_MATCH = 0,        /* Dispatchers produced identical results */
	VDBE_RESULT_MISMATCH = 1,     /* Results differ */
	VDBE_RESULT_ERROR = -1,       /* Validation error */
} VdbeValidationResult;

/*
 * Validation statistics structure
 */
typedef struct {
	uint64_t opcodes_executed;    /* Total opcodes executed */
	uint64_t validation_passes;   /* Matching results */
	uint64_t validation_fails;    /* Mismatching results */
	uint64_t validation_errors;   /* Validation errors */
	uint64_t total_cycles;        /* Total CPU cycles (if profiling) */
} VdbeValidationStats;

/*
 * Get current validation statistics
 */
extern VdbeValidationStats vdbe_validation_stats;

/*
 * Reset validation statistics
 */
void vdbe_reset_validation_stats(void);

/*
 * Log validation mismatch for debugging
 */
void vdbe_log_validation_mismatch(struct Vdbe *p, struct VdbeOp *pOp,
                                   int old_result, int new_result,
                                   const char *reason);

/*
 * Validate VDBE state consistency between dispatchers
 */
int vdbe_validate_state(struct Vdbe *p, struct VdbeOp *pOp,
                        int rc_original, int rc_generated);

#endif /* VDBE_DISPATCH_H */
