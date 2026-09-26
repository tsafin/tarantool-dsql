#ifndef INCLUDES_TARANTOOL_BOX_VY_ITERATOR_BUDGET_H
#define INCLUDES_TARANTOOL_BOX_VY_ITERATOR_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Optional per-operation work limits for statistics sampling through Vinyl.
 * The caller owns this object and must keep it alive until iterator close.
 * Counters count first probes of disk sources and uncached page-read attempts.
 */
struct vy_iterator_work_budget {
	uint64_t max_disk_sources;
	uint64_t max_page_reads;
	uint64_t disk_sources_probed;
	uint64_t page_reads_attempted;
	bool exhausted;
};

static inline bool
vy_iterator_work_budget_try_source(struct vy_iterator_work_budget *budget)
{
	if (budget == NULL)
		return true;
	if (budget->exhausted ||
	    budget->disk_sources_probed >= budget->max_disk_sources) {
		budget->exhausted = true;
		return false;
	}
	budget->disk_sources_probed++;
	return true;
}

static inline bool
vy_iterator_work_budget_try_page(struct vy_iterator_work_budget *budget)
{
	if (budget == NULL)
		return true;
	if (budget->exhausted ||
	    budget->page_reads_attempted >= budget->max_page_reads) {
		budget->exhausted = true;
		return false;
	}
	budget->page_reads_attempted++;
	return true;
}

#endif /* INCLUDES_TARANTOOL_BOX_VY_ITERATOR_BUDGET_H */
