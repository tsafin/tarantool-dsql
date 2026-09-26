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

static void
test_reservoir_without_replacement(void)
{
	plan(5);
	header();
	const char *input[] = {"aaaa", "bbbb", "cccc", "dddd"};
	uint64_t metadata_bytes;
	fail_if(sql_stats_sample_reservoir_metadata_bytes(2,
							 &metadata_bytes) != 0);
	struct sql_stats_sample_reservoir *a =
		sql_stats_sample_reservoir_new(2, 8, metadata_bytes + 8, 17);
	struct sql_stats_sample_reservoir *b =
		sql_stats_sample_reservoir_new(2, 8, metadata_bytes + 8, 17);
	struct capture ca = {}, cb = {};
	struct sql_stats_sample_sink sa = {&ca, capture_tuple};
	struct sql_stats_sample_sink sb = {&cb, capture_tuple};
	struct sql_stats_sample_result ra = {}, rb = {};
	ok(a != NULL && b != NULL, "reservoir allocates within payload and buffer bounds");
	for (size_t i = 0; i < 4; ++i) {
		fail_if(sql_stats_sample_reservoir_add(a, input[i], 4) != 0);
		fail_if(sql_stats_sample_reservoir_add(b, input[i], 4) != 0);
	}
	ok(sql_stats_sample_reservoir_deliver(a, &sa, NULL, 0, &ra) == 0 &&
	   sql_stats_sample_reservoir_deliver(b, &sb, NULL, 0, &rb) == 0,
	   "completed reservoir can be delivered");
	ok(ra.rows == 2 && ra.bytes == 8 && ra.population_known &&
	   ra.visible_population == 4 && !ra.with_replacement,
	   "result reports sample size and exhaustive visible population");
	ok(ca.count == 2 && ca.rows[0] != ca.rows[1],
	   "reservoir draws distinct tuples without replacement");
	ok(ca.count == cb.count && memcmp(ca.rows, cb.rows, ca.count) == 0,
	   "fixed seed makes the reservoir sample deterministic");
	sql_stats_sample_reservoir_delete(a);
	sql_stats_sample_reservoir_delete(b);
	footer();
	check_plan();
}

static void
test_reservoir_uniformity_and_limits(void)
{
	plan(3);
	header();
	const char *input[] = {"aaaa", "bbbb", "cccc", "dddd"};
	uint64_t metadata_bytes;
	fail_if(sql_stats_sample_reservoir_metadata_bytes(1,
							 &metadata_bytes) != 0);
	uint32_t frequency[4] = {};
	for (uint64_t seed = 1; seed <= 4096; ++seed) {
		struct sql_stats_sample_reservoir *reservoir =
			sql_stats_sample_reservoir_new(1, 4,
						       metadata_bytes + 4, seed);
		fail_if(reservoir == NULL);
		for (size_t i = 0; i < 4; ++i)
			fail_if(sql_stats_sample_reservoir_add(reservoir,
								 input[i], 4) != 0);
		struct capture capture = {};
		struct sql_stats_sample_sink sink = {&capture, capture_tuple};
		struct sql_stats_sample_result result = {};
		fail_if(sql_stats_sample_reservoir_deliver(reservoir, &sink, NULL,
								   0, &result) != 0);
		fail_if(capture.count != 1);
		frequency[capture.rows[0] - 'a']++;
		sql_stats_sample_reservoir_delete(reservoir);
	}
	ok(frequency[0] > 900 && frequency[0] < 1150 &&
	   frequency[1] > 900 && frequency[1] < 1150 &&
	   frequency[2] > 900 && frequency[2] < 1150 &&
	   frequency[3] > 900 && frequency[3] < 1150,
	   "seed sweep is consistent with uniform inclusion over four input keys");
	struct sql_stats_sample_reservoir *too_small =
		sql_stats_sample_reservoir_new(1, 4, metadata_bytes - 1, 1);
	ok(too_small == NULL,
	   "slot metadata is rejected when it exceeds the hard buffer budget");
	struct sql_stats_sample_reservoir *reservoir =
		sql_stats_sample_reservoir_new(1, 3, metadata_bytes + 4, 1);
	ok(reservoir != NULL &&
	   sql_stats_sample_reservoir_add(reservoir, input[0], 4) != 0 &&
	   sql_stats_sample_reservoir_population(reservoir) == 0,
	   "payload byte exhaustion aborts before a sample is complete");
	sql_stats_sample_reservoir_delete(reservoir);
	footer();
	check_plan();
}

int
main(void)
{
	test_row_and_byte_budgets();
	test_seed_and_replacement();
	test_invalid_and_aborted_requests();
	test_reservoir_without_replacement();
	test_reservoir_uniformity_and_limits();
	return 0;
}
