#ifndef TARANTOOL_SQL_STATS_SELECTIVITY_H
#define TARANTOOL_SQL_STATS_SELECTIVITY_H

#include "sql_stats_histogram.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sql_stats_mcv_sample {
	const void *value;
	size_t value_size;
	uint64_t count;
};

/* One non-NULL component of a multivariate MCV tuple. */
struct sql_stats_mcv_value {
	const void *value;
	size_t value_size;
};

/* A joint MCV entry for a fully specified conjunction of equalities. */
struct sql_stats_joint_mcv_sample {
	const struct sql_stats_mcv_value *values;
	size_t value_count;
	uint64_t count;
};

struct sql_stats_column_summary {
	double row_count;
	double null_fraction;
	double distinct_count;
	double confidence;
	uint64_t sample_nonnull_rows;
	const struct sql_stats_mcv_sample *mcv;
	size_t mcv_count;
	const struct sql_stats_histogram *histogram;
	sql_stats_value_compare_f compare;
	void *compare_context;
	/* A proven unique-key equality, supplied by the resolver. */
	bool equality_is_unique;
};

enum sql_stats_selectivity_source {
	SQL_STATS_SELECTIVITY_EXACT,
	SQL_STATS_SELECTIVITY_MCV,
	SQL_STATS_SELECTIVITY_HISTOGRAM,
	SQL_STATS_SELECTIVITY_INDEPENDENCE,
	SQL_STATS_SELECTIVITY_JOINT_NDV,
};

struct sql_stats_selectivity {
	double value;
	double confidence;
	enum sql_stats_selectivity_source source;
};

enum sql_stats_range_operator {
	SQL_STATS_RANGE_LT,
	SQL_STATS_RANGE_LE,
	SQL_STATS_RANGE_GT,
	SQL_STATS_RANGE_GE,
};

enum sql_stats_predicate_kind {
	SQL_STATS_PREDICATE_EQUALITY,
	SQL_STATS_PREDICATE_RANGE,
};

struct sql_stats_predicate {
	size_t column_index;
	enum sql_stats_predicate_kind kind;
	enum sql_stats_range_operator range_operator;
	const void *value;
	size_t value_size;
};

/* Estimate a single-column equality, including SQL NULL semantics. */
int
sql_stats_estimate_equality(const struct sql_stats_column_summary *summary,
			    const void *value, size_t value_size, bool is_null,
			    struct sql_stats_selectivity *result);

/* Estimate a single-column range using cumulative histogram buckets. */
int
sql_stats_estimate_range(const struct sql_stats_column_summary *summary,
			 const void *value, size_t value_size,
			 enum sql_stats_range_operator op,
			 struct sql_stats_selectivity *result);

/* Independence fallback for conjunctions; confidence is conservatively min. */
int
sql_stats_selectivity_and(const struct sql_stats_selectivity *terms,
			  size_t term_count,
			  struct sql_stats_selectivity *result);

/*
 * Estimate a conjunction of non-NULL equalities. A matching joint MCV is
 * authoritative; otherwise per-column estimates are combined by independence.
 * Joint sample counts are measured over sample_nonnull_rows observations.
 */
int
sql_stats_estimate_equality_conjunction(
	const struct sql_stats_column_summary *summaries,
	const struct sql_stats_mcv_value *predicates, size_t term_count,
	const struct sql_stats_joint_mcv_sample *joint_mcv,
	size_t joint_mcv_count, uint64_t sample_nonnull_rows,
	struct sql_stats_selectivity *result);

/*
 * As above, but use a compatible joint-NDV estimate for an equality tuple
 * absent from joint_mcv. joint_ndv must describe the same non-NULL sample as
 * sample_nonnull_rows; the residual tail is modeled uniformly across
 * joint_ndv - joint_mcv_count distinct tuples. This is a low-confidence tail
 * estimate, not a dependency model. Missing, invalid, or incompatible NDV
 * metadata preserves the independence fallback. Exact joint MCV matches keep
 * precedence. joint_ndv_confidence must be in [0, 1].
 */
int
sql_stats_estimate_equality_conjunction_with_joint_ndv(
	const struct sql_stats_column_summary *summaries,
	const struct sql_stats_mcv_value *predicates, size_t term_count,
	const struct sql_stats_joint_mcv_sample *joint_mcv,
	size_t joint_mcv_count, uint64_t sample_nonnull_rows,
	double joint_ndv, double joint_ndv_confidence,
	struct sql_stats_selectivity *result);

/*
 * Estimate a conjunction of equality and range predicates by column index.
 * Repeated same-column predicates are combined before fallback. A non-point
 * two-sided range needs an exhaustive joint sample; otherwise the function
 * rejects it because histograms expose no interval-mass primitive.
 * An exhaustive joint sample (all non-NULL joint observations represented
 * by distinct tuples) answers the conjunction exactly; otherwise unique
 * per-column constraints are combined conservatively. Unsupported intervals
 * are rejected rather than treated as independent terms.
 */
int
sql_stats_estimate_predicate_conjunction(
	const struct sql_stats_column_summary *summaries, size_t summary_count,
	const struct sql_stats_predicate *predicates, size_t predicate_count,
	const struct sql_stats_joint_mcv_sample *joint_mcv,
	size_t joint_mcv_count, uint64_t sample_nonnull_rows,
	struct sql_stats_selectivity *result);

#endif /* TARANTOOL_SQL_STATS_SELECTIVITY_H */
