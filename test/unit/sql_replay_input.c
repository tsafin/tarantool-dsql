#include <stdlib.h>
#include <string.h>

#include "box/sql/sql_replay_input.h"
#include "box/sql/sql_replay_extract.h"
#include "box/sql/sqlInt.h"
#include "box/space.h"
#include "box/coll_id_cache.h"
#include "msgpuck.h"
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
test_extract_resolved_select(void)
{
	plan(9);
	header();
	struct sql_replay_column_spec column = {"integer", "binary"};
	struct sql_replay_input_spec metadata = {
		.relation = {
			.logical_key = "logical-r0",
			.canonical_definition = "table(c0:integer)",
			.columns = &column, .column_count = 1,
		},
		/* These expression fields must be replaced by Select extraction. */
		.predicate = "SQL text", .planner_algorithm_version = 1,
		.planner_config_version = 1, .beam_width = 1,
	};
	const uint32_t cursor_map[] = {UINT32_MAX, 0};
	struct Expr column_expr = {
		.op = TK_COLUMN_REF, .flags = EP_Resolved, .iTable = 1,
		.iColumn = 0,
	};
	struct Expr one = {
		.op = TK_INTEGER, .flags = EP_Resolved | EP_IntValue,
		.u.iValue = 1,
	};
	struct Expr predicate = {
		.op = TK_EQ, .flags = EP_Resolved, .pLeft = &column_expr,
		.pRight = &one,
	};
	struct Expr limit = {
		.op = TK_INTEGER, .flags = EP_Resolved | EP_IntValue,
		.u.iValue = 10,
	};
	struct Expr offset = {
		.op = TK_INTEGER, .flags = EP_Resolved | EP_IntValue,
		.u.iValue = 2,
	};
	struct ExprList_item projection_items[] = {{.pExpr = &column_expr}};
	struct ExprList projection = {.nExpr = 1, .a = projection_items};
	struct ExprList_item order_items[] = {{
		.pExpr = &column_expr, .sort_order = SORT_ORDER_DESC,
	}};
	struct ExprList order = {.nExpr = 1, .a = order_items};
	struct space_def space_definition = {.field_count = 1};
	struct space source_space = {.def = &space_definition};
	struct SrcList sources = {
		.nSrc = 1,
		.a = {{.space = &source_space, .iCursor = 1}},
	};
	struct Select select = {
		.pEList = &projection, .pSrc = &sources, .pWhere = &predicate,
		.pOrderBy = &order, .pLimit = &limit, .pOffset = &offset,
		.selFlags = SF_Resolved,
	};
	struct sql_replay_input *input = NULL;
	ok(sql_replay_input_extract_select(&select, &metadata, cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_OK &&
	   input != NULL && strcmp(input->predicate,
				   "eq(col(r0,c0),int(1))") == 0 &&
	   input->projection_count == 1 &&
	   strcmp(input->projections[0], "col(r0,c0)") == 0,
	   "resolved single-table SELECT expressions populate detached replay input");
	column_expr.iColumn = 9;
	ok(input != NULL && strcmp(input->projections[0], "col(r0,c0)") == 0 &&
	   input->order_by_count == 1 && input->order_by[0].descending &&
	   !input->order_by[0].nulls_first && input->limit_present &&
	   input->limit == 10 && input->offset_present && input->offset == 2,
	   "order, limits, and expressions are detached from the SQL AST");
	if (input != NULL)
		sql_replay_input_delete(input);
	column_expr.iColumn = 0;
	select.pWhere = NULL;
	select.pOrderBy = NULL;
	select.pLimit = NULL;
	select.pOffset = NULL;
	input = NULL;
	ok(sql_replay_input_extract_select(&select, &metadata, cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_OK &&
	   input != NULL && strcmp(input->predicate, "int(1)") == 0 &&
	   input->order_by_count == 0 && !input->limit_present,
	   "absent WHERE and optional clauses extract with explicit truth predicate");
	if (input != NULL)
		sql_replay_input_delete(input);
	struct Expr function = {
		.op = TK_FUNCTION, .flags = EP_Resolved | EP_ConstFunc,
		.u.zToken = "abs",
	};
	select.pWhere = &function;
	input = NULL;
	ok(sql_replay_input_extract_select(&select, &metadata, cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL,
	   "unsupported scalar function rejects SELECT extraction without output");
	select.pWhere = NULL;
	struct Expr negative_limit = {
		.op = TK_UMINUS, .flags = EP_Resolved, .pLeft = &one,
	};
	select.pLimit = &negative_limit;
	ok(sql_replay_input_extract_select(&select, &metadata, cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL,
	   "negative LIMIT is rejected rather than reinterpreted as unsigned");
	select.pLimit = NULL;
	const uint32_t invalid_cursor_map[] = {0, UINT32_MAX};
	ok(sql_replay_input_extract_select(&select, &metadata, invalid_cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL,
	   "cursor bindings must identify the source as logical relation zero");
	sources.a[0].fg.notIndexed = true;
	ok(sql_replay_input_extract_select(&select, &metadata, cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL,
	   "unmodeled NOT INDEXED access restriction is rejected");
	sources.a[0].fg.notIndexed = false;
	space_definition.opts.is_view = true;
	ok(sql_replay_input_extract_select(&select, &metadata, cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL,
	   "views are outside the base-relation replay model");
	space_definition.opts.is_view = false;
	select.selFlags |= SF_Distinct;
	ok(sql_replay_input_extract_select(&select, &metadata, cursor_map, 2,
					   &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL,
	   "unsupported DISTINCT shape is rejected by logical-plan validation");
	footer();
	check_plan();
}

static void
test_detached_single_relation_select(void)
{
	plan(8);
	header();
	char relation_key[] = "logical-r0";
	char relation_def[] = "relation(columns=[int,binary;text,binary])";
	char col_type0[] = "integer";
	char col_coll0[] = "binary";
	char col_type1[] = "string";
	char col_coll1[] = "unicode";
	struct sql_replay_column_spec columns[] = {
		{col_type0, col_coll0}, {col_type1, col_coll1},
	};
	char index_key[] = "logical-i0";
	char index_def[] = "key(parts=[c0:int:binary],unique=true)";
	char pop_basis[] = "visible_rows@view-9";
	char ndv_basis[] = "visible_rows@view-9";
	uint32_t part_columns[] = {0};
	uint64_t distinct_prefixes[] = {12};
	struct sql_replay_index_spec indexes[] = {{
		.logical_key = index_key, .canonical_definition = index_def,
		.part_columns = part_columns, .part_count = 1,
		.statistics_present = true, .tuple_count = 12,
		.population_basis = pop_basis, .ndv_basis = ndv_basis,
		.distinct_prefixes = distinct_prefixes, .prefix_count = 1,
	}};
	char relation_pop[] = "visible_rows@view-9";
	char width_basis[] = "payload_bytes/sample_rows";
	char confidence_source[] = "caller-calibrated-v1";
	struct sql_replay_relation_spec relation = {
		.logical_key = relation_key, .canonical_definition = relation_def,
		.columns = columns, .column_count = 2, .indexes = indexes,
		.index_count = 1, .statistics_present = true, .row_count = 12,
		.cardinality_semantics = SQL_REPLAY_CARDINALITY_VISIBLE_ROWS,
		.population_basis = relation_pop, .average_row_width = 8,
		.width_basis = width_basis, .width_denominator_count = 12,
		.confidence_ppm = 900000, .confidence_source = confidence_source,
		.collected_at = 17, .modification_epoch = 3,
	};
	char predicate[] = "eq(col(r0,c0),int(7))";
	char projection0[] = "col(r0,c0)";
	char projection1[] = "col(r0,c1)";
	const char *projections[] = {projection0, projection1};
	char order_expr[] = "col(r0,c1)";
	struct sql_replay_order_spec order[] = {{
		.canonical_expression = order_expr, .descending = true,
		.nulls_first = false,
	}};
	struct sql_replay_input_spec spec = {
		.relation = relation, .predicate = predicate,
		.projections = projections, .projection_count = 2,
		.order_by = order, .order_by_count = 1,
		.limit_present = true, .limit = 10,
		.offset_present = true, .offset = 2,
		.planner_algorithm_version = 1, .planner_config_version = 2,
		.beam_width = 8,
	};
	struct sql_replay_input *input = NULL;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_OK,
	   "complete single-relation SELECT input accepted");
	if (input == NULL) {
		for (int i = 0; i < 7; i++)
			ok(false, "detached input retains model components");
		footer();
		check_plan();
		return;
	}
	strcpy(relation_key, "X");
	strcpy(relation_def, "X");
	strcpy(col_type0, "X");
	strcpy(col_coll1, "X");
	strcpy(index_key, "X");
	strcpy(index_def, "X");
	strcpy(pop_basis, "X");
	strcpy(ndv_basis, "X");
	strcpy(relation_pop, "X");
	strcpy(width_basis, "X");
	strcpy(confidence_source, "X");
	strcpy(predicate, "X");
	strcpy(projection0, "X");
	strcpy(order_expr, "X");
	part_columns[0] = 1;
	distinct_prefixes[0] = 99;
	ok(strcmp(input->relation_key, "logical-r0") == 0 &&
	   strcmp(input->relation_definition,
		  "relation(columns=[int,binary;text,binary])") == 0 &&
	   strcmp(input->columns[0].type, "integer") == 0 &&
	   strcmp(input->columns[1].collation, "unicode") == 0,
	   "relation, schema types, and collations are detached copies");
	ok(input->index_count == 1 &&
	   strcmp(input->indexes[0].logical_key, "logical-i0") == 0 &&
	   strcmp(input->indexes[0].canonical_definition,
		  "key(parts=[c0:int:binary],unique=true)") == 0 &&
	   input->indexes[0].part_columns[0] == 0,
	   "logical index definition and part ordinals are retained");
	ok(input->statistics_present && input->row_count == 12 &&
	   input->cardinality_semantics == SQL_REPLAY_CARDINALITY_VISIBLE_ROWS &&
	   strcmp(input->population_basis, "visible_rows@view-9") == 0 &&
	   input->average_row_width == 8 && input->width_denominator_count == 12 &&
	   input->confidence_ppm == 900000 && input->collected_at == 17 &&
	   input->modification_epoch == 3,
	   "relation statistics carry semantics, provenance, and freshness");
	ok(input->indexes[0].statistics_present &&
	   input->indexes[0].tuple_count == 12 &&
	   strcmp(input->indexes[0].population_basis,
		  "visible_rows@view-9") == 0 &&
	   strcmp(input->indexes[0].ndv_basis,
		  "visible_rows@view-9") == 0 &&
	   input->indexes[0].distinct_prefixes[0] == 12,
	   "index population and prefix NDV are copied with semantics");
	ok(strcmp(input->predicate, "eq(col(r0,c0),int(7))") == 0 &&
	   input->projection_count == 2 &&
	   strcmp(input->projections[0], "col(r0,c0)") == 0 &&
	   input->order_by_count == 1 &&
	   strcmp(input->order_by[0].canonical_expression, "col(r0,c1)") == 0 &&
	   input->order_by[0].descending && !input->order_by[0].nulls_first &&
	   input->limit == 10 && input->offset == 2,
	   "predicate, projection, order, limit, and offset are detached");	ok(input->planner_algorithm_version == 1 &&
	   input->planner_config_version == 2 && input->beam_width == 8,
	   "planner algorithm and configuration are captured");
	sql_replay_input_delete(input);
	input = NULL;

	indexes[0].part_columns = part_columns;
	indexes[0].logical_key = "logical-i0";
	indexes[0].canonical_definition = "key(c0)";
	indexes[0].population_basis = "visible";
	indexes[0].ndv_basis = "visible";
	indexes[0].distinct_prefixes = distinct_prefixes;
	indexes[0].prefix_count = 1;
	indexes[0].part_count = 1;
	indexes[0].statistics_present = true;
	indexes[0].tuple_count = 12;
	columns[0] = (struct sql_replay_column_spec){"integer", "binary"};
	columns[1] = (struct sql_replay_column_spec){"string", "binary"};
	projections[0] = "col(r0,c0)";
	order[0].canonical_expression = "col(r0,c1)";
	relation.logical_key = "r0";
	relation.canonical_definition = "table";
	relation.statistics_present = false;
	relation.row_count = 0;
	relation.cardinality_semantics = 0;
	relation.population_basis = NULL;
	relation.average_row_width = 0;
	relation.width_basis = NULL;
	relation.width_denominator_count = 0;
	relation.confidence_ppm = 0;
	relation.confidence_source = NULL;
	relation.collected_at = 0;
	relation.modification_epoch = 0;
	relation.index_count = 0;
	spec.relation = relation;
	spec.predicate = "eq(int(1),int(1))";
	spec.order_by_count = 0;
	spec.limit_present = false;
	spec.limit = 0;
	spec.offset_present = false;
	spec.offset = 0;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_OK &&
	   input != NULL && !input->statistics_present && input->row_count == 0,
	   "explicitly absent statistics differ from measured zero");
	sql_replay_input_delete(input);
	footer();
	check_plan();
}

static void
test_canonical_expression_grammar(void)
{
	plan(3);
	header();
	struct sql_replay_column_spec column = {"integer", "binary"};
	struct sql_replay_relation_spec relation = {
		.logical_key = "r0", .canonical_definition = "table",
		.columns = &column, .column_count = 1,
	};
	const char *projections[] = {
		"null", "str(6162)", "float(0x1p+0)",
		"plus(int(1),int(2))", "notnull(col(r0,c0))",
	};
	struct sql_replay_input_spec spec = {
		.relation = relation,
		.predicate = "and(eq(col(r0,c0),int(1)),not(isnull(col(r0,c0))))",
		.projections = projections, .projection_count = 5,
		.planner_algorithm_version = 1, .planner_config_version = 1,
		.beam_width = 1,
	};
	struct sql_replay_input *input = NULL;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_OK,
	   "canonical literals, unary/binary operators, and columns are accepted");
	if (input != NULL)
		input->projections[1][strlen(input->projections[1]) - 2] = '\0';
	char *bytes = NULL;
	size_t size = 0;
	ok(input != NULL && sql_replay_input_serialize(input, &bytes, &size) ==
	   SQL_REPLAY_INPUT_INVALID && bytes == NULL && size == 0,
	   "serializer rejects a mutated malformed canonical string");
	free(bytes);
	sql_replay_input_delete(input);
	input = NULL;
	projections[1] = "str(616)";
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL, "string literals require complete lowercase hex pairs");
	footer();
	check_plan();
}

static void
test_rejects_incomplete_or_invalid_inputs(void)
{
	plan(9);
	header();
	struct sql_replay_column_spec column = {"integer", "binary"};
	const char *projection[] = {"col(r0,c0)"};
	struct sql_replay_relation_spec relation = {
		.logical_key = "r0", .canonical_definition = "table",
		.columns = &column, .column_count = 1,
	};
	struct sql_replay_input_spec spec = {
		.relation = relation, .predicate = "eq(int(1),int(1))",
		.projections = projection,
		.projection_count = 1, .planner_algorithm_version = 1,
		.planner_config_version = 1, .beam_width = 1,
	};
	struct sql_replay_input *input = NULL;
	struct sql_replay_index_spec bad_index = {
		.logical_key = "i0", .canonical_definition = "key(c9)",
		.part_columns = (uint32_t[]){9}, .part_count = 1,
	};
	spec.relation.indexes = &bad_index;
	spec.relation.index_count = 1;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL, "out-of-range index part rejected");
	spec.relation.index_count = 0;
	spec.relation.statistics_present = true;
	spec.relation.row_count = 0;
	spec.relation.cardinality_semantics = SQL_REPLAY_CARDINALITY_VISIBLE_ROWS;
	spec.relation.population_basis = "visible";
	spec.relation.width_basis = "bytes/rows";
	spec.relation.width_denominator_count = 0;
	spec.relation.confidence_ppm = 1000001;
	spec.relation.confidence_source = "test";
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "missing width denominator and invalid confidence rejected");
	spec.relation.statistics_present = false;
	spec.relation.row_count = 0;
	spec.relation.cardinality_semantics = 0;
	spec.relation.population_basis = NULL;
	spec.relation.width_basis = NULL;
	spec.relation.width_denominator_count = 0;
	spec.relation.confidence_ppm = 0;
	spec.relation.confidence_source = NULL;
	spec.offset_present = true;
	spec.offset = 1;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "offset without a limit rejected");
	spec.offset_present = false;
	spec.offset = 0;
	spec.relation.columns = NULL;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "missing logical column definitions rejected");
	spec.relation.columns = &column;
	spec.projections = NULL;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "missing normalized projection list rejected");
	spec.projections = projection;
	spec.relation.statistics_present = false;
	spec.relation.row_count = 1;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "statistics absence cannot conceal a nonzero row count");
	spec.relation.row_count = 0;
	spec.predicate = "SELECT 1";
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "SQL text is not accepted as a canonical expression");
	spec.predicate = "call(int(1))";
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "unsupported expression calls are rejected");
	spec.predicate = "eq(col(r0,c1),int(1))";
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID,
	   "out-of-range canonical column references are rejected");
	footer();
	check_plan();
}

static void
test_canonical_msgpack(void)
{
	plan(4);
	header();
	struct sql_replay_column_spec column = {"integer", "binary"};
	uint32_t parts[] = {0, 0};
	struct sql_replay_index_spec indexes[] = {
		{.logical_key = "idx-a", .canonical_definition = "key(c0)",
		 .part_columns = parts, .part_count = 2},
		{.logical_key = "idx-b", .canonical_definition = "key(c0:desc)",
		 .part_columns = parts, .part_count = 1},
	};
	struct sql_replay_relation_spec relation = {
		.logical_key = "r0", .canonical_definition = "table(c0:integer)",
		.columns = &column, .column_count = 1,
		.indexes = indexes, .index_count = 2,
	};
	const char *projection[] = {"col(r0,c0)"};
	struct sql_replay_input_spec spec = {
		.relation = relation, .predicate = "eq(int(1),int(1))",
		.projections = projection,
		.projection_count = 1, .planner_algorithm_version = 1,
		.planner_config_version = 1, .beam_width = 1,
	};
	struct sql_replay_input *a = NULL, *b = NULL;
	char *bytes_a = NULL, *bytes_b = NULL;
	size_t size_a = 0, size_b = 0;
	ok(sql_replay_input_create(&spec, &a) == SQL_REPLAY_INPUT_OK &&
	   sql_replay_input_serialize(a, &bytes_a, &size_a) == SQL_REPLAY_INPUT_OK,
	   "input serializes to owned MsgPack bytes");
	struct sql_replay_index_spec reverse[] = {indexes[1], indexes[0]};
	spec.relation.indexes = reverse;
	ok(sql_replay_input_create(&spec, &b) == SQL_REPLAY_INPUT_OK &&
	   sql_replay_input_serialize(b, &bytes_b, &size_b) == SQL_REPLAY_INPUT_OK &&
	   bytes_a != NULL && bytes_b != NULL && size_a == size_b &&
	   memcmp(bytes_a, bytes_b, size_a) == 0,
	   "index input order canonicalizes to identical MsgPack");
	const char *cursor = bytes_a;
	ok(bytes_a != NULL && mp_check(&cursor, bytes_a + size_a) == 0 &&
	   cursor == bytes_a + size_a,
	   "canonical serialization is one valid MsgPack value");
	if (a != NULL)
		a->indexes[0].tuple_count = 1;
	char *invalid_bytes = NULL;
	size_t invalid_size = 0;
	ok(a != NULL && sql_replay_input_serialize(a, &invalid_bytes,
			&invalid_size) == SQL_REPLAY_INPUT_INVALID &&
	   invalid_bytes == NULL && invalid_size == 0,
	   "serializer rejects inconsistent mutated values without partial output");
	free(invalid_bytes);
	free(bytes_a);
	free(bytes_b);
	sql_replay_input_delete(a);
	sql_replay_input_delete(b);
	footer();
	check_plan();
}

int
main(void)
{
	test_extract_resolved_select();
	test_detached_single_relation_select();
	test_canonical_expression_grammar();
	test_rejects_incomplete_or_invalid_inputs();
	test_canonical_msgpack();
	return 0;
}
