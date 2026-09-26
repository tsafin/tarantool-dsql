test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})
box.execute([[SET SESSION "sql_seq_scan" = true;]])

-- Forbid multistatement queries.
box.execute('select 1;')
box.execute('select 1; select 2;')
box.execute('create table t1 (id INT primary key); select 100;')
box.space.t1 == nil
box.execute(';')
box.execute('')
box.execute('     ;')
box.execute('\n\n\n\t\t\t   ')

-- gh-3820: only table constraints can have a name.
--
box.execute('CREATE TABLE test (id INTEGER PRIMARY KEY, b INTEGER CONSTRAINT c1 NULL)')
box.execute('CREATE TABLE test (id INTEGER PRIMARY KEY, b INTEGER CONSTRAINT c1 DEFAULT 300)')
box.execute('CREATE TABLE test (id INTEGER PRIMARY KEY, b TEXT CONSTRAINT c1 COLLATE "binary")')

-- Make sure that type of literals in meta complies with its real
-- type. For instance, typeof(0.5) is number, not integer.
--
box.execute('SELECT 1;')
box.execute('SELECT 1.5;')
box.execute('SELECT 1.0;')
box.execute('SELECT 1.5e0;')
box.execute('SELECT 1e0;')
box.execute('SELECT \'abc\';')
box.execute('SELECT X\'4D6564766564\'')

--
-- gh-4139: assertion when reading a data-temporary space.
--
format = {{name = 'id', type = 'integer'}}
s = box.schema.space.create('s',{format=format, temporary=true})
i = s:create_index('i')
box.execute('select * from "s"')
s:drop()

-- Planner summaries are stable structured rows and retain SQL metadata.
--
_, err = box.execute([[CREATE TABLE summary_t (id INTEGER PRIMARY KEY)]])
planner_stats_before = box.stat.sql()
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM summary_t]])
assert(err == nil)
assert(#summary.metadata == 3)
assert(summary.metadata[1].name == 'section')
assert(summary.metadata[2].name == 'key')
assert(summary.metadata[3].name == 'value')
assert(#summary.rows == 2)
assert(summary.rows[1][1] == 'planner')
assert(summary.rows[1][2] == 'path_class')
assert(summary.rows[1][3] == 'current_where_c')
assert(summary.rows[2][1] == 'planner')
assert(summary.rows[2][2] == 'fallback_reason')
assert(summary.rows[2][3] == nil)
planner_stats_after = box.stat.sql()
assert(planner_stats_after.sql_planner_candidates_total > planner_stats_before.sql_planner_candidates_total)
assert(planner_stats_after.sql_planner_elapsed_us >= planner_stats_before.sql_planner_elapsed_us)
assert(planner_stats_after.sql_planner_fallback_total == planner_stats_before.sql_planner_fallback_total)
assert(planner_stats_after.sql_planner_fallback_UNSUPPORTED_RELATION_COUNT_total == planner_stats_before.sql_planner_fallback_UNSUPPORTED_RELATION_COUNT_total)
assert(planner_stats_after.sql_planner_fallback_UNSUPPORTED_AGGREGATE_total == planner_stats_before.sql_planner_fallback_UNSUPPORTED_AGGREGATE_total)
simple_count_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT count(*) FROM summary_t]])
assert(err == nil)
assert(simple_count_summary.rows[1][3] == nil)
assert(simple_count_summary.rows[2][3] == nil)
aggregate_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT a.id FROM summary_t AS a JOIN summary_t AS b ON a.id = b.id]])
assert(err == nil)
assert(aggregate_summary.rows[1][3] == 'fallback')
assert(aggregate_summary.rows[2][3] == 'UNSUPPORTED_RELATION_COUNT')
planner_stats_after_fallback = box.stat.sql()
assert(planner_stats_after_fallback.sql_planner_fallback_total > planner_stats_after.sql_planner_fallback_total)
assert(planner_stats_after_fallback.sql_planner_fallback_UNSUPPORTED_RELATION_COUNT_total == planner_stats_after.sql_planner_fallback_UNSUPPORTED_RELATION_COUNT_total + 1)
aggregate_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT count(*) FROM summary_t WHERE id > 0]])
assert(err == nil)
assert(aggregate_summary.rows[1][3] == 'fallback')
assert(aggregate_summary.rows[2][3] == 'UNSUPPORTED_AGGREGATE')
planner_stats_after_aggregate_fallback = box.stat.sql()
assert(planner_stats_after_aggregate_fallback.sql_planner_fallback_total > planner_stats_after_fallback.sql_planner_fallback_total)
assert(planner_stats_after_aggregate_fallback.sql_planner_fallback_UNSUPPORTED_AGGREGATE_total == planner_stats_after_fallback.sql_planner_fallback_UNSUPPORTED_AGGREGATE_total + 1)
planner_stats_before_subquery = box.stat.sql()
subquery_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM (SELECT id FROM summary_t LIMIT 1)]])
assert(err == nil)
assert(subquery_summary.rows[1][3] == 'fallback')
assert(subquery_summary.rows[2][3] == 'UNSUPPORTED_SUBQUERY')
planner_stats_after_subquery = box.stat.sql()
assert(planner_stats_after_subquery.sql_planner_fallback_UNSUPPORTED_SUBQUERY_total == planner_stats_before_subquery.sql_planner_fallback_UNSUPPORTED_SUBQUERY_total + 1)
planner_stats_before_expression_subqueries = box.stat.sql()
expression_subquery_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id, (SELECT max(id) FROM summary_t) FROM summary_t]])
assert(err == nil)
assert(expression_subquery_summary.rows[1][3] == 'fallback')
assert(expression_subquery_summary.rows[2][3] == 'UNSUPPORTED_SUBQUERY')
exists_subquery_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM summary_t WHERE EXISTS (SELECT 1 FROM summary_t AS nested WHERE nested.id = summary_t.id)]])
assert(err == nil)
assert(exists_subquery_summary.rows[1][3] == 'fallback')
assert(exists_subquery_summary.rows[2][3] == 'UNSUPPORTED_SUBQUERY')
in_subquery_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM summary_t WHERE id IN (SELECT id FROM summary_t AS nested)]])
assert(err == nil)
assert(in_subquery_summary.rows[1][3] == 'fallback')
assert(in_subquery_summary.rows[2][3] == 'UNSUPPORTED_SUBQUERY')
planner_stats_after_expression_subqueries = box.stat.sql()
assert(planner_stats_after_expression_subqueries.sql_planner_fallback_UNSUPPORTED_SUBQUERY_total == planner_stats_before_expression_subqueries.sql_planner_fallback_UNSUPPORTED_SUBQUERY_total + 3)
planner_stats_before_nondeterministic = box.stat.sql()
nondeterministic_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT random() FROM summary_t]])
assert(err == nil)
assert(nondeterministic_summary.rows[1][3] == 'fallback')
assert(nondeterministic_summary.rows[2][3] == 'UNSUPPORTED_NONDETERMINISTIC')
deterministic_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT abs(id) FROM summary_t]])
assert(err == nil)
assert(deterministic_summary.rows[1][3] == 'current_where_c')
assert(deterministic_summary.rows[2][3] == nil)
planner_stats_after_nondeterministic = box.stat.sql()
assert(planner_stats_after_nondeterministic.sql_planner_fallback_UNSUPPORTED_NONDETERMINISTIC_total == planner_stats_before_nondeterministic.sql_planner_fallback_UNSUPPORTED_NONDETERMINISTIC_total + 1)
box.schema.func.create('planner_nondeterministic_udf', {language = 'Lua', is_deterministic = false, body = 'function() return math.random() end', returns = 'double', param_list = {}, exports = {'SQL'}})
planner_stats_before_nondeterministic_udf = box.stat.sql()
nondeterministic_udf_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT planner_nondeterministic_udf() FROM summary_t]])
assert(err == nil)
assert(nondeterministic_udf_summary.rows[1][3] == 'fallback')
assert(nondeterministic_udf_summary.rows[2][3] == 'UNSUPPORTED_NONDETERMINISTIC')
planner_stats_after_nondeterministic_udf = box.stat.sql()
assert(planner_stats_after_nondeterministic_udf.sql_planner_fallback_UNSUPPORTED_NONDETERMINISTIC_total == planner_stats_before_nondeterministic_udf.sql_planner_fallback_UNSUPPORTED_NONDETERMINISTIC_total + 1)
box.schema.func.drop('planner_nondeterministic_udf')
planner_stats_before_distinct = box.stat.sql()
distinct_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT DISTINCT id % 2 FROM summary_t]])
assert(err == nil)
assert(distinct_summary.rows[1][3] == 'fallback')
assert(distinct_summary.rows[2][3] == 'UNSUPPORTED_DISTINCT')
planner_stats_after_distinct = box.stat.sql()
assert(planner_stats_after_distinct.sql_planner_fallback_UNSUPPORTED_DISTINCT_total == planner_stats_before_distinct.sql_planner_fallback_UNSUPPORTED_DISTINCT_total + 1)
planner_stats_before_compound = box.stat.sql()
compound_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM summary_t UNION SELECT id FROM summary_t]])
assert(err == nil)
assert(compound_summary.rows[1][3] == 'fallback')
assert(compound_summary.rows[2][3] == 'UNSUPPORTED_COMPOUND')
planner_stats_after_compound = box.stat.sql()
assert(planner_stats_after_compound.sql_planner_fallback_UNSUPPORTED_COMPOUND_total == planner_stats_before_compound.sql_planner_fallback_UNSUPPORTED_COMPOUND_total + 1)
planner_stats_before_cte = box.stat.sql()
cte_summary, err = box.execute([[EXPLAIN (planner = 'summary') WITH cte AS (SELECT id FROM summary_t) SELECT id FROM cte]])
assert(err == nil)
assert(cte_summary.rows[1][3] == 'fallback')
assert(cte_summary.rows[2][3] == 'UNSUPPORTED_CTE')
planner_stats_after_cte = box.stat.sql()
assert(planner_stats_after_cte.sql_planner_fallback_UNSUPPORTED_CTE_total == planner_stats_before_cte.sql_planner_fallback_UNSUPPORTED_CTE_total + 1)
_, err = box.execute([[DROP TABLE summary_t]])

_, err = box.execute([[CREATE TABLE planner_snapshot_t (id INTEGER PRIMARY KEY)]])
assert(err == nil)
snapshot, err = box.execute([[EXPLAIN (planner = 'snapshot') SELECT a.id FROM planner_snapshot_t AS a JOIN planner_snapshot_t AS b ON a.id = b.id]])
assert(err == nil)
assert(#snapshot.metadata == 1)
assert(snapshot.metadata[1].name == 'snapshot')
assert(snapshot.metadata[1].type == 'varbinary')
assert(#snapshot.rows == 1)
snapshot_object = require('msgpack').decode(tostring(snapshot.rows[1][1]))
assert(snapshot_object.format == 'tarantool.sql.planner.snapshot')
assert(snapshot_object.version == 2)
assert(snapshot_object.path_class == 'fallback')
assert(snapshot_object.fallback_reason == 'UNSUPPORTED_RELATION_COUNT')
assert(snapshot_object.replayable == false)
assert(snapshot_object.replay_inputs == nil)
assert(snapshot_object.planner.fallback_count == 1)
assert(snapshot_object.planner.candidate_count >= 0)
assert(snapshot_object.planner.elapsed_us >= 0)
assert(snapshot_object.planner.generated >= 0)
assert(snapshot_object.planner.dominated >= 0)
assert(snapshot_object.planner.truncated >= 0)
assert(snapshot_object.planner.retained >= 0)
simple_snapshot, err = box.execute([[EXPLAIN (planner = 'snapshot') SELECT id FROM planner_snapshot_t WHERE id = 1]])
assert(err == nil)
simple_snapshot_object = require('msgpack').decode(tostring(simple_snapshot.rows[1][1]))
assert(simple_snapshot_object.path_class == 'current_where_c')
assert(simple_snapshot_object.replayable == false)
assert(simple_snapshot_object.replay_inputs == nil)
assert(simple_snapshot_object.planner.candidate_count > 0)
assert(simple_snapshot_object.planner.fallback_count == 0)
assert(simple_snapshot_object.planner.generated > 0)
assert(simple_snapshot_object.planner.retained > 0)
_, err = box.execute([[DROP TABLE planner_snapshot_t]])
assert(err == nil)

--
-- gh-4267: Full power of vdbe_field_ref
-- Tarantool's SQL internally stores data offset for all acceded
-- fields. It also keeps a bitmask of size 64 with all initialized
-- slots in actual state to find the nearest left field really
-- fast and parse tuple from that position. For fieldno >= 64
-- bitmask is not applicable, so it scans data offsets area in
-- a cycle.
--
-- The test below covers a case when this optimisation doesn't
-- work and the second lookup require parsing tuple from
-- beginning.
---
format = {}
t = {}
for i = 1, 70 do                                                \
        format[i] = {name = 'field'..i, type = 'unsigned'}      \
        t[i] = i                                                \
end
s = box.schema.create_space('test', {format = format})
pk = s:create_index('pk', {parts = {70}})
s:insert(t)
box.execute('SELECT field70, field64 FROM test')

-- In the case below described optimization works fine.
pk:alter({parts = {66}})
box.execute('SELECT field66, field68, field70 FROM test')
box.space.test:drop()

-- gh-4933: Make sure that autoindex optimization is used.
box.execute('CREATE TABLE t1(i INT PRIMARY KEY, a INT);')
box.execute('CREATE TABLE t2(i INT PRIMARY KEY, b INT);')
for i = 1, 10240 do\
	box.execute('INSERT INTO t1 VALUES ($1, $1);', {i})\
	box.execute('INSERT INTO t2 VALUES ($1, $1);', {i})\
end
box.execute('EXPLAIN QUERY PLAN SELECT a, b FROM t1, t2 WHERE a = b;')

-- gh-5592: Make sure that diag is not changed with the correct query.
box.execute('SELECT a;')
diag = box.error.last()
box.execute('SELECT * FROM (VALUES(true));')
diag == box.error.last()

-- exclude_null + SQL correctness
box.execute([[CREATE TABLE j (s1 INT PRIMARY KEY, s2 STRING, s3 VARBINARY)]])
s = box.space.j
i = box.space.j:create_index('I3', {parts = {2, 'string', exclude_null = true}})
box.execute([[INSERT INTO j VALUES (1,NULL,NULL), (2,'',X'00');]])

box.execute([[SELECT * FROM j;]])
box.execute([[SELECT * FROM j INDEXED BY I3;]])

box.execute([[SELECT COUNT(*) FROM j GROUP BY s2;]])
box.execute([[SELECT COUNT(*) FROM j INDEXED BY I3;]])

box.execute([[UPDATE j INDEXED BY I3 SET s2 = NULL;]])
box.execute([[INSERT INTO j VALUES (3, 'a', X'33');]])

box.execute([[SELECT * FROM j;]])
box.execute([[SELECT * FROM j INDEXED BY I3;]])

box.execute([[UPDATE j INDEXED BY I3 SET s3 = NULL;]])
s:select{}

s:drop()
