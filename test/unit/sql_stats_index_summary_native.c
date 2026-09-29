#include <stdint.h>

#include "coll/coll.h"
#include "fiber.h"
#include "key_def.h"
#include "memory.h"
#include "msgpuck.h"
#include "box/sql/sql_stats_index_summary.h"
#include "tuple.h"
#include "tuple_compare.h"
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
	plan(6);
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
		sql_stats_index_summary_new_for_index_with_mcv(tuple_format_runtime,
			&index_def, 8, 42, 16384, 4, 32);
	ok(summary != NULL,
	   "unsigned native key hash and MCV are accepted by the index summary");

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
	bool found_seven = false;
	for (uint32_t i = 0; summary != NULL &&
	     i < sql_stats_index_summary_mcv_count(summary, 0); i++) {
		uint8_t type_tag;
		const void *value;
		size_t value_size;
		struct sql_stats_spacesaving_entry entry;
		if (sql_stats_index_summary_mcv_at(summary, 0, i, &type_tag,
							   &value, &value_size,
							   &entry) == 0 &&
		    type_tag == FIELD_TYPE_UNSIGNED + 1 && value_size > 0) {
			const char *end = value;
			uint64_t decoded = mp_decode_uint(&end);
			if (end == (const char *)value + value_size && decoded == 7 &&
			    entry.estimate == 2 && entry.error == 0)
				found_seven = true;
		}
	}
	ok(found_seven &&
	   sql_stats_index_summary_mcv_sample_nonnull_rows(summary, 0) == 3 &&
	   sql_stats_index_summary_sample_rows(summary) == 3,
	   "native MCV retains canonical typed values and sample denominators");

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
	ok(signed_key_def != NULL && signed_summary != NULL,
	   "signed integer native hash is accepted with canonical MessagePack values");

	char signed_tuple_data_buf[16];
	char *signed_pos = mp_encode_array(signed_tuple_data_buf, 1);
	signed_pos = mp_encode_int(signed_pos, -7);
	struct tuple *negative = tuple_new(tuple_format_runtime,
					   signed_tuple_data_buf, signed_pos);
	signed_pos = mp_encode_array(signed_tuple_data_buf, 1);
	signed_pos = mp_encode_int(signed_pos, -7);
	struct tuple *negative_same = tuple_new(tuple_format_runtime,
						 signed_tuple_data_buf, signed_pos);
	signed_pos = mp_encode_array(signed_tuple_data_buf, 1);
	signed_pos = mp_encode_uint(signed_pos, 7);
	struct tuple *positive = tuple_new(tuple_format_runtime,
					   signed_tuple_data_buf, signed_pos);
	double signed_ndv[1] = {};
	bool signed_values_ok = signed_summary != NULL && negative != NULL &&
		negative_same != NULL && positive != NULL &&
		tuple_compare(negative, HINT_NONE, negative_same, HINT_NONE,
			signed_key_def) == 0 &&
		tuple_compare(negative, HINT_NONE, positive, HINT_NONE,
			signed_key_def) != 0 &&
		sql_stats_index_summary_consume(signed_summary, tuple_data(negative),
			tuple_bsize(negative), NULL, 0) == 0 &&
		sql_stats_index_summary_consume(signed_summary,
			tuple_data(negative_same), tuple_bsize(negative_same),
			NULL, 0) == 0 &&
		sql_stats_index_summary_consume(signed_summary, tuple_data(positive),
			tuple_bsize(positive), NULL, 0) == 0 &&
		sql_stats_index_summary_prefix_ndv(signed_summary, 1, signed_ndv,
						    1) == 0 &&
		signed_ndv[0] > 1.5 && signed_ndv[0] < 2.5;
	ok(signed_values_ok,
	   "integer hash deduplicates signed values and distinguishes signs");
	if (negative != NULL)
		tuple_delete(negative);
	if (negative_same != NULL)
		tuple_delete(negative_same);
	if (positive != NULL)
		tuple_delete(positive);

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
