#include "box/sql/sql_stats_sample.h"

#include <string.h>

#include "unit.h"

struct fake_source {
	unsigned calls;
	unsigned stop_after;
};

struct capture {
	char rows[16];
	unsigned count;
	const uint32_t *field_ids;
	size_t field_count;
	bool fail;
};

static int
fake_random(void *context, uint32_t seed, const char **tuple, size_t *size)
{
	struct fake_source *source = context;
	if (source->stop_after != 0 && source->calls >= source->stop_after) {
		*tuple = NULL;
		*size = 0;
		return 0;
	}
	++source->calls;
	*tuple = (seed & 1) == 0 ? "aaaa" : "bbbb";
	*size = 4;
	return 0;
}

static int
capture_tuple(void *context, const char *tuple, size_t size,
	      const uint32_t *field_ids, size_t field_count)
{
	struct capture *capture = context;
	if (capture->fail)
		return -1;
	if (size != 4 || capture->count >= sizeof(capture->rows))
		return -1;
	capture->rows[capture->count++] = tuple[0];
	capture->field_ids = field_ids;
	capture->field_count = field_count;
	return 0;
}

static struct sql_stats_sample_request
make_request(uint64_t rows, uint64_t bytes, uint64_t seed)
{
	return (struct sql_stats_sample_request){
		.max_rows = rows,
		.max_bytes = bytes,
		.seed = seed,
	};
}

static void
test_row_and_byte_budgets(void)
{
	plan(5);
	header();
	struct fake_source source = {};
	struct capture capture = {};
	struct sql_stats_sample_sink sink = {&capture, capture_tuple};
	struct sql_stats_sample_result result;
	struct sql_stats_sample_request request = make_request(5, 9, 12);
	ok(sql_stats_sample_run(&request, &sink, &result, fake_random,
				&source) == 0, "bounded sampling succeeds");
	ok(result.rows == 2 && result.bytes == 8 && source.calls == 3,
	   "byte limit stops before delivering an oversized next draw");
	ok(capture.count == 2 && result.with_replacement,
	   "delivered draw count includes replacement sampling semantics");
	request = make_request(1, 100, 12);
	source.calls = 0;
	capture.count = 0;
	ok(sql_stats_sample_run(&request, &sink, &result, fake_random,
				&source) == 0 && result.rows == 1 &&
	   source.calls == 1, "row limit is also a hard upper bound");
	ok(result.bytes == 4 && capture.count == 1,
	   "result reports delivered bytes and rows");
	footer();
	check_plan();
}

static void
test_seed_and_replacement(void)
{
	plan(3);
	header();
	struct fake_source source_a = {};
	struct fake_source source_b = {};
	struct capture capture_a = {};
	struct capture capture_b = {};
	struct sql_stats_sample_sink sink_a = {&capture_a, capture_tuple};
	struct sql_stats_sample_sink sink_b = {&capture_b, capture_tuple};
	struct sql_stats_sample_result result_a, result_b;
	struct sql_stats_sample_request request = make_request(8, 100, 77);
	ok(sql_stats_sample_run(&request, &sink_a, &result_a, fake_random,
				&source_a) == 0 &&
	   sql_stats_sample_run(&request, &sink_b, &result_b, fake_random,
				&source_b) == 0,
	   "same seed produces repeatable samples");
	ok(capture_a.count == 8 && capture_b.count == 8 &&
	   memcmp(capture_a.rows, capture_b.rows, 8) == 0,
	   "draw sequence is deterministic for a fixed seed");
	ok(result_a.with_replacement && result_a.rows == 8,
	   "repeated sampled rows remain separate counted draws");
	footer();
	check_plan();
}

static void
test_invalid_and_aborted_requests(void)
{
	plan(3);
	header();
	struct fake_source source = {};
	struct capture capture = {};
	struct sql_stats_sample_sink sink = {&capture, capture_tuple};
	struct sql_stats_sample_result result;
	struct sql_stats_sample_request request = make_request(0, 100, 1);
	ok(sql_stats_sample_run(&request, &sink, &result, fake_random,
				&source) == -1,
	   "zero row budget is rejected");
	request = make_request(3, 100, 1);
	capture.fail = true;
	ok(sql_stats_sample_run(&request, &sink, &result, fake_random,
				&source) == -1,
	   "sink failure aborts sampling");
	capture.fail = false;
	request.field_count = 1;
	request.field_ids = NULL;
	ok(sql_stats_sample_run(&request, &sink, &result, fake_random,
				&source) == -1,
	   "missing requested field list is rejected");
	footer();
	check_plan();
}

int
main(void)
{
	test_row_and_byte_budgets();
	test_seed_and_replacement();
	test_invalid_and_aborted_requests();
	return 0;
}
