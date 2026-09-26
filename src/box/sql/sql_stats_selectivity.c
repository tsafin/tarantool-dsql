#include "sql_stats_selectivity.h"

#include <math.h>
#include <stdlib.h>

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

static bool
joint_tuple_equal(const struct sql_stats_column_summary *summaries,
		  const struct sql_stats_mcv_value *a,
		  const struct sql_stats_mcv_value *b, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (summaries[i].compare(a[i].value, a[i].value_size,
					 b[i].value, b[i].value_size,
					 summaries[i].compare_context) != 0)
			return false;
	}
	return true;
}

int
sql_stats_estimate_equality_conjunction(
	const struct sql_stats_column_summary *summaries,
	const struct sql_stats_mcv_value *predicates, size_t term_count,
	const struct sql_stats_joint_mcv_sample *joint_mcv,
	size_t joint_mcv_count, uint64_t sample_nonnull_rows,
	struct sql_stats_selectivity *result)
{
	if (result == NULL || summaries == NULL || predicates == NULL ||
	    term_count == 0 || (joint_mcv_count != 0 &&
	    (joint_mcv == NULL || sample_nonnull_rows == 0)))
		return -1;
	double confidence = 1;
	struct sql_stats_selectivity *terms = calloc(term_count, sizeof(*terms));
	if (terms == NULL)
		return -1;
	int rc = -1;
	for (size_t i = 0; i < term_count; i++) {
		if (predicates[i].value == NULL && predicates[i].value_size != 0)
			goto done;
		if (sql_stats_estimate_equality(&summaries[i], predicates[i].value,
						predicates[i].value_size, false,
						&terms[i]) != 0)
			goto done;
		if (summaries[i].confidence < confidence)
			confidence = summaries[i].confidence;
	}
	{
		uint64_t total = 0;
		for (size_t i = 0; i < joint_mcv_count; i++) {
			const struct sql_stats_joint_mcv_sample *entry = &joint_mcv[i];
			if (entry->values == NULL || entry->value_count != term_count ||
			    entry->count == 0 || total > sample_nonnull_rows ||
			    entry->count > sample_nonnull_rows - total)
				goto done;
			for (size_t c = 0; c < term_count; c++) {
				if (entry->values[c].value == NULL &&
				    entry->values[c].value_size != 0)
					goto done;
			}
			for (size_t j = 0; j < i; j++) {
				if (joint_tuple_equal(summaries, entry->values,
						      joint_mcv[j].values,
						      term_count))
					goto done;
			}
			total += entry->count;
		}
	}
	for (size_t i = 0; i < joint_mcv_count; i++) {
		if (joint_tuple_equal(summaries, predicates, joint_mcv[i].values,
				      term_count)) {
			double estimate = (double)joint_mcv[i].count /
				sample_nonnull_rows;
			set_result(result, estimate, confidence,
				   SQL_STATS_SELECTIVITY_MCV);
			rc = 0;
			goto done;
		}
	}
	rc = sql_stats_selectivity_and(terms, term_count, result);
done:
	free(terms);
	return rc;
}

int
sql_stats_estimate_predicate_conjunction(
	const struct sql_stats_column_summary *summaries, size_t summary_count,
	const struct sql_stats_predicate *predicates, size_t predicate_count,
	const struct sql_stats_joint_mcv_sample *joint_mcv,
	size_t joint_mcv_count, uint64_t sample_nonnull_rows,
	struct sql_stats_selectivity *result)
{
	if (result == NULL || (predicate_count != 0 &&
	    (summaries == NULL || predicates == NULL)) ||
	    (joint_mcv_count != 0 &&
	    (summaries == NULL || joint_mcv == NULL || summary_count == 0 ||
	     sample_nonnull_rows == 0)))
		return -1;
	if (predicate_count == 0)
		return sql_stats_selectivity_and(NULL, 0, result);
	struct sql_stats_selectivity *terms =
		calloc(predicate_count, sizeof(*terms));
	if (terms == NULL)
		return -1;
	int rc = -1;
	double confidence = 1;
	for (size_t i = 0; i < predicate_count; i++) {
		const struct sql_stats_predicate *predicate = &predicates[i];
		if (predicate->column_index >= summary_count ||
		    (predicate->value == NULL && predicate->value_size != 0))
			goto done;
		for (size_t j = 0; j < i; j++) {
			if (predicates[j].column_index == predicate->column_index)
				goto done;
		}
		const struct sql_stats_column_summary *summary =
			&summaries[predicate->column_index];
		if (predicate->kind == SQL_STATS_PREDICATE_EQUALITY) {
			if (sql_stats_estimate_equality(summary, predicate->value,
							predicate->value_size, false,
							&terms[i]) != 0)
				goto done;
		} else if (predicate->kind == SQL_STATS_PREDICATE_RANGE) {
			if (sql_stats_estimate_range(summary, predicate->value,
						     predicate->value_size,
						     predicate->range_operator,
						     &terms[i]) != 0)
				goto done;
		} else {
			goto done;
		}
		if (summary->confidence < confidence)
			confidence = summary->confidence;
	}
	if (joint_mcv_count != 0) {
		uint64_t total = 0;
		for (size_t i = 0; i < summary_count; i++) {
			if (!valid_summary(&summaries[i]) ||
			    summaries[i].compare == NULL)
				goto done;
		}
		for (size_t i = 0; i < joint_mcv_count; i++) {
			const struct sql_stats_joint_mcv_sample *entry = &joint_mcv[i];
			if (entry->values == NULL || entry->value_count != summary_count ||
			    entry->count == 0 || total > sample_nonnull_rows ||
			    entry->count > sample_nonnull_rows - total)
				goto done;
			for (size_t c = 0; c < summary_count; c++) {
				/* Joint sample denominators are non-NULL on every column. */
				if (entry->values[c].value == NULL)
					goto done;
			}
			for (size_t j = 0; j < i; j++) {
				if (joint_tuple_equal(summaries, entry->values,
						      joint_mcv[j].values,
						      summary_count))
					goto done;
			}
			total += entry->count;
		}
		/* Partial joint MCVs cannot account for the unobserved tail. */
		if (total == sample_nonnull_rows) {
			uint64_t matching = 0;
			for (size_t i = 0; i < joint_mcv_count; i++) {
				const struct sql_stats_joint_mcv_sample *entry =
					&joint_mcv[i];
				bool matches = true;
				for (size_t j = 0; j < predicate_count; j++) {
					const struct sql_stats_predicate *predicate =
						&predicates[j];
					const struct sql_stats_mcv_value *value =
						&entry->values[predicate->column_index];
					int cmp = summaries[predicate->column_index].compare(
						value->value, value->value_size,
						predicate->value, predicate->value_size,
						summaries[predicate->column_index].compare_context);
					if (predicate->kind == SQL_STATS_PREDICATE_EQUALITY) {
						matches = cmp == 0;
					} else {
						switch (predicate->range_operator) {
						case SQL_STATS_RANGE_LT:
							matches = cmp < 0;
							break;
						case SQL_STATS_RANGE_LE:
							matches = cmp <= 0;
							break;
						case SQL_STATS_RANGE_GT:
							matches = cmp > 0;
							break;
						case SQL_STATS_RANGE_GE:
							matches = cmp >= 0;
							break;
						default:
							goto done;
						}
					}
					if (!matches)
						break;
				}
				if (matches)
					matching += entry->count;
			}
			set_result(result, matching / (double)sample_nonnull_rows,
				   confidence, SQL_STATS_SELECTIVITY_MCV);
			rc = 0;
			goto done;
		}
	}
	rc = sql_stats_selectivity_and(terms, predicate_count, result);
done:
	free(terms);
	return rc;
}
