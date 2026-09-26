#include "sql_plan_lowering.h"

static int
emit_event(sql_plan_lowering_emit_f emit, void *context,
	   const struct sql_plan_descriptor_input *input,
	   enum sql_plan_lowering_stage stage,
	   const struct sql_plan_filter *filter,
	   const struct sql_plan_finalize *finalize)
{
	const struct sql_plan_lowering_event event = {
		.stage = stage,
		.plan = input,
		.filter = filter,
		.finalize = finalize,
	};
	return emit(context, &event);
}

int
sql_plan_lower(const struct sql_plan_descriptor *plan,
	       sql_plan_lowering_emit_f emit, void *context)
{
	if (plan == NULL || emit == NULL)
		return -1;
	const struct sql_plan_descriptor_input *input =
		sql_plan_descriptor_get_input(plan);
	if (input == NULL || input->path_class != SQL_PLAN_NEW_PLANNER)
		return -1;
	int rc = emit_event(emit, context, input, SQL_PLAN_LOWER_SCAN, NULL,
			    NULL);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < input->filter_count; ++i) {
		rc = emit_event(emit, context, input, SQL_PLAN_LOWER_FILTER,
				&input->filters[i], NULL);
		if (rc != 0)
			return rc;
	}
	rc = emit_event(emit, context, input, SQL_PLAN_LOWER_PROJECT, NULL,
			NULL);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < input->finalize_count; ++i) {
		enum sql_plan_lowering_stage stage =
			input->finalize[i].kind == SQL_PLAN_SORT ?
			SQL_PLAN_LOWER_SORT : SQL_PLAN_LOWER_LIMIT;
		rc = emit_event(emit, context, input, stage, NULL,
				&input->finalize[i]);
		if (rc != 0)
			return rc;
	}
	return emit_event(emit, context, input, SQL_PLAN_LOWER_RESULT, NULL,
			   NULL);
}
