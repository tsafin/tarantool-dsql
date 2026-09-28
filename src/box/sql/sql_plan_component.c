#include "sql_plan_component.h"

#include <string.h>

void
sql_plan_component_ledger_create(struct sql_plan_component_ledger *ledger)
{
	memset(ledger, 0, sizeof(*ledger));
}

static struct sql_plan_component_record *
find_component(struct sql_plan_component_ledger *ledger, uint32_t id)
{
	for (size_t i = 0; i < ledger->count; i++) {
		if (ledger->records[i].id == id)
			return &ledger->records[i];
	}
	return NULL;
}

enum sql_plan_component_status
sql_plan_component_add(struct sql_plan_component_ledger *ledger, uint32_t id,
		       uint32_t parent_id, enum sql_plan_component_role role)
{
	if (ledger == NULL || id == 0 || role < SQL_PLAN_COMPONENT_ROOT ||
	    role >= SQL_PLAN_COMPONENT_ROLE_COUNT)
		return SQL_PLAN_COMPONENT_INVALID;
	if (ledger->overflowed)
		return SQL_PLAN_COMPONENT_OVERFLOW;
	if (find_component(ledger, id) != NULL)
		return SQL_PLAN_COMPONENT_DUPLICATE_ID;
	bool is_root = role == SQL_PLAN_COMPONENT_ROOT ||
		role == SQL_PLAN_COMPONENT_INSERT_SELECT_ROOT ||
		role == SQL_PLAN_COMPONENT_DML_VIEW_MATERIALIZATION_ROOT ||
		role == SQL_PLAN_COMPONENT_TRIGGER_SELECT_ROOT;
	if ((parent_id == 0) != is_root)
		return SQL_PLAN_COMPONENT_INVALID;
	if (parent_id == 0 && ledger->count != 0)
		return SQL_PLAN_COMPONENT_INVALID;
	if (parent_id != 0 && find_component(ledger, parent_id) == NULL)
		return SQL_PLAN_COMPONENT_MISSING_PARENT;
	if (ledger->count == SQL_PLAN_COMPONENT_MAX) {
		ledger->overflowed = 1;
		return SQL_PLAN_COMPONENT_OVERFLOW;
	}
	ledger->records[ledger->count++] = (struct sql_plan_component_record) {
		.id = id,
		.parent_id = parent_id,
		.role = role,
		.route = SQL_PLAN_COMPONENT_PENDING,
		.fallback_reason = SQL_PLAN_FALLBACK_NONE,
	};
	return SQL_PLAN_COMPONENT_OK;
}

enum sql_plan_component_status
sql_plan_component_set_route(struct sql_plan_component_ledger *ledger,
			     uint32_t id,
			     enum sql_plan_component_route route,
			     enum sql_plan_fallback_reason fallback_reason)
{
	if (ledger == NULL || route <= SQL_PLAN_COMPONENT_PENDING ||
	    route >= SQL_PLAN_COMPONENT_MIXED ||
	    fallback_reason < SQL_PLAN_FALLBACK_NONE ||
	    fallback_reason >= SQL_PLAN_FALLBACK_REASON_COUNT)
		return SQL_PLAN_COMPONENT_INVALID;
	struct sql_plan_component_record *record = find_component(ledger, id);
	if (record == NULL)
		return SQL_PLAN_COMPONENT_INVALID;
	if ((route == SQL_PLAN_COMPONENT_FALLBACK) !=
	    (fallback_reason != SQL_PLAN_FALLBACK_NONE) ||
	    (fallback_reason != SQL_PLAN_FALLBACK_NONE &&
	     sql_plan_fallback_reason_name(fallback_reason) == NULL))
		return SQL_PLAN_COMPONENT_INVALID;
	if (record->route != SQL_PLAN_COMPONENT_PENDING) {
		if (record->route == route &&
		    record->fallback_reason == fallback_reason)
			return SQL_PLAN_COMPONENT_OK;
		return SQL_PLAN_COMPONENT_CONFLICT;
	}
	record->route = route;
	record->fallback_reason = fallback_reason;
	return SQL_PLAN_COMPONENT_OK;
}

bool
sql_plan_component_route_is_pending(
	const struct sql_plan_component_ledger *ledger, uint32_t id)
{
	if (ledger == NULL)
		return false;
	for (size_t i = 0; i < ledger->count; i++) {
		if (ledger->records[i].id == id)
			return ledger->records[i].route == SQL_PLAN_COMPONENT_PENDING;
	}
	return false;
}

enum sql_plan_component_status
sql_plan_component_finalize(const struct sql_plan_component_ledger *ledger,
			    struct sql_plan_component_summary *summary)
{
	if (ledger == NULL || summary == NULL || ledger->count == 0 ||
	    ledger->overflowed)
		return SQL_PLAN_COMPONENT_INCOMPLETE;
	const struct sql_plan_component_record *root = NULL;
	for (size_t i = 0; i < ledger->count; i++) {
		const struct sql_plan_component_record *record = &ledger->records[i];
		if (record->route == SQL_PLAN_COMPONENT_PENDING)
			return SQL_PLAN_COMPONENT_INCOMPLETE;
		if (record->role == SQL_PLAN_COMPONENT_ROOT ||
		    record->role == SQL_PLAN_COMPONENT_INSERT_SELECT_ROOT ||
		    record->role ==
		    SQL_PLAN_COMPONENT_DML_VIEW_MATERIALIZATION_ROOT ||
		    record->role == SQL_PLAN_COMPONENT_TRIGGER_SELECT_ROOT)
			root = record;
	}
	if (root == NULL) {
		*summary = (struct sql_plan_component_summary) {
			.route = SQL_PLAN_COMPONENT_MIXED,
			.fallback_reason = SQL_PLAN_FALLBACK_NONE,
			.component_count = ledger->count,
		};
		return SQL_PLAN_COMPONENT_OK;
	}
	enum sql_plan_component_route route = root->route;
	enum sql_plan_fallback_reason reason = root->fallback_reason;
	for (size_t i = 0; i < ledger->count; i++) {
		const struct sql_plan_component_record *record = &ledger->records[i];
		if (record->route != route) {
			route = SQL_PLAN_COMPONENT_MIXED;
			reason = SQL_PLAN_FALLBACK_NONE;
			break;
		}
	}
	*summary = (struct sql_plan_component_summary) {
		.route = route,
		.fallback_reason = reason,
		.component_count = ledger->count,
	};
	return SQL_PLAN_COMPONENT_OK;
}

const char *
sql_plan_component_role_name(enum sql_plan_component_role role)
{
	static const char *const names[] = {
		"root", "compound_branch", "recursive_anchor",
		"recursive_term", "from_subquery", "scalar_subquery",
		"subquery", "values", "count", "cte",
		"expression_subquery", "insert_select_root",
		"dml_view_materialization_root", "trigger_select_root",
		"trigger_select",
	};
	return role >= SQL_PLAN_COMPONENT_ROOT &&
		role < SQL_PLAN_COMPONENT_ROLE_COUNT ? names[role] : NULL;
}

const char *
sql_plan_component_route_name(enum sql_plan_component_route route)
{
	static const char *const names[] = {
		"pending", "current_where_c", "new_planner", "fallback",
		"direct_values", "direct_op_count", "compound_dispatch", "mixed",
	};
	return route >= SQL_PLAN_COMPONENT_PENDING &&
		route < SQL_PLAN_COMPONENT_ROUTE_COUNT ? names[route] : NULL;
}
