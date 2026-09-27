#ifndef TARANTOOL_SQL_SELECT_PREFLIGHT_H
#define TARANTOOL_SQL_SELECT_PREFLIGHT_H

struct Select;
struct SelectDest;

/* Reasons are stable within the preflight API, not externally serialized. */
enum sql_select_preflight_reject {
	SQL_SELECT_PREFLIGHT_OK,
	SQL_SELECT_PREFLIGHT_UNRESOLVED,
	SQL_SELECT_PREFLIGHT_DESTINATION,
	SQL_SELECT_PREFLIGHT_RELATION,
	SQL_SELECT_PREFLIGHT_SHAPE,
	SQL_SELECT_PREFLIGHT_PROJECTION,
	SQL_SELECT_PREFLIGHT_COLUMN_BINDING,
};

/* Pure eligibility check for the narrow SELECT c[, ...] FROM t scan.
 * It reads the resolved AST and destination only; it does not mutate either
 * object or allocate/emit VDBE state. */
enum sql_select_preflight_reject
sql_select_preflight_table_scan(const struct Select *select,
				const struct SelectDest *dest);

#endif /* TARANTOOL_SQL_SELECT_PREFLIGHT_H */
