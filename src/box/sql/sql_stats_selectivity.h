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

#endif /* TARANTOOL_SQL_STATS_SELECTIVITY_H */
