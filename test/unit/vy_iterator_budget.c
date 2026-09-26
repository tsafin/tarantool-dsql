#include "vy_iterator_budget.h"

#include "unit.h"

static void
test_source_budget(void)
{
	plan(2);
	header();
	struct vy_iterator_work_budget budget = {
		.max_disk_sources = 1,
		.max_page_reads = 2,
	};
	ok(vy_iterator_work_budget_try_source(&budget) &&
	   budget.disk_sources_probed == 1,
	   "first disk source consumes one operation-local unit");
	ok(!vy_iterator_work_budget_try_source(&budget) && budget.exhausted &&
	   budget.disk_sources_probed == 1,
	   "source budget exhaustion is sticky and does not overrun");
	footer();
	check_plan();
}

static void
test_page_budget(void)
{
	plan(3);
	header();
	struct vy_iterator_work_budget budget = {
		.max_disk_sources = 8,
		.max_page_reads = 1,
	};
	ok(vy_iterator_work_budget_try_page(&budget) &&
	   budget.page_reads_attempted == 1,
	   "first uncached page consumes one operation-local unit");
	ok(!vy_iterator_work_budget_try_page(&budget) && budget.exhausted &&
	   budget.page_reads_attempted == 1,
	   "page budget exhaustion is sticky and does not overrun");
	ok(vy_iterator_work_budget_try_source(NULL) &&
	   vy_iterator_work_budget_try_page(NULL),
	   "NULL budget preserves unlimited legacy iteration");
	footer();
	check_plan();
}

int
main(void)
{
	test_source_budget();
	test_page_budget();
	return 0;
}
