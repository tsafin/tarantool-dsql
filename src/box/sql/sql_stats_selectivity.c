#include "sql_stats_selectivity.h"

#include <math.h>

static bool
valid_summary(const struct sql_stats_column_summary *s)
{
	if (s == NULL || !isfinite(s->row_count) || s->row_count < 0 ||
	    !isfinite(s->null_fraction) || s->null_fraction < 0 ||
	    s->null_fraction > 1 || !isfinite(s->distinct_count) ||
	    s->distinct_count < 0 || !isfinite(s->confidence) ||
	    s->confidence < 0 || s->confidence > 1 ||
	    (s->mcv_count != 0 && (s->mcv == NULL || s->compare == NULL)) ||
	    (s->mcv_count > s->distinct_count) ||
	    (s->row_count > 0 && s->null_fraction < 1 &&
	     s->distinct_count < 1) ||
	    (s->histogram != NULL &&
	     (s->compare == NULL || s->sample_nonnull_rows == 0)))
		return false;
	return true;
}

static double
clamp(double value)
{
	if (value < 0)
		return 0;
	if (value > 1)
		return 1;
	return value;
}

static void
set_result(struct sql_stats_selectivity *result, double value,
	   double confidence, enum sql_stats_selectivity_source source)
{
	result->value = clamp(value);
	result->confidence = clamp(confidence);
	result->source = source;
}

static bool
valid_mcv(const struct sql_stats_column_summary *s)
{
	uint64_t total = 0;
	for (size_t i = 0; i < s->mcv_count; i++) {
		if (s->mcv[i].count == 0 ||
		    (s->mcv[i].value == NULL && s->mcv[i].value_size != 0) ||
		    total > s->sample_nonnull_rows ||
		    s->mcv[i].count > s->sample_nonnull_rows - total)
			return false;
		for (size_t j = 0; j < i; j++) {
			if (s->compare(s->mcv[i].value, s->mcv[i].value_size,
				       s->mcv[j].value, s->mcv[j].value_size,
				       s->compare_context) == 0)
				return false;
		}
		total += s->mcv[i].count;
	}
	return true;
}

int
sql_stats_estimate_equality(const struct sql_stats_column_summary *s,
			    const void *value, size_t value_size, bool is_null,
			    struct sql_stats_selectivity *result)
{
	if (!valid_summary(s) || result == NULL ||
	    (!is_null && value == NULL && value_size != 0) ||
	    !valid_mcv(s))
		return -1;
	if (s->row_count == 0) {
		set_result(result, 0, 1, SQL_STATS_SELECTIVITY_EXACT);
		return 0;
	}
	if (is_null) {
		set_result(result, s->null_fraction, s->confidence,
			   SQL_STATS_SELECTIVITY_EXACT);
		return 0;
	}
	if (s->equality_is_unique) {
		set_result(result, 1 / s->row_count,
			   1, SQL_STATS_SELECTIVITY_EXACT);
		return 0;
	}
	uint64_t mcv_total = 0;
	for (size_t i = 0; i < s->mcv_count; i++) {
		const struct sql_stats_mcv_sample *entry = &s->mcv[i];
		mcv_total += entry->count;
		if (s->compare != NULL && s->compare(value, value_size,
							   entry->value,
							   entry->value_size,
							   s->compare_context) == 0) {
			double scaled = s->sample_nonnull_rows == 0 ? 0 :
				entry->count / (double)s->sample_nonnull_rows *
				(1 - s->null_fraction);
			set_result(result, scaled, s->confidence,
				   SQL_STATS_SELECTIVITY_MCV);
			return 0;
		}
	}
	/* Estimate the non-MCV residual uniformly over residual NDV. */
	double residual_ndv = s->distinct_count - s->mcv_count;
	if (residual_ndv < 1)
		residual_ndv = 1;
	double sample_nonnull = s->sample_nonnull_rows;
	double residual_sample = sample_nonnull - mcv_total;
	if (residual_sample < 0)
		residual_sample = 0;
	double estimate = sample_nonnull > 0 ?
		(1 - s->null_fraction) *
		(residual_sample / sample_nonnull) / residual_ndv :
		(1 - s->null_fraction) / residual_ndv;
	set_result(result, estimate, s->confidence * 0.5,
		   SQL_STATS_SELECTIVITY_INDEPENDENCE);
	return 0;
}

int
sql_stats_estimate_range(const struct sql_stats_column_summary *s,
			 const void *value, size_t value_size,
			 enum sql_stats_range_operator op,
			 struct sql_stats_selectivity *result)
{
	if (!valid_summary(s) || result == NULL || s->histogram == NULL ||
	    s->compare == NULL || (value == NULL && value_size != 0) ||
	    op < SQL_STATS_RANGE_LT || op > SQL_STATS_RANGE_GE)
		return -1;
	if (s->row_count == 0) {
		set_result(result, 0, 1, SQL_STATS_SELECTIVITY_EXACT);
		return 0;
	}
	size_t buckets = sql_stats_histogram_bucket_count(s->histogram);
	if (buckets == 0)
		return -1;
	struct sql_stats_histogram_bucket tail;
	if (sql_stats_histogram_get_bucket(s->histogram, buckets - 1,
					   &tail) != 0 ||
	    tail.cumulative_count != s->sample_nonnull_rows)
		return -1;
	uint64_t previous_count = 0;
	double fraction = 0;
	bool resolved = false;
	for (size_t i = 0; i < buckets; i++) {
		struct sql_stats_histogram_bucket bucket;
		if (sql_stats_histogram_get_bucket(s->histogram, i, &bucket) != 0)
			return -1;
		int cmp = s->compare(value, value_size, bucket.upper_bound,
				    bucket.upper_bound_size, s->compare_context);
		if (cmp <= 0) {
			if (cmp == 0 && (op == SQL_STATS_RANGE_LT ||
					 op == SQL_STATS_RANGE_GE)) {
				fraction = previous_count /
					(double)s->sample_nonnull_rows;
			} else if (cmp == 0) {
				fraction = bucket.cumulative_count /
					(double)s->sample_nonnull_rows;
			} else {
				fraction = (previous_count + bucket.cumulative_count) /
					(2.0 * s->sample_nonnull_rows);
			}
			resolved = true;
			break;
		}
		previous_count = bucket.cumulative_count;
	}
	if (!resolved)
		fraction = 1;
	fraction *= (1 - s->null_fraction);
	if (op == SQL_STATS_RANGE_LT || op == SQL_STATS_RANGE_LE) {
		set_result(result, fraction, s->confidence * 0.75,
			   SQL_STATS_SELECTIVITY_HISTOGRAM);
	} else {
		set_result(result, (1 - s->null_fraction) - fraction,
			   s->confidence * 0.75,
			   SQL_STATS_SELECTIVITY_HISTOGRAM);
	}
	return 0;
}

int
sql_stats_selectivity_and(const struct sql_stats_selectivity *terms,
			  size_t term_count,
			  struct sql_stats_selectivity *result)
{
	if (result == NULL || (term_count != 0 && terms == NULL))
		return -1;
	double value = 1;
	double confidence = 1;
	for (size_t i = 0; i < term_count; i++) {
		if (!isfinite(terms[i].value) || terms[i].value < 0 ||
		    terms[i].value > 1 || !isfinite(terms[i].confidence) ||
		    terms[i].confidence < 0 || terms[i].confidence > 1)
			return -1;
		value *= terms[i].value;
		if (terms[i].confidence < confidence)
			confidence = terms[i].confidence;
	}
	set_result(result, value, confidence,
		   SQL_STATS_SELECTIVITY_INDEPENDENCE);
	return 0;
}
