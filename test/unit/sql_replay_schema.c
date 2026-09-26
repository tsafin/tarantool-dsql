#include <stdlib.h>
#include <string.h>

#include "box/field_def.h"
#include "box/index.h"
#include "box/index_def.h"
#include "box/space.h"
#include "box/sql/sql_replay_schema.h"
#include "unit.h"

const char *field_type_strs[] = {
	"any", "unsigned", "string", "number", "double", "integer",
	"boolean", "varbinary", "scalar", "decimal", "uuid", "datetime",
	"interval", "array", "map", "int8", "uint8", "int16", "uint16",
	"int32", "uint32", "int64", "uint64", "float32", "float64",
};

const char *index_type_strs[] = {"HASH", "TREE", "BITSET", "RTREE"};

struct coll_id *
coll_by_id(uint32_t id)
{
	(void)id;
	return NULL;
}

static void
test_detached_catalog_schema(void)
{
	plan(6);
	header();
	struct field_def fields[] = {
		{.type = FIELD_TYPE_INTEGER, .name = "id",
		 .nullable_action = ON_CONFLICT_ACTION_ABORT},
		{.type = FIELD_TYPE_STRING, .name = "label",
		 .nullable_action = ON_CONFLICT_ACTION_NONE},
	};
	size_t def_size = sizeof(struct space_def) + sizeof("alpha");
	struct space_def *space_def = calloc(1, def_size);
	struct key_def *key_def = calloc(1, sizeof(*key_def) +
					 2 * sizeof(key_def->parts[0]));
	if (space_def == NULL || key_def == NULL) {
		ok(false, "test catalog fixture allocations succeed");
		free(space_def);
		free(key_def);
		for (int i = 0; i < 5; i++)
			ok(false, "catalog schema assertions require fixture");
		footer();
		check_plan();
		return;
	}
	space_def->id = 1234;
	space_def->fields = fields;
	space_def->field_count = 2;
	strcpy(space_def->name, "alpha");
	key_def->part_count = 2;
	key_def->parts[0] = (struct key_part) {
		.fieldno = 0, .type = FIELD_TYPE_INTEGER, .sort_order = SORT_ORDER_ASC,
		.nullable_action = ON_CONFLICT_ACTION_ABORT,
	};
	key_def->parts[1] = (struct key_part) {
		.fieldno = 0, .type = FIELD_TYPE_INTEGER, .sort_order = SORT_ORDER_DESC,
		.nullable_action = ON_CONFLICT_ACTION_ABORT,
	};
	struct index_def index_def = {
		.iid = 88, .space_id = 1234, .name = "idx_id_twice",
		.type = TREE, .opts = {.is_unique = false,
				      .hint = INDEX_HINT_DEFAULT},
		.key_def = key_def,
	};
	struct index index = {.def = &index_def};
	struct index *indexes[] = {&index};
	struct space space = {
		.def = space_def, .index_count = 1, .index = indexes,
	};
	struct sql_replay_space_schema schema;
	ok(sql_replay_space_schema_create(&space, &schema) == SQL_REPLAY_INPUT_OK,
	   "catalog schema converts into a detached replay relation");
	ok(schema.relation.column_count == 2 &&
	   strcmp(schema.relation.columns[0].type, "integer") == 0 &&
	   strcmp(schema.relation.columns[1].type, "string") == 0 &&
	   strcmp(schema.relation.logical_key, "r0") == 0 &&
	   strstr(schema.relation.canonical_definition, "616c706861") != NULL &&
	   strstr(schema.relation.canonical_definition, "1234") == NULL,
	   "relation schema uses stable logical ordinals, not space IDs");
	ok(schema.relation.index_count == 1 && schema.storage_index_ids[0] == 88 &&
	   strcmp(schema.relation.indexes[0].logical_key, "idx_id_twice") == 0 &&
	   schema.relation.indexes[0].part_count == 2 &&
	   schema.relation.indexes[0].part_columns[0] == 0 &&
	   schema.relation.indexes[0].part_columns[1] == 0 &&
	   strstr(schema.relation.indexes[0].canonical_definition, "88") == NULL,
	   "index definition preserves repeated key parts without serializing IDs");
	ok(!schema.relation.statistics_present &&
	   !schema.relation.indexes[0].statistics_present,
	   "catalog metadata does not fabricate statistics");
	sql_replay_space_schema_destroy(&schema);
	ok(schema.relation.columns == NULL && schema.relation.indexes == NULL &&
	   schema.storage_index_ids == NULL,
	   "detached catalog schema releases every owned value");
	index_def.opts.func_id = 17;
	ok(sql_replay_space_schema_create(&space, &schema) ==
	   SQL_REPLAY_INPUT_INVALID && schema.relation.columns == NULL,
	   "functional index with unmodeled identity fails closed");
	free(key_def);
	free(space_def);
	footer();
	check_plan();
}

int
main(void)
{
	test_detached_catalog_schema();
	return 0;
}
