#ifndef TARANTOOL_SQL_PLAN_COMPONENT_H
#define TARANTOOL_SQL_PLAN_COMPONENT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "sql_plan_descriptor.h"

/* Bound planner snapshot diagnostics without truncating reviewed VALUES sets. */
#define SQL_PLAN_COMPONENT_MAX 4096

enum sql_plan_component_role {
	SQL_PLAN_COMPONENT_ROOT = 0,
	SQL_PLAN_COMPONENT_COMPOUND_BRANCH,
	SQL_PLAN_COMPONENT_RECURSIVE_ANCHOR,
	SQL_PLAN_COMPONENT_RECURSIVE_TERM,
	SQL_PLAN_COMPONENT_FROM_SUBQUERY,
	SQL_PLAN_COMPONENT_SCALAR_SUBQUERY,
	SQL_PLAN_COMPONENT_SUBQUERY,
	SQL_PLAN_COMPONENT_VALUES,
	SQL_PLAN_COMPONENT_COUNT,
	SQL_PLAN_COMPONENT_CTE,
	SQL_PLAN_COMPONENT_EXPRESSION_SUBQUERY,
	SQL_PLAN_COMPONENT_INSERT_SELECT_ROOT,
	SQL_PLAN_COMPONENT_DML_VIEW_MATERIALIZATION_ROOT,
	SQL_PLAN_COMPONENT_TRIGGER_SELECT_ROOT,
	SQL_PLAN_COMPONENT_TRIGGER_SELECT,
	SQL_PLAN_COMPONENT_ROLE_COUNT,
};

enum sql_plan_component_route {
	SQL_PLAN_COMPONENT_PENDING = 0,
	SQL_PLAN_COMPONENT_CURRENT_WHERE_C,
	SQL_PLAN_COMPONENT_NEW_PLANNER,
	SQL_PLAN_COMPONENT_FALLBACK,
	SQL_PLAN_COMPONENT_DIRECT_VALUES,
	SQL_PLAN_COMPONENT_DIRECT_OP_COUNT,
	SQL_PLAN_COMPONENT_COMPOUND_DISPATCH,
	SQL_PLAN_COMPONENT_MIXED,
	SQL_PLAN_COMPONENT_ROUTE_COUNT,
};

enum sql_plan_component_status {
	SQL_PLAN_COMPONENT_OK = 0,
	SQL_PLAN_COMPONENT_INVALID,
	SQL_PLAN_COMPONENT_DUPLICATE_ID,
	SQL_PLAN_COMPONENT_MISSING_PARENT,
	SQL_PLAN_COMPONENT_CONFLICT,
	SQL_PLAN_COMPONENT_OVERFLOW,
	SQL_PLAN_COMPONENT_INCOMPLETE,
};

struct sql_plan_component_record {
	uint32_t id;
	uint32_t parent_id;
	enum sql_plan_component_role role;
	enum sql_plan_component_route route;
	enum sql_plan_fallback_reason fallback_reason;
};

struct sql_plan_component_ledger {
	struct sql_plan_component_record records[SQL_PLAN_COMPONENT_MAX];
	size_t count;
	int overflowed;
};

struct sql_plan_component_summary {
	enum sql_plan_component_route route;
	enum sql_plan_fallback_reason fallback_reason;
	size_t component_count;
};

void
sql_plan_component_ledger_create(struct sql_plan_component_ledger *ledger);

enum sql_plan_component_status
sql_plan_component_add(struct sql_plan_component_ledger *ledger, uint32_t id,
		       uint32_t parent_id, enum sql_plan_component_role role);

enum sql_plan_component_status
sql_plan_component_set_route(struct sql_plan_component_ledger *ledger,
			     uint32_t id,
			     enum sql_plan_component_route route,
			     enum sql_plan_fallback_reason fallback_reason);

bool
sql_plan_component_route_is_pending(
	const struct sql_plan_component_ledger *ledger, uint32_t id);

enum sql_plan_component_status
sql_plan_component_finalize(const struct sql_plan_component_ledger *ledger,
			    struct sql_plan_component_summary *summary);

const char *
sql_plan_component_role_name(enum sql_plan_component_role role);

const char *
sql_plan_component_route_name(enum sql_plan_component_route route);

#endif /* TARANTOOL_SQL_PLAN_COMPONENT_H */
