#ifndef TARANTOOL_SQL_STATS_ANALYZE_H
#define TARANTOOL_SQL_STATS_ANALYZE_H

/* Execute the volatile SQL ANALYZE operation. NULL means the bare form. */
int
sql_stats_analyze_execute(const char *space_name);

#endif /* TARANTOOL_SQL_STATS_ANALYZE_H */
