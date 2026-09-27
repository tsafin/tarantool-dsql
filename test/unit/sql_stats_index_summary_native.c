#include <stdint.h>

#include "coll/coll.h"
#include "fiber.h"
#include "key_def.h"
#include "memory.h"
#include "msgpuck.h"
#include "box/sql/sql_stats_index_summary.h"
#include "tuple.h"
#include "unit.h"

static uint32_t
test_field_name_hash(const char *name, uint32_t len)
{
	return len == 0 ? 0 : (uint8_t)name[0] + len;
}

int
main(void)
{
	memory_init();
	fiber_init(fiber_c_invoke);
	coll_init();
	tuple_init(test_field_name_hash);
	plan(4);
	header();

	struct key_part_def part = key_part_def_default;
	part.fieldno = 0;
	part.type = FIELD_TYPE_UNSIGNED;
	struct key_def *key_def = key_def_new(&part, 1, 0);
	if (key_def != NULL)
		key_def_update_optionality(key_def, 0);
	size_t sketch_bytes = 0;
	ok(key_def != NULL && sql_stats_hll_storage_bytes(8,
		&sketch_bytes) != 0, "unsigned key definition and HLL budget initialize");
	struct index_def index_def = {
		.type = TREE,
		.key_def = key_def,
	};
	struct sql_stats_index_summary *summary = key_def == NULL ? NULL :
		sql_stats_index_summary_new_for_index(tuple_format_runtime,
			&index_def, 8, 42, sketch_bytes + sizeof(void *));
	ok(summary != NULL,
	   "unsigned native key hash is accepted by the index summary");

	char tuple_data_buf[16];
	char *pos = mp_encode_array(tuple_data_buf, 1);
	pos = mp_encode_uint(pos, 7);
	struct tuple *first = tuple_new(tuple_format_runtime, tuple_data_buf, pos);
	pos = mp_encode_array(tuple_data_buf, 1);
	pos = mp_encode_uint(pos, 7);
	struct tuple *same = tuple_new(tuple_format_runtime, tuple_data_buf, pos);
	pos = mp_encode_array(tuple_data_buf, 1);
	pos = mp_encode_uint(pos, 8);
	struct tuple *other = tuple_new(tuple_format_runtime, tuple_data_buf, pos);
	double ndv[1] = {};
	bool values_ok = summary != NULL && first != NULL && same != NULL &&
		other != NULL &&
		sql_stats_index_summary_consume(summary, tuple_data(first),
			tuple_bsize(first), NULL, 0) == 0 &&
		sql_stats_index_summary_consume(summary, tuple_data(same),
			tuple_bsize(same), NULL, 0) == 0 &&
		sql_stats_index_summary_consume(summary, tuple_data(other),
			tuple_bsize(other), NULL, 0) == 0 &&
		sql_stats_index_summary_prefix_ndv(summary, 1, ndv, 1) == 0 &&
		ndv[0] > 1.5 && ndv[0] < 2.5;
	ok(values_ok,
	   "unsigned hash deduplicates repeats and distinguishes values");

	struct key_part_def signed_part = key_part_def_default;
	signed_part.fieldno = 0;
	signed_part.type = FIELD_TYPE_INTEGER;
	struct key_def *signed_key_def = key_def_new(&signed_part, 1, 0);
	struct index_def signed_index_def = {
		.type = TREE,
		.key_def = signed_key_def,
	};
	struct sql_stats_index_summary *signed_summary = signed_key_def == NULL ?
		NULL : sql_stats_index_summary_new_for_index(tuple_format_runtime,
			&signed_index_def, 8, 42, sketch_bytes + sizeof(void *));
	ok(signed_key_def != NULL && signed_summary == NULL,
	   "signed integer remains rejected without a proven hash contract");

	if (first != NULL)
		tuple_delete(first);
	if (same != NULL)
		tuple_delete(same);
	if (other != NULL)
		tuple_delete(other);
	sql_stats_index_summary_delete(summary);
	sql_stats_index_summary_delete(signed_summary);
	if (key_def != NULL)
		key_def_delete(key_def);
	if (signed_key_def != NULL)
		key_def_delete(signed_key_def);
	footer();
	int rc = check_plan();
	tuple_free();
	coll_free();
	fiber_free();
	memory_free();
	return rc;
}
