#ifndef TARANTOOL_SQL_PLAN_DESCRIPTOR_H
#define TARANTOOL_SQL_PLAN_DESCRIPTOR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define SQL_PLAN_POINT_KEY_PART_MAX 255
#define SQL_PLAN_FILTER_MAX 8

/* Internal, immutable descriptor for the M3.1 single-relation contract. */
enum sql_plan_access_kind {
	SQL_PLAN_PK_POINT_LOOKUP,
	SQL_PLAN_INDEX_POINT_LOOKUP,
	SQL_PLAN_INDEX_RANGE_SCAN,
	SQL_PLAN_INDEX_FULL_SCAN,
	SQL_PLAN_TABLE_FULL_SCAN,
	SQL_PLAN_PK_PREFIX_SCAN,
	SQL_PLAN_INDEX_EQUALITY_SCAN,
	SQL_PLAN_INDEX_PREFIX_SCAN,
};

enum sql_plan_direction { SQL_PLAN_ASC, SQL_PLAN_DESC };
enum sql_plan_bound_side { SQL_PLAN_LOWER, SQL_PLAN_UPPER };
enum sql_plan_bound_op { SQL_PLAN_EQ, SQL_PLAN_GT, SQL_PLAN_GE,
			 SQL_PLAN_LT, SQL_PLAN_LE };
enum sql_plan_path_class { SQL_PLAN_CURRENT_WHERE_C, SQL_PLAN_NEW_PLANNER,
			   SQL_PLAN_FALLBACK };
/* Stable, append-only reason codes exposed by the planner producer. */
enum sql_plan_fallback_reason {
	SQL_PLAN_FALLBACK_NONE = 0,
	SQL_PLAN_FALLBACK_UNRESOLVED_INPUT = 1,
	SQL_PLAN_FALLBACK_UNSUPPORTED_RELATION_COUNT = 2,
	SQL_PLAN_FALLBACK_UNSUPPORTED_SUBQUERY = 3,
	SQL_PLAN_FALLBACK_UNSUPPORTED_AGGREGATE = 4,
	SQL_PLAN_FALLBACK_UNSUPPORTED_COMPOUND = 5,
	SQL_PLAN_FALLBACK_UNSUPPORTED_CTE = 6,
	SQL_PLAN_FALLBACK_UNSUPPORTED_DISTINCT = 7,
	SQL_PLAN_FALLBACK_INVALID_LOGICAL_PLAN = 8,
	SQL_PLAN_FALLBACK_NO_ACCESS_PATH = 9,
	SQL_PLAN_FALLBACK_INVALID_CANDIDATE = 10,
	SQL_PLAN_FALLBACK_UNSUPPORTED_NONDETERMINISTIC = 11,
	SQL_PLAN_FALLBACK_UNSUPPORTED_ACCESS_HINT = 12,
	SQL_PLAN_FALLBACK_UNSUPPORTED_FUNCTION = 13,
	SQL_PLAN_FALLBACK_UNSUPPORTED_COLLATION = 14,
	SQL_PLAN_FALLBACK_UNSUPPORTED_EXPRESSION = 15,
	SQL_PLAN_FALLBACK_UNSUPPORTED_FILTER = 16,
	SQL_PLAN_FALLBACK_UNSUPPORTED_DESTINATION = 17,
	/** Exclusive upper bound for valid reason-code values. */
	SQL_PLAN_FALLBACK_REASON_COUNT,
};
enum sql_plan_finalize_kind { SQL_PLAN_SORT, SQL_PLAN_LIMIT };

struct sql_plan_bound {
	enum sql_plan_bound_side side;
	enum sql_plan_bound_op op;
	uint32_t expr_ref;
};
struct sql_plan_order_term {
	uint32_t column;
	enum sql_plan_direction direction;
	int nulls_first;
};
struct sql_plan_point_key_part {
	int64_t integer_value;
	uint64_t unsigned_value;
	uint32_t column;
	bool is_unsigned;
};
struct sql_plan_access {
	enum sql_plan_access_kind kind;
	uint32_t index_id;
	/* Owned scalar key for the executable integer primary-key point path. */
	int64_t integer_point_key;
	bool has_integer_point_key;
	uint64_t unsigned_point_key;
	bool has_unsigned_point_key;
	/* One-based SQL bind ordinal for a scalar primary-key point lookup. */
	uint32_t point_key_variable;
	/* Multi-part integer/unsigned primary-key point lookup key. */
	const struct sql_plan_point_key_part *point_key_parts;
	size_t point_key_part_count;
	/* Equality key for an ordered leading-prefix scan on a composite PK. */
	const struct sql_plan_point_key_part *prefix_key_parts;
	size_t prefix_key_part_count;
	/* One-sided bound key/operator, or lower bound for a two-sided range. */
	int64_t integer_range_key;
	bool has_integer_range_key;
	uint64_t unsigned_range_key;
	bool has_unsigned_range_key;
	enum sql_plan_bound_op integer_range_op;
	/* Upper endpoint for two-sided primary-key ranges. */
	int64_t integer_range_end_key;
	uint64_t unsigned_range_end_key;
	bool has_integer_range_end_key;
	bool has_unsigned_range_end_key;
	enum sql_plan_bound_op integer_range_end_op;
	uint32_t range_key_column;
	const struct sql_plan_bound *bounds;
	size_t bound_count;
	enum sql_plan_direction direction;
	const uint32_t *projected_columns;
	size_t projected_column_count;
	const struct sql_plan_order_term *produced_order;
	size_t produced_order_count;
	double est_rows;
	double est_rows_confidence;
};
enum sql_plan_filter_op {
	SQL_PLAN_FILTER_EXPRESSION,
	SQL_PLAN_FILTER_IS_NULL,
	SQL_PLAN_FILTER_IS_NOT_NULL,
};
struct sql_plan_filter {
	uint32_t expr_ref;
	double selectivity;
	double confidence;
	uint32_t column;
	enum sql_plan_filter_op op;
};
struct sql_plan_finalize {
	enum sql_plan_finalize_kind kind;
	const struct sql_plan_order_term *keys;
	size_t key_count;
	uint64_t limit;
	uint64_t offset;
};
struct sql_plan_expression { uint32_t id; const char *canonical; };
struct sql_plan_descriptor_input {
	uint32_t descriptor_version;
	uint32_t planner_version;
	enum sql_plan_path_class path_class;
	uint32_t fallback_reason;
	uint32_t space_id;
	const char *space_name;
	struct sql_plan_access access;
	const struct sql_plan_filter *filters;
	size_t filter_count;
	const uint32_t *projection_columns;
	size_t projection_column_count;
	/* Optional parallel refs: 0 means direct column, otherwise expression id. */
	const uint32_t *projection_expr_refs;
	const struct sql_plan_finalize *finalize;
	size_t finalize_count;
	const struct sql_plan_expression *expressions;
	size_t expression_count;
	double cost_startup, cost_total, cost_rows, cost_row_width;
	double cost_confidence;
};
struct sql_plan_descriptor;

const char *sql_plan_fallback_reason_name(uint32_t reason);

/* Deep-copies input; NULL means malformed input or allocation failure. */
struct sql_plan_descriptor *
sql_plan_descriptor_new(const struct sql_plan_descriptor_input *input);
void sql_plan_descriptor_delete(struct sql_plan_descriptor *descriptor);
uint32_t sql_plan_descriptor_version(const struct sql_plan_descriptor *d);
const struct sql_plan_descriptor_input *
sql_plan_descriptor_get_input(const struct sql_plan_descriptor *d);
uint32_t sql_plan_descriptor_space_id(const struct sql_plan_descriptor *d);
const char *sql_plan_descriptor_space_name(const struct sql_plan_descriptor *d);
enum sql_plan_access_kind sql_plan_descriptor_access_kind(
	const struct sql_plan_descriptor *d);
size_t sql_plan_descriptor_filter_count(const struct sql_plan_descriptor *d);
size_t sql_plan_descriptor_expression_count(const struct sql_plan_descriptor *d);

#endif
