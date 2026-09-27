test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})

box.execute([[CREATE TABLE planner_preflight_t (id INTEGER PRIMARY KEY, v INTEGER)]])
box.execute([[INSERT INTO planner_preflight_t VALUES (1, 10), (2, 20)]])

plain = box.execute([[SELECT v FROM planner_preflight_t]])
assert(plain.rows[1][1] == 10)
assert(plain.rows[2][1] == 20)
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT v FROM planner_preflight_t]])
assert(err == nil)
assert(summary.rows[1][3] == 'current_where_c')

filtered = box.execute([[SELECT v FROM planner_preflight_t WHERE id = 2]])
assert(#filtered.rows == 1 and filtered.rows[1][1] == 20)
computed = box.execute([[SELECT v + 1 FROM planner_preflight_t]])
assert(computed.rows[1][1] == 11 and computed.rows[2][1] == 21)

box.execute([[DROP TABLE planner_preflight_t]])
test_run = require('test_run').new()
