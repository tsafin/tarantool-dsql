#include "sql_logical_plan.h"

#include <stdlib.h>

#include "sqlInt.h"
#include "box/space.h"

struct sql_logical_plan {
	struct sql_logical_node *nodes;
	size_t node_count;
	enum sql_logical_reject_reason reason;
};

static struct sql_logical_node *
append_node(struct sql_logical_plan *plan, enum sql_logical_op op,
	    struct sql_logical_node *input)
{
	if (plan->node_count == 5)
		return NULL;
	struct sql_logical_node *node = &plan->nodes[plan->node_count++];
	*node = (struct sql_logical_node){.op = op, .input = input};
	return node;
}

static struct sql_logical_plan *
reject(enum sql_logical_reject_reason why,
	 enum sql_logical_reject_reason *reason)
{
	if (reason != NULL)
		*reason = why;
	return NULL;
}

struct sql_logical_plan *
sql_logical_plan_from_select(const struct Select *select,
			     enum sql_logical_reject_reason *reason)
{
	if (reason != NULL)
		*reason = SQL_LOGICAL_REJECT_NONE;
	if (select == NULL || (select->selFlags & SF_Resolved) == 0)
		return reject(SQL_LOGICAL_REJECT_UNRESOLVED, reason);
	if (select->pPrior != NULL || (select->selFlags & SF_Compound) != 0)
		return reject(SQL_LOGICAL_REJECT_COMPOUND, reason);
	if (select->pWith != NULL)
		return reject(SQL_LOGICAL_REJECT_CTE, reason);
	if (select->pGroupBy != NULL || select->pHaving != NULL ||
	    (select->selFlags & (SF_Aggregate | SF_HasAgg)) != 0)
		return reject(SQL_LOGICAL_REJECT_AGGREGATE, reason);
	if ((select->selFlags & SF_Distinct) != 0)
		return reject(SQL_LOGICAL_REJECT_DISTINCT, reason);
	if (select->pSrc == NULL || select->pSrc->nSrc != 1)
		return reject(SQL_LOGICAL_REJECT_RELATION_COUNT, reason);
	const struct SrcList_item *src = &select->pSrc->a[0];
	if (src->space == NULL || src->pSelect != NULL || src->fg.isTabFunc)
		return reject(SQL_LOGICAL_REJECT_SUBQUERY, reason);
	/* INDEXED BY / NOT INDEXED are semantic access-path constraints. The
	 * prototype does not model them, so never let a future consumer silently
	 * choose a different access path. */
	if (src->fg.isIndexedBy || src->fg.notIndexed)
		return reject(SQL_LOGICAL_REJECT_ACCESS_HINT, reason);

	struct sql_logical_plan *plan = calloc(1, sizeof(*plan));
	if (plan == NULL)
		return NULL;
	plan->nodes = calloc(5, sizeof(*plan->nodes));
	if (plan->nodes == NULL) {
		free(plan);
		return NULL;
	}
	struct sql_logical_node *node = append_node(plan, SQL_LOGICAL_SCAN, NULL);
	if (node == NULL)
		goto allocation_error;
	node->space_id = src->space->def->id;
	node->space_name = src->space->def->name;
	if (select->pWhere != NULL) {
		node = append_node(plan, SQL_LOGICAL_FILTER, node);
		if (node == NULL)
			goto allocation_error;
		node->expr = select->pWhere;
	}
	node = append_node(plan, SQL_LOGICAL_PROJECT, node);
	if (node == NULL)
		goto allocation_error;
	node->expr_list = select->pEList;
	if (select->pOrderBy != NULL) {
		node = append_node(plan, SQL_LOGICAL_SORT, node);
		if (node == NULL)
			goto allocation_error;
		node->expr_list = select->pOrderBy;
	}
	if (select->pLimit != NULL || select->pOffset != NULL) {
		node = append_node(plan, SQL_LOGICAL_LIMIT, node);
		if (node == NULL)
			goto allocation_error;
		node->expr = select->pLimit;
		node->expr2 = select->pOffset;
	}
	return plan;

allocation_error:
	sql_logical_plan_delete(plan);
	return NULL;
}

void
sql_logical_plan_delete(struct sql_logical_plan *plan)
{
	if (plan == NULL)
		return;
	free(plan->nodes);
	free(plan);
}

const struct sql_logical_node *
sql_logical_plan_root(const struct sql_logical_plan *plan)
{
	return plan != NULL && plan->node_count != 0 ?
		&plan->nodes[plan->node_count - 1] : NULL;
}

enum sql_logical_reject_reason
sql_logical_plan_reject_reason(const struct sql_logical_plan *plan)
{
	return plan == NULL ? SQL_LOGICAL_REJECT_UNRESOLVED : plan->reason;
}
