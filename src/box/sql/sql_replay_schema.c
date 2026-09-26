#include "sql_replay_schema.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box/coll_id_cache.h"
#include "box/field_def.h"
#include "box/index.h"
#include "box/index_def.h"
#include "box/space.h"
#include "sqlInt.h"

struct replay_string {
	char *data;
	size_t size;
	size_t capacity;
};

static bool
reserve(struct replay_string *s, size_t extra)
{
	if (extra > SIZE_MAX - s->size - 1)
		return false;
	size_t needed = s->size + extra + 1;
	if (needed <= s->capacity)
		return true;
	size_t capacity = s->capacity == 0 ? 128 : s->capacity;
	while (capacity < needed) {
		if (capacity > SIZE_MAX / 2) {
			capacity = needed;
			break;
		}
		capacity *= 2;
	}
	char *data = realloc(s->data, capacity);
	if (data == NULL)
		return false;
	s->data = data;
	s->capacity = capacity;
	return true;
}

static bool
append_n(struct replay_string *s, const char *value, size_t size)
{
	if (!reserve(s, size))
		return false;
	memcpy(s->data + s->size, value, size);
	s->size += size;
	s->data[s->size] = '\0';
	return true;
}

static bool
append(struct replay_string *s, const char *value)
{
	return append_n(s, value, strlen(value));
}

static bool
appendf(struct replay_string *s, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	int n = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (n < 0 || !reserve(s, (size_t)n)) {
		va_end(args);
		return false;
	}
	vsnprintf(s->data + s->size, s->capacity - s->size, format, args);
	va_end(args);
	s->size += n;
	return true;
}

static bool
append_hex(struct replay_string *s, const char *value, size_t size)
{
	static const char hex[] = "0123456789abcdef";
	if (size > SIZE_MAX / 2 || !reserve(s, size * 2))
		return false;
	for (size_t i = 0; i < size; i++) {
		unsigned char c = value[i];
		s->data[s->size++] = hex[c >> 4];
		s->data[s->size++] = hex[c & 0xf];
	}
	s->data[s->size] = '\0';
	return true;
}

static const char *
collation_name(uint32_t id)
{
	if (id == COLL_NONE)
		return "binary";
	struct coll_id *collation = coll_by_id(id);
	return collation == NULL ? NULL : collation->name;
}

static const char *
hint_name(enum index_hint_cfg hint)
{
	switch (hint) {
	case INDEX_HINT_DEFAULT:
		return "default";
	case INDEX_HINT_ON:
		return "on";
	case INDEX_HINT_OFF:
		return "off";
	}
	return NULL;
}

static void
destroy_partial(struct sql_replay_space_schema *schema)
{
	free((void *)schema->relation.logical_key);
	free((void *)schema->relation.canonical_definition);
	for (size_t i = 0; schema->relation.columns != NULL &&
	     i < schema->relation.column_count; i++) {
		free((void *)schema->relation.columns[i].type);
		free((void *)schema->relation.columns[i].collation);
	}
	free((void *)schema->relation.columns);
	for (size_t i = 0; schema->relation.indexes != NULL &&
	     i < schema->relation.index_count; i++) {
		free((void *)schema->relation.indexes[i].logical_key);
		free((void *)schema->relation.indexes[i].canonical_definition);
		free((void *)schema->relation.indexes[i].part_columns);
	}
	free((void *)schema->relation.indexes);
	free(schema->storage_index_ids);
	*schema = (struct sql_replay_space_schema){};
}

void
sql_replay_space_schema_destroy(struct sql_replay_space_schema *schema)
{
	if (schema != NULL)
		destroy_partial(schema);
}

static bool
encode_relation_definition(const struct space_def *def,
			   struct replay_string *out)
{
	if (!append(out, "table(name=") ||
	    !append_hex(out, def->name, strlen(def->name)) ||
	    !append(out, ";fields=["))
		return false;
	for (uint32_t i = 0; i < def->field_count; i++) {
		const struct field_def *field = &def->fields[i];
		const char *collation = collation_name(field->coll_id);
		if (field->type >= field_type_MAX || collation == NULL ||
		    field->nullable_action >= on_conflict_action_MAX ||
		    field->name == NULL)
			return false;
		if (i != 0 && !append(out, ";"))
			return false;
		if (!appendf(out, "%u:", i) ||
		    !append_hex(out, field->name, strlen(field->name)) ||
		    !appendf(out, ":%s:", field_type_strs[field->type]) ||
		    !append_hex(out, collation, strlen(collation)) ||
		    !appendf(out, ":%u:%u", field->is_nullable,
			 (unsigned int)field->nullable_action))
			return false;
	}
	return append(out, "])");
}

static bool
encode_index_definition(const struct index_def *def, uint32_t field_count,
			struct replay_string *out, uint32_t *parts)
{
	const struct key_def *key = def->key_def;
	const char *hint = hint_name(def->opts.hint);
	if (key == NULL || key->part_count == 0 ||
	    key->for_func_index || key->is_multikey || def->opts.func_id != 0 ||
	    def->type >= index_type_MAX || hint == NULL)
		return false;
	if (!append(out, "index(name=") ||
	    !append_hex(out, def->name, strlen(def->name)) ||
	    !appendf(out, ";type=%s;unique=%u;hint=%s;dimension=%lld;distance=%d;parts=[",
		     index_type_strs[def->type], def->opts.is_unique, hint,
		     (long long)def->opts.dimension, (int)def->opts.distance))
		return false;
	for (uint32_t i = 0; i < key->part_count; i++) {
		const struct key_part *part = &key->parts[i];
		const char *collation = collation_name(part->coll_id);
		if (part->fieldno >= field_count || part->type >= field_type_MAX ||
		    collation == NULL || part->sort_order >= sort_order_MAX ||
		    part->nullable_action >= on_conflict_action_MAX ||
		    (part->path_len != 0 && part->path == NULL))
			return false;
		parts[i] = part->fieldno;
		if (i != 0 && !append(out, ";"))
			return false;
		if (!appendf(out, "%u:%s:", part->fieldno,
			     field_type_strs[part->type]) ||
		    !append_hex(out, collation, strlen(collation)) ||
		    !appendf(out, ":%d:%u:%u:", (int)part->sort_order,
			 part->nullable_action, part->exclude_null) ||
		    !append_hex(out, part->path, part->path_len))
			return false;
	}
	return append(out, "])");
}

enum sql_replay_input_status
sql_replay_space_schema_create(const struct space *space,
			       struct sql_replay_space_schema *result)
{
	if (result == NULL)
		return SQL_REPLAY_INPUT_INVALID;
	*result = (struct sql_replay_space_schema){};
	if (space == NULL || space->def == NULL || space->def->opts.is_view ||
	    space->def->name[0] == '\0' || space->def->field_count == 0 ||
	    space->def->fields == NULL ||
	    (space->index_count != 0 && space->index == NULL))
		return SQL_REPLAY_INPUT_INVALID;
	struct sql_replay_space_schema schema = {};
	schema.relation.logical_key = strdup("r0");
	schema.relation.column_count = space->def->field_count;
	schema.relation.index_count = space->index_count;
	schema.relation.columns = calloc(schema.relation.column_count,
					 sizeof(*schema.relation.columns));
	if (schema.relation.index_count != 0) {
		schema.relation.indexes = calloc(schema.relation.index_count,
						 sizeof(*schema.relation.indexes));
		schema.storage_index_ids = calloc(schema.relation.index_count,
						   sizeof(*schema.storage_index_ids));
	}
	if (schema.relation.logical_key == NULL ||
	    schema.relation.columns == NULL ||
	    (schema.relation.index_count != 0 &&
	     (schema.relation.indexes == NULL ||
	      schema.storage_index_ids == NULL)))
		goto nomem;
	struct replay_string relation_definition = {};
	if (!encode_relation_definition(space->def, &relation_definition)) {
		free(relation_definition.data);
		goto invalid;
	}
	schema.relation.canonical_definition = relation_definition.data;
	struct sql_replay_column_spec *columns =
		(struct sql_replay_column_spec *)schema.relation.columns;
	for (size_t i = 0; i < schema.relation.column_count; i++) {
		const struct field_def *field = &space->def->fields[i];
		const char *collation = collation_name(field->coll_id);
		if (field->type >= field_type_MAX || collation == NULL ||
		    field->name == NULL)
			goto invalid;
		columns[i].type = strdup(field_type_strs[field->type]);
		columns[i].collation = strdup(collation);
		if (columns[i].type == NULL || columns[i].collation == NULL)
			goto nomem;
	}
	for (size_t i = 0; i < schema.relation.index_count; i++) {
		const struct index *index = space->index[i];
		if (index == NULL || index->def == NULL ||
		    index->def->space_id != space->def->id ||
		    index->def->name == NULL || index->def->name[0] == '\0')
			goto invalid;
		struct sql_replay_index_spec *out =
			&((struct sql_replay_index_spec *)schema.relation.indexes)[i];
		out->logical_key = strdup(index->def->name);
		out->part_count = index->def->key_def == NULL ? 0 :
			index->def->key_def->part_count;
		if (out->logical_key == NULL)
			goto nomem;
		if (out->part_count == 0 ||
		    out->part_count > SIZE_MAX / sizeof(*out->part_columns))
			goto invalid;
		uint32_t *parts = calloc(out->part_count, sizeof(*parts));
		if (parts == NULL)
			goto nomem;
		out->part_columns = parts;
		struct replay_string definition = {};
		if (!encode_index_definition(index->def, schema.relation.column_count,
					     &definition, parts)) {
			free(definition.data);
			goto invalid;
		}
		out->canonical_definition = definition.data;
		schema.storage_index_ids[i] = index->def->iid;
	}
	*result = schema;
	return SQL_REPLAY_INPUT_OK;
nomem:
	destroy_partial(&schema);
	return SQL_REPLAY_INPUT_NOMEM;
invalid:
	destroy_partial(&schema);
	return SQL_REPLAY_INPUT_INVALID;
}
