#include <stdlib.h>
#include <string.h>
#include "box/sql/sql_replay_input.h"
#include "unit.h"

static void test_detached_copy(void)
{
	plan(5); header();
	char relation[] = "logical-r0", predicate[] = "eq(col(r0,c0),int(7))";
	char index[] = "logical-i0", definition[] = "key(c0:int:binary;unique=false)";
	struct sql_replay_input_spec spec = {relation, predicate, index, definition,
		true, 12, 8, 1, 1, 32};
	struct sql_replay_input *input = NULL;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_OK,
	   "valid logical input accepted");
	relation[0] = 'X'; predicate[0] = 'X'; index[0] = 'X'; definition[0] = 'X';
	ok(strcmp(input->relation_key, "logical-r0") == 0 &&
	   strcmp(input->predicate, "eq(col(r0,c0),int(7))") == 0,
	   "input owns relation and expression bytes");
	ok(strcmp(input->index_key, "logical-i0") == 0 &&
	   strcmp(input->index_definition,
		  "key(c0:int:binary;unique=false)") == 0,
	   "input owns logical access-path metadata");
	ok(input->statistics_present && input->row_count == 12 &&
	   input->average_row_width == 8 && input->beam_width == 32,
	   "statistics and planner config captured by value");
	sql_replay_input_delete(input);
	input = NULL;
	spec.index_key = "orphan"; spec.index_definition = NULL;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_INVALID &&
	   input == NULL, "incomplete candidate metadata rejected");
	footer(); check_plan();
}

static void test_absent_stats(void)
{
	plan(2); header();
	struct sql_replay_input_spec spec = {"r0", "true", NULL, NULL,
		false, 0, 0, 1, 1, 1};
	struct sql_replay_input *input = NULL;
	ok(sql_replay_input_create(&spec, &input) == SQL_REPLAY_INPUT_OK,
	   "explicitly absent statistics accepted");
	ok(input != NULL && !input->statistics_present,
	   "absence distinguished from measured zero");
	sql_replay_input_delete(input);
	footer(); check_plan();
}

int main(void) { test_detached_copy(); test_absent_stats(); return 0; }
