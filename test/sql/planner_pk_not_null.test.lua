test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})

box.execute([[CREATE TABLE pk_not_null_t (id INTEGER PRIMARY KEY, v INTEGER)]])
box.execute([[CREATE INDEX pk_not_null_v ON pk_not_null_t(v)]])
box.execute([[INSERT INTO pk_not_null_t VALUES (1, NULL), (2, 20), (3, 30)]])

box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
legacy_rows = box.execute([[SELECT id, v FROM pk_not_null_t WHERE id IS NOT NULL ORDER BY id DESC]]).rows
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id, v FROM pk_not_null_t WHERE id IS NOT NULL ORDER BY id DESC]])
assert(err == nil and summary.rows[1][3] == 'new_planner')
enabled_rows = box.execute([[SELECT id, v FROM pk_not_null_t WHERE id IS NOT NULL ORDER BY id DESC]]).rows
assert(#enabled_rows == #legacy_rows)
for i = 1, #legacy_rows do
    assert(enabled_rows[i][1] == legacy_rows[i][1] and enabled_rows[i][2] == legacy_rows[i][2])
end

-- A non-primary column has no non-null guarantee; it must remain legacy.
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM pk_not_null_t WHERE v IS NOT NULL]])
assert(err == nil and summary.rows[1][3] ~= 'new_planner')
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
