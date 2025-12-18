/*
 * VDBE Dispatcher Validation Infrastructure
 * Phase 5.3 - Parallel Dispatch Validation Support
 *
 * This file provides the validation framework for comparing
 * generated vs. original dispatchers.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_dispatch.h"

/* Global statistics for validation */
VdbeValidationStats vdbe_validation_stats = {
	.opcodes_executed = 0,
	.validation_passes = 0,
	.validation_fails = 0,
	.validation_errors = 0,
	.total_cycles = 0,
};

/*
 * Reset validation statistics for new test run
 */
void
vdbe_reset_validation_stats(void)
{
	memset(&vdbe_validation_stats, 0, sizeof(vdbe_validation_stats));
}

/*
 * Log validation mismatch for debugging
 * Used when parallel validation detects different results
 */
void
vdbe_log_validation_mismatch(struct Vdbe *p, struct VdbeOp *pOp,
                             int old_result, int new_result,
                             const char *reason)
{
	static FILE *log_file = NULL;

	/* Suppress unused parameter warnings (parameters kept for future use) */
	(void)p;
	(void)pOp;

	/* Open log file on first call */
	if (log_file == NULL) {
		log_file = fopen("/tmp/vdbe_validation.log", "w");
		if (log_file == NULL)
			return;
		fprintf(log_file, "VDBE Validation Log\n");
		fprintf(log_file, "===================\n\n");
	}

	fprintf(log_file,
		"MISMATCH at opcode\n"
		"  Old result: %d\n"
		"  New result: %d\n"
		"  Reason: %s\n\n",
		old_result,
		new_result,
		reason ? reason : "unknown");

	fflush(log_file);
}

/*
 * Print validation statistics summary
 */
void
vdbe_print_validation_stats(void)
{
	VdbeValidationStats *stats = &vdbe_validation_stats;

	if (stats->opcodes_executed == 0)
		return;

	fprintf(stderr,
		"\n=== VDBE Validation Statistics (Phase 5.3.4) ===\n"
		"Opcodes executed:    %"PRIu64"\n"
		"Validation passes:   %"PRIu64"\n"
		"Validation failures: %"PRIu64"\n"
		"Validation errors:   %"PRIu64"\n"
		"Success rate:        %.2f%%\n"
		"Performance overhead: <2%% target\n"
		"===================================\n",
		stats->opcodes_executed,
		stats->validation_passes,
		stats->validation_fails,
		stats->validation_errors,
		(stats->opcodes_executed > 0) ?
			(100.0 * stats->validation_passes / stats->opcodes_executed) : 0.0);

	/* Check for validation failures */
	if (stats->validation_fails > 0) {
		fprintf(stderr,
			"WARNING: Validation failures detected!\n"
			"Review /tmp/vdbe_validation.log for details.\n");
	}
}

/*
 * Validate a VDBE program state consistency
 * Compares key execution state variables for correctness
 * Used for debugging parallel dispatcher execution
 */
int
vdbe_validate_state(struct Vdbe *p, struct VdbeOp *pOp,
                    int rc_original, int rc_generated)
{
	/* For Phase 5.3: Just track execution statistics for now
	 * Full state comparison would require deeper analysis of
	 * register values, cursor states, transaction state, etc.
	 */

	vdbe_validation_stats.opcodes_executed++;

	/* Check return codes match */
	if (rc_original == rc_generated) {
		vdbe_validation_stats.validation_passes++;
		return VDBE_RESULT_MATCH;
	} else {
		vdbe_validation_stats.validation_fails++;
		vdbe_log_validation_mismatch(p, pOp, rc_original, rc_generated,
					     "Return code mismatch");
		return VDBE_RESULT_MISMATCH;
	}
}
