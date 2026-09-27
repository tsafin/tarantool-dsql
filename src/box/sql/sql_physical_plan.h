#ifndef TARANTOOL_SQL_PHYSICAL_PLAN_H
#define TARANTOOL_SQL_PHYSICAL_PLAN_H

#include <stddef.h>
#include <stdint.h>

#include "sql_plan_descriptor.h"

struct sql_logical_plan;
struct Select;

enum sql_physical_reject_reason {
	SQL_PHYSICAL_REJECT_NONE,
	SQL_PHYSICAL_REJECT_INVALID_LOGICAL_PLAN,
	SQL_PHYSICAL_REJECT_NO_ACCESS_PATH,
	SQL_PHYSICAL_REJECT_INVALID_CANDIDATE,
	SQL_PHYSICAL_REJECT_UNSUPPORTED_FILTER,
	SQL_PHYSICAL_REJECT_UNSUPPORTED_DESTINATION,
};

/* Candidate fields are supplied by a fixed/current statistics provider. */
struct sql_physical_candidate {
	struct sql_plan_access access;
	const struct sql_plan_expression *expressions;
	size_t expression_count;
	double startup_cost;
	double total_cost;
	double rows;
	double row_width;
	double confidence;
};

/* Estimates for the deliberately narrow, no-predicate table-scan producer.
 * They must come from the caller's statement-time stats view; zero confidence
 * is valid when only a conservative fallback estimate is available.
 */
struct sql_physical_table_scan_estimate {
	double startup_cost;
	double total_cost;
	double rows;
	double row_width;
	double confidence;
};

/*
 * Select a deterministic least-cost access candidate for a single-table
 * logical chain and produce its immutable physical descriptor. Expressions,
 * filters, projection, and finalize details are not normalized by this
 * prototype; those remain borrowed logical operators for M3 integration.
 */
struct sql_plan_descriptor *
sql_physical_plan_from_logical(const struct sql_logical_plan *logical,
			       const struct sql_physical_candidate *candidates,
			       size_t candidate_count,
			       enum sql_physical_reject_reason *reason);

/* Produce a descriptor only for resolved `SELECT column[, ...] FROM t` with
 * no filter/finalize clauses. This is a producer-side adapter, not routing or
 * executable lowering. */
struct sql_plan_descriptor *
sql_physical_table_scan_from_select(const struct Select *select,
				    const struct sql_physical_table_scan_estimate *estimate,
				    enum sql_physical_reject_reason *reason);

#endif
