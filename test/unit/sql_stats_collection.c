#include "box/sql/sql_stats_collection.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "box/index.h"
#include "box/index_def.h"
#include "box/box.h"
#include "box/read_view.h"
#include "box/schema.h"
#include "box/space.h"
#include "box/space_cache.h"
#include "box/space_def.h"
#include "box/txn.h"
#include "vclock/vclock.h"
#include "unit.h"

static struct vclock test_vclock;
const struct vclock *box_vclock = &test_vclock;

static uint64_t test_schema_version = 12;
static int test_read_view_mode;
static unsigned test_read_view_close_count;
static bool test_txn_active;
static int64_t test_txn_id;
static int test_txn_isolation;
static int test_txn_begin_result;
static int test_txn_set_isolation_result;
static int test_txn_commit_result;
static int test_txn_rollback_result;
static int test_engine_sample_mode;
static bool test_schema_change_on_isolation;

static void
advance_test_vclock(void)
{
	vclock_follow(&test_vclock, 0, vclock_get(&test_vclock, 0) + 1);
}

static struct space_def test_space_def = {.id = 42};
static struct space test_space = {.def = &test_space_def};
static struct index_def test_index_def = {.space_id = 42, .iid = 8};
static struct index test_index = {.def = &test_index_def};
static struct index_read_view test_index_view = {.def = &test_index_def};
static struct index *test_space_index_map[9];
static struct index_read_view *test_index_map[9];
static struct space_read_view test_space_view = {
	.id = 42, .index_id_max = 8, .index_map = test_index_map,
};

struct test_index_iterator {
	struct index_read_view_iterator_base base;
	unsigned next;
};

static bool test_schema_change_on_eof;

static int
test_index_iterator_next(struct index_read_view_iterator *iterator,
			 struct read_view_tuple *tuple)
{
	static const char *const values[] = {"a", "bb"};
	struct test_index_iterator *it =
		(struct test_index_iterator *)iterator;
	if (it->next == sizeof(values) / sizeof(values[0])) {
		*tuple = read_view_tuple_none();
		if (test_schema_change_on_eof)
			test_schema_version++;
		return 0;
	}
	const char *value = values[it->next++];
	*tuple = (struct read_view_tuple){
		.data = value, .size = strlen(value),
	};
	return 0;
}

static void
test_index_iterator_destroy(struct index_read_view_iterator *iterator)
{
	(void)iterator;
}

static int
test_index_iterator_position(struct index_read_view_iterator *iterator,
			    const char **pos, uint32_t *size)
{
	(void)iterator;
	*pos = NULL;
	*size = 0;
	return 0;
}

static int
test_index_create_iterator(struct index_read_view *view,
			   enum iterator_type type, const char *key,
			   uint32_t part_count, const char *pos,
			   struct index_read_view_iterator *iterator)
{
	(void)type;
	(void)key;
	(void)part_count;
	(void)pos;
	struct test_index_iterator *it =
		(struct test_index_iterator *)iterator;
	it->base.index = view;
	it->base.next_raw = test_index_iterator_next;
	it->base.destroy = test_index_iterator_destroy;
	it->base.position = test_index_iterator_position;
	it->next = 0;
	return 0;
}

static const struct index_read_view_vtab test_index_view_vtab = {
	.create_iterator = test_index_create_iterator,
};

struct test_sink_state {
	unsigned rows;
	size_t bytes;
};

static int
test_sink_consume(void *context, const char *tuple, size_t tuple_size,
		  const uint32_t *field_ids, size_t field_count)
{
	(void)tuple;
	(void)field_ids;
	(void)field_count;
	struct test_sink_state *state = context;
	state->rows++;
	state->bytes += tuple_size;
	return 0;
}

uint64_t
box_schema_version(void)
{
	return test_schema_version;
}

bool
box_txn(void)
{
	return test_txn_active;
}

int64_t
box_txn_id(void)
{
	return test_txn_active ? test_txn_id : -1;
}

int
box_txn_isolation(void)
{
	return test_txn_active ? test_txn_isolation : -1;
}

int
box_txn_begin(void)
{
	if (test_txn_active || test_txn_begin_result != 0)
		return -1;
	test_txn_active = true;
	test_txn_id++;
	test_txn_isolation = TXN_ISOLATION_BEST_EFFORT;
	return 0;
}

int
box_txn_set_isolation(uint32_t isolation)
{
	if (!test_txn_active || test_txn_set_isolation_result != 0)
		return -1;
	test_txn_isolation = isolation;
	if (test_schema_change_on_isolation)
		test_schema_version++;
	return 0;
}

int
box_txn_commit(void)
{
	if (!test_txn_active)
		return -1;
	test_txn_active = false;
	return test_txn_commit_result;
}

int
box_txn_rollback(void)
{
	if (!test_txn_active || test_txn_rollback_result != 0)
		return -1;
	test_txn_active = false;
	return 0;
}

struct space *
space_by_id_slow(uint32_t id)
{
	if (id != test_space_def.id)
		return NULL;
	test_space.index_map = test_space_index_map;
	test_space.index_id_max = 8;
	return &test_space;
}

int
engine_sql_stats_sample(struct space *space,
			const struct sql_stats_sample_request *request,
			struct sql_stats_sample_sink *sink,
			struct sql_stats_sample_result *result)
{
	(void)request;
	if (space != &test_space)
		return -1;
	static const char first[] = "a";
	static const char second[] = "bb";
	*result = (struct sql_stats_sample_result){};
	if (sink->consume(sink->context, first, sizeof(first) - 1, NULL, 0) != 0)
		return -1;
	result->rows = 1;
	result->bytes = sizeof(first) - 1;
	if (test_engine_sample_mode == 1)
		return -1;
	if (sink->consume(sink->context, second, sizeof(second) - 1, NULL, 0) != 0)
		return -1;
	result->rows++;
	result->bytes += sizeof(second) - 1;
	result->population_known = true;
	result->visible_population = 2;
	if (test_engine_sample_mode == 2)
		test_schema_version++;
	if (test_engine_sample_mode == 3)
		advance_test_vclock();
	return 0;
}

void
read_view_opts_create(struct read_view_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
}

int
read_view_open(struct read_view *view, const struct read_view_opts *opts)
{
	if (test_read_view_mode == 0)
		return -1;
	view->id = 99;
	rlist_create(&view->spaces);
	rlist_create(&test_space_view.link);
	memset(test_index_map, 0, sizeof(test_index_map));
	if (test_read_view_mode != 3 &&
	    opts->filter_space(&test_space, opts->filter_arg) &&
	    opts->filter_index(&test_space, &test_index, opts->filter_arg)) {
		test_index_map[8] = &test_index_view;
		rlist_add_tail_entry(&view->spaces, &test_space_view, link);
	}
	if (test_read_view_mode == 2)
		test_schema_version++;
	return 0;
}

void
read_view_close(struct read_view *view)
{
	test_read_view_close_count++;
	if (!rlist_empty(&test_space_view.link))
		rlist_del(&test_space_view.link);
	rlist_create(&view->spaces);
}

static void
test_collection_context(void)
{
	plan(6);
	header();
	struct sql_stats_collection_target target = {.space_id = 42, .index_id = 8};
	struct sql_stats_collection_target duplicate[] = {target, target};
	test_read_view_mode = 1;
	unsigned close_count = test_read_view_close_count;
	ok(sql_stats_collection_context_new(duplicate, 2) == NULL &&
	   test_read_view_close_count == close_count,
	   "duplicate collection targets fail before opening a read view");
	struct sql_stats_collection_context *context =
		sql_stats_collection_context_new(&target, 1);
	ok(context != NULL &&
	   sql_stats_collection_context_visibility_id(context) == 99 &&
	   sql_stats_collection_context_schema_version(context) == 12,
	   "collector owns the pinned engine read-view ID and schema generation");
	sql_stats_collection_context_delete(context);
	ok(test_read_view_close_count == close_count + 1,
	   "deleting collection context closes its owned read view");
	test_read_view_mode = 3;
	close_count = test_read_view_close_count;
	ok(sql_stats_collection_context_new(&target, 1) == NULL &&
	   test_read_view_close_count == close_count + 1,
	   "missing requested index closes view and fails collection closed");
	test_schema_version = 12;
	test_read_view_mode = 2;
	close_count = test_read_view_close_count;
	ok(sql_stats_collection_context_new(&target, 1) == NULL &&
	   test_read_view_close_count == close_count + 1,
	   "schema drift during view capture closes and rejects context");
	test_schema_version = 12;
	test_read_view_mode = 0;
	ok(sql_stats_collection_context_new(&target, 1) == NULL,
	   "engine read-view open failure does not produce a context");
	footer();
	check_plan();
}

static void
test_context_sample_index(void)
{
	plan(4);
	header();
	test_read_view_mode = 1;
	test_schema_version = 12;
	test_schema_change_on_eof = false;
	test_index_view.vtab = &test_index_view_vtab;
	struct sql_stats_collection_target target = {.space_id = 42, .index_id = 8};
	struct sql_stats_collection_context *context =
		sql_stats_collection_context_new(&target, 1);
	struct test_sink_state sink_state = {};
	struct sql_stats_sample_sink sink = {
		.context = &sink_state, .consume = test_sink_consume,
	};
	struct sql_stats_sample_request request = {
		.max_rows = 2, .max_bytes = 16, .max_buffer_bytes = 256,
		.max_tuples_examined = 3, .seed = 7,
	};
	struct sql_stats_sample_result result;
	int rc = sql_stats_collection_context_sample_index(context, &target,
		&request, &sink, &result);
	ok(rc == 0 && result.rows == 2 && result.visible_population == 2 &&
	   !result.with_replacement && sink_state.rows == 2 &&
	   sink_state.bytes == 3,
	   "pinned secondary-index scan delivers a bounded exhaustive sample");
	sink_state = (struct test_sink_state){};
	request.max_tuples_examined = 2;
	rc = sql_stats_collection_context_sample_index(context, &target,
		&request, &sink, &result);
	ok(rc != 0 && result.rows == 0 && sink_state.rows == 0,
	   "tuple-budget exhaustion fails before sink delivery");
	request.max_tuples_examined = 3;
	test_schema_change_on_eof = true;
	rc = sql_stats_collection_context_sample_index(context, &target,
		&request, &sink, &result);
	ok(rc != 0 && result.rows == 0 && sink_state.rows == 0,
	   "schema drift after pinned scan fails before sink delivery");
	test_schema_change_on_eof = false;
	test_schema_version = 12;
	unsigned close_count = test_read_view_close_count;
	sql_stats_collection_context_delete(context);
	ok(test_read_view_close_count == close_count + 1,
	   "sample context closes its read view after scanning");
	footer();
	check_plan();
}

static void
reset_test_txn(void)
{
	test_txn_active = false;
	test_txn_isolation = -1;
	test_txn_begin_result = 0;
	test_txn_set_isolation_result = 0;
	test_txn_commit_result = 0;
	test_txn_rollback_result = 0;
	test_schema_change_on_isolation = false;
	test_engine_sample_mode = 0;
	test_schema_version = 12;
	vclock_create(&test_vclock);
	test_space_index_map[8] = &test_index;
	test_index.unique_id = 808;
}

static void
test_transaction_sample_context(void)
{
	plan(27);
	header();
	reset_test_txn();
	struct sql_stats_collection_target target = {.space_id = 42, .index_id = 8};
	struct sql_stats_collection_target duplicate[] = {target, target};
	struct sql_stats_tx_context *context = NULL;
	ok(sql_stats_tx_context_begin(duplicate, 2, &context) != 0 &&
	   context == NULL && !test_txn_active,
	   "duplicate targets fail before the context acquires a transaction");
	struct sql_stats_collection_target missing = {
		.space_id = 42, .index_id = 7,
	};
	ok(sql_stats_tx_context_begin(&missing, 1, &context) != 0 &&
	   !test_txn_active,
	   "missing target index fails before transaction begin");
	test_txn_active = true;
	test_txn_id = 100;
	ok(sql_stats_tx_context_begin(&target, 1, &context) != 0 &&
	   test_txn_active && test_txn_id == 100,
	   "context refuses to take ownership of an existing transaction");
	test_txn_active = false;
	test_txn_begin_result = -1;
	ok(sql_stats_tx_context_begin(&target, 1, &context) != 0 &&
	   context == NULL && !test_txn_active,
	   "failed transaction begin returns no active context");
	test_txn_begin_result = 0;
	test_txn_set_isolation_result = -1;
	ok(sql_stats_tx_context_begin(&target, 1, &context) != 0 &&
	   context == NULL && !test_txn_active,
	   "failed READ_CONFIRMED setup rolls back the owned transaction");
	test_txn_set_isolation_result = 0;
	test_schema_change_on_isolation = true;
	ok(sql_stats_tx_context_begin(&target, 1, &context) != 0 &&
	   context == NULL && !test_txn_active,
	   "schema drift during begin rolls back without returning a context");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0 &&
	   context != NULL && test_txn_active &&
	   test_txn_isolation == TXN_ISOLATION_READ_CONFIRMED &&
	   sql_stats_tx_context_visibility_id(context) == 0,
	   "context owns a READ_CONFIRMED transaction and captures visibility");
	struct test_sink_state sink_state = {};
	struct sql_stats_sample_sink sink = {
		.context = &sink_state, .consume = test_sink_consume,
	};
	struct sql_stats_sample_request request = {
		.max_rows = 2, .max_bytes = 16, .max_buffer_bytes = 256,
	};
	struct sql_stats_sample_result result;
	test_engine_sample_mode = 1;
	ok(sql_stats_tx_context_sample_index(context, &target, &request,
		&sink, &result) != 0 && sink_state.rows == 0 && result.rows == 0,
	   "partial engine failure is withheld from the collector sink");
	ok(sql_stats_tx_context_finish(&context) != 0 && context == NULL &&
	   !test_txn_active,
	   "failed sampling cannot finish or publish a partial collection");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0,
	   "context begins before a concurrent commit check");
	test_engine_sample_mode = 3;
	sink_state = (struct test_sink_state){};
	ok(sql_stats_tx_context_sample_index(context, &target, &request,
		&sink, &result) != 0 && sink_state.rows == 0 && result.rows == 0,
	   "visibility generation drift withholds staged tuples");
	ok(sql_stats_tx_context_finish(&context) != 0 && context == NULL &&
	   !test_txn_active,
	   "visibility-drifted context rolls back instead of completing");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0,
	   "context begins before finish-boundary visibility check");
	test_engine_sample_mode = 0;
	sink_state = (struct test_sink_state){};
	ok(sql_stats_tx_context_sample_index(context, &target, &request,
		&sink, &result) == 0,
	   "sample completes before a visibility change at finish");
	advance_test_vclock();
	ok(sql_stats_tx_context_finish(&context) != 0 && context == NULL &&
	   !test_txn_active,
	   "finish-boundary visibility drift rejects the completed sample");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0 &&
	   sql_stats_tx_context_finish(&context) != 0 && context == NULL &&
	   !test_txn_active,
	   "finish with a missing target sample rolls back the transaction");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0,
	   "context starts before a runtime isolation check");
	test_txn_isolation = TXN_ISOLATION_READ_COMMITTED;
	sink_state = (struct test_sink_state){};
	ok(sql_stats_tx_context_sample_index(context, &target, &request,
		&sink, &result) != 0 && sink_state.rows == 0,
	   "changed transaction isolation fails before sample delivery");
	ok(sql_stats_tx_context_finish(&context) != 0 && context == NULL &&
	   !test_txn_active,
	   "finish rolls back when the owned transaction mode changed");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0,
	   "context starts before a runtime schema check");
	test_engine_sample_mode = 2;
	sink_state = (struct test_sink_state){};
	ok(sql_stats_tx_context_sample_index(context, &target, &request,
		&sink, &result) != 0 && sink_state.rows == 0,
	   "schema drift during engine sampling withholds staged tuples");
	ok(sql_stats_tx_context_finish(&context) != 0 && context == NULL &&
	   !test_txn_active,
	   "schema-drifted sample context rolls back");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0,
	   "context begins before abort ownership validation");
	int64_t owned_id = test_txn_id;
	test_txn_id++;
	int abort_rc = sql_stats_tx_context_abort(&context);
	bool preserved = abort_rc != 0 && context != NULL && test_txn_active;
	test_txn_id = owned_id;
	ok(preserved && sql_stats_tx_context_abort(&context) == 0 &&
	   context == NULL && !test_txn_active,
	   "abort preserves another transaction and releases only its own");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0 &&
	   sql_stats_tx_context_abort(&context) == 0 && context == NULL &&
	   !test_txn_active,
	   "explicit abort rolls back and consumes the owned context");
	reset_test_txn();
	ok(sql_stats_tx_context_begin(&target, 1, &context) == 0 &&
	   sql_stats_tx_context_sample_index(context, &target, &request,
		&sink, &result) == 0 && sink_state.rows == 2,
	   "successful requested-index sample stages complete rows");
	ok(sql_stats_tx_context_finish(&context) == 0 && context == NULL &&
	   !test_txn_active,
	   "finish commits only after all requested index samples succeed");
	footer();
	check_plan();
}

static void
test_population_from_engine_sample(void)
{
	plan(9);
	header();
	struct sql_stats_collected_population population;
	struct sql_stats_sample_result sample = {
		.rows = 8, .bytes = 256, .population_known = true,
		.visible_population = 20, .with_replacement = true,
	};
	ok(sql_stats_collection_population_from_sample(&sample, &population) &&
	   population.row_count == 20 &&
	   population.semantics == SQL_STATS_CARDINALITY_VISIBLE_ROWS,
	   "memtx sample draws retain exact visible population, not draw count");
	struct sql_stats_collected_width width;
	ok(sql_stats_collection_width_from_sample(&sample, &width) &&
	   width.average_bytes == 32 && width.denominator_rows == 8,
	   "sample bytes produce a row-count-denominated mean serialized width");
	struct sql_stats_sample_result fractional = {
		.rows = 3, .bytes = 10, .population_known = true,
		.visible_population = 20, .with_replacement = true,
	};
	ok(sql_stats_collection_width_from_sample(&fractional, &width) &&
	   fabs(width.average_bytes - 10.0 / 3.0) < 1e-12 &&
	   width.denominator_rows == 3,
	   "fractional sample-average width is not rounded down");
	sample.bytes = 7;
	ok(!sql_stats_collection_width_from_sample(&sample, &width),
	   "sample byte count cannot be smaller than its tuple count");
	sample.bytes = 256;
	sample.with_replacement = false;
	ok(sql_stats_collection_population_from_sample(&sample, &population),
	   "exhaustive Vinyl population converts when sampled rows fit population");
	sample.rows = 21;
	ok(!sql_stats_collection_population_from_sample(&sample, &population),
	   "without-replacement rows cannot exceed visible population");
	sample.rows = 0;
	sample.visible_population = 0;
	ok(sql_stats_collection_population_from_sample(&sample, &population) &&
	   population.row_count == 0,
	   "empty visible population is a valid exact count");
	ok(!sql_stats_collection_width_from_sample(&sample, &width),
	   "empty sample does not invent a row-width estimate");
	sample.population_known = false;
	ok(!sql_stats_collection_population_from_sample(&sample, &population),
	   "unknown engine population cannot become exact relation count");
	footer();
	check_plan();
}

static void
test_complete_result_and_rejections(void)
{
	plan(24);
	header();
	uint64_t prefixes[] = {2, 4};
	struct sql_stats_expected_index expected_index = {
		.index_id = 8, .definition_version = 3, .part_count = 2,
	};
	struct sql_stats_expected_relation expected_relation = {
		.space_id = 42, .modification_epoch = 11,
		.indexes = &expected_index, .index_count = 1,
	};
	struct sql_stats_collection_generation generation = {
		.catalog_version = 4, .schema_version = 7, .visibility_id = 9,
	};
	struct sql_stats_collected_index index = {
		.index_id = 8, .definition_version = 3, .visibility_id = 9,
		.tuple_count = 10,
		.tuple_count_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.population_basis = "visible_rows@view-9", .ndv_basis = "visible_rows@view-9",
		.distinct_prefixes = prefixes, .prefix_count = 2,
	};
	struct sql_stats_collected_relation relation = {
		.space_id = 42, .catalog_version = 4, .schema_version = 7,
		.visibility_id = 9, .modification_epoch = 11, .row_count = 10,
		.cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS,
		.population_basis = "visible_rows@view-9", .average_row_width = 24,
		.width_basis = "sampled_payload_bytes/sample_rows",
		.width_denominator_count = 8,
		.confidence = 0.75, .confidence_source = "caller-calibrated-v1",
		.collected_at = 12, .indexes = &index, .index_count = 1,
	};
	struct sql_stats_collection_result result = {
		.generation = generation, .relations = &relation, .relation_count = 1,
	};
	struct sql_stats_snapshot *snapshot = sql_stats_collection_build_candidate(
		&generation, &expected_relation, 1, &result, 4096);
	ok(snapshot != NULL, "complete matching collection builds candidate");
	if (snapshot != NULL) {
		const struct sql_stats_relation *r = NULL;
		const struct sql_stats_index *i = NULL;
		ok(sql_stats_snapshot_get_relation(snapshot, 7, 42, &r) ==
		   SQL_STATS_LOOKUP_AVAILABLE, "candidate contains relation");
		ok(strcmp(sql_stats_relation_width_basis(r),
			  "sampled_payload_bytes/sample_rows") == 0 &&
		   strcmp(sql_stats_relation_population_basis(r),
			  "visible_rows@view-9") == 0 &&
		   sql_stats_relation_width_denominator_count(r) == 8 &&
		   strcmp(sql_stats_relation_confidence_source(r),
			  "caller-calibrated-v1") == 0 &&
		   sql_stats_relation_visibility_id(r) == 9,
		   "relation provenance and visibility token deep-copied");
		ok(sql_stats_relation_get_index(r, 8, &i) == SQL_STATS_LOOKUP_AVAILABLE &&
		   sql_stats_index_definition_version(i) == 3 &&
		   sql_stats_index_tuple_count_semantics(i) ==
			SQL_STATS_CARDINALITY_VISIBLE_ROWS &&
		   strcmp(sql_stats_index_population_basis(i), "visible_rows@view-9") == 0 &&
		   strcmp(sql_stats_index_ndv_basis(i), "visible_rows@view-9") == 0,
		   "index population and NDV provenance deep-copied");
		prefixes[0] = 99;
		ok(sql_stats_index_distinct_prefix(i, 0) == 2,
		   "candidate owns copied prefix values");
		sql_stats_snapshot_release(snapshot);
	} else {
		ok(false, "candidate contains relation");
		ok(false, "relation provenance and visibility token deep-copied");
		ok(false, "index population and NDV provenance deep-copied");
		ok(false, "candidate owns copied prefix values");
	}
	prefixes[0] = 2;
	unsigned int staging_failures = 0;
	bool complete_candidate = false;
	for (long fail_after = 0; fail_after < 4; fail_after++) {
		sql_stats_collection_test_fail_allocation_after(fail_after);
		struct sql_stats_snapshot *candidate =
			sql_stats_collection_build_candidate(&generation,
				&expected_relation, 1, &result, 4096);
		if (candidate == NULL) {
			staging_failures++;
			continue;
		}
		sql_stats_collection_test_fail_allocation_after(-1);
		sql_stats_snapshot_release(candidate);
		complete_candidate = true;
		break;
	}
	ok(staging_failures == 2 && complete_candidate,
	   "each collection staging allocation fails before candidate succeeds");
	relation.index_count = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing expected index is rejected");
	relation.index_count = 1;
	result.relation_count = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing expected relation is rejected");
	result.relation_count = 1;
	index.prefix_count = 1;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing leading-prefix NDV is rejected");
	index.prefix_count = 2;
	index.definition_version++;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "index-definition mismatch is rejected");
	index.definition_version--;
	index.visibility_id++;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "index visibility mismatch is rejected");
	index.visibility_id--;
	generation.schema_version++;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "expected generation mismatch is rejected");
	generation.schema_version--;
	index.population_basis = "physical-tuples";
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "mixed population bases are rejected");
	index.population_basis = "visible_rows@view-9";
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 1) == NULL, "candidate copy budget failure is rejected");
	bool saw_budget_rejection = false;
	bool reached_complete_candidate = false;
	for (size_t budget = 1; budget <= 4096; budget++) {
		struct sql_stats_snapshot *candidate =
			sql_stats_collection_build_candidate(&generation,
				&expected_relation, 1, &result, budget);
		if (candidate == NULL) {
			saw_budget_rejection = true;
			continue;
		}
		sql_stats_snapshot_release(candidate);
		reached_complete_candidate = true;
		break;
	}
	ok(saw_budget_rejection && reached_complete_candidate,
	   "budget sweep rejects incomplete copies then accepts a full candidate");
	index.visibility_id = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "unknown index visibility token is rejected");
	index.visibility_id = 9;
	index.definition_version = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "unknown index definition version is rejected");
	index.definition_version = 3;
	index.tuple_count_semantics = (enum sql_stats_cardinality_semantics)99;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "invalid index population semantics rejected");
	index.tuple_count_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS;
	relation.cardinality_semantics = (enum sql_stats_cardinality_semantics)99;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "invalid relation cardinality semantics rejected");
	relation.cardinality_semantics = SQL_STATS_CARDINALITY_VISIBLE_ROWS;
	relation.width_denominator_count = 0;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "missing width denominator count rejected");
	relation.width_denominator_count = 8;
	relation.row_count = NAN;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "non-finite relation count rejected");
	relation.row_count = -1;
	ok(sql_stats_collection_build_candidate(&generation, &expected_relation, 1,
		&result, 4096) == NULL, "negative relation count rejected");
	relation.row_count = 10;
	struct sql_stats_expected_relation expected_relations[] = {
		expected_relation,
		{ .space_id = 43, .modification_epoch = 11,
		  .indexes = &expected_index, .index_count = 1 },
	};
	struct sql_stats_collected_relation duplicate_relations[] = {
		relation,
		relation,
	};
	struct sql_stats_collection_result duplicate_relation_result = {
		.generation = generation,
		.relations = duplicate_relations,
		.relation_count = 2,
	};
	ok(sql_stats_collection_build_candidate(&generation,
		&expected_relations[0], 2, &duplicate_relation_result, 4096) == NULL,
	   "duplicate collected relation IDs are rejected");
	struct sql_stats_expected_index expected_indexes[] = {
		expected_index,
		{ .index_id = 9, .definition_version = 3, .part_count = 2 },
	};
	struct sql_stats_collected_index duplicate_indexes[] = {
		index,
		index,
	};
	relation.indexes = duplicate_indexes;
	relation.index_count = 2;
	struct sql_stats_expected_relation two_indexes = {
		.space_id = 42,
		.modification_epoch = 11,
		.indexes = expected_indexes,
		.index_count = 2,
	};
	struct sql_stats_collection_result duplicate_index_result = {
		.generation = generation,
		.relations = &relation,
		.relation_count = 1,
	};
	ok(sql_stats_collection_build_candidate(&generation, &two_indexes, 1,
		&duplicate_index_result, 4096) == NULL,
	   "duplicate collected index IDs are rejected");
	footer();
	check_plan();
}

int
main(void)
{
	test_collection_context();
	test_context_sample_index();
	test_transaction_sample_context();
	test_population_from_engine_sample();
	test_complete_result_and_rejections();
	return 0;
}
