#ifndef TARANTOOL_SQL_STATS_HLL_H
#define TARANTOOL_SQL_STATS_HLL_H

#include <stddef.h>
#include <stdint.h>

/**
 * In-memory HyperLogLog sketch for approximate SQL column NDV.
 *
 * Initialize with precision [4, 18] and a caller-selected seed; merge is
 * permitted only when both match. Inputs are byte strings, so callers must
 * encode typed SQL values unambiguously (including type and NULL semantics).
 * Hashing is deterministic across processes/platforms for the same seed and
 * bytes. The seed is a compatibility parameter, not a cryptographic secret.
 *
 * For m=2^precision registers, asymptotic relative standard error is about
 * 1.04/sqrt(m), with linear-counting correction for small cardinalities.
 * Precision 12 gives approximately 1.63% asymptotic RSE. This is probabilistic,
 * not a worst-case bound; confidence metadata remains the caller's concern.
 * No persistent representation is defined; do not serialize this struct.
 */
struct sql_stats_hll;

/** Allocate a sketch, or return NULL for invalid precision/allocation failure. */
struct sql_stats_hll *
sql_stats_hll_new(uint8_t precision, uint64_t seed);

/** Free a sketch; NULL is accepted. */
void
sql_stats_hll_delete(struct sql_stats_hll *hll);

/** Add one byte string. data may be NULL only when size is zero. */
int
sql_stats_hll_add(struct sql_stats_hll *hll, const void *data, size_t size);

/** Merge via register-wise maximum; returns -1 for incompatible sketches. */
int
sql_stats_hll_merge(struct sql_stats_hll *dst,
		    const struct sql_stats_hll *src);

/** Estimated cardinality, or 0 for NULL/empty sketch. */
double
sql_stats_hll_estimate(const struct sql_stats_hll *hll);

/** Configured precision, or 0 for NULL. */
uint8_t
sql_stats_hll_precision(const struct sql_stats_hll *hll);

#endif /* TARANTOOL_SQL_STATS_HLL_H */
