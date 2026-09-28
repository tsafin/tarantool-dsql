test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})
box.execute([[SET SESSION "sql_seq_scan" = true]])

box.execute([[CREATE TABLE pk_not_null_t (id INTEGER PRIMARY KEY, v INTEGER)]])
box.execute([[CREATE INDEX pk_not_null_v ON pk_not_null_t(v)]])
box.execute([[INSERT INTO pk_not_null_t VALUES (1, NULL), (2, 20), (3, 30)]])

box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
legacy_result, legacy_error = box.execute([[SELECT id, v FROM pk_not_null_t WHERE id IS NOT NULL ORDER BY id DESC]])
assert(legacy_error == nil, tostring(legacy_error))
legacy_rows = legacy_result.rows
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id, v FROM pk_not_null_t WHERE id IS NOT NULL ORDER BY id DESC]])
assert(err == nil and summary.rows[1][3] == 'new_planner')
enabled_rows = box.execute([[SELECT id, v FROM pk_not_null_t WHERE id IS NOT NULL ORDER BY id DESC]]).rows
assert(#enabled_rows == #legacy_rows)
for i = 1, #legacy_rows do assert(enabled_rows[i][1] == legacy_rows[i][1] and enabled_rows[i][2] == legacy_rows[i][2]) end

-- Every part of a composite primary key is also guaranteed non-null.
box.execute([[CREATE TABLE pk_not_null_composite_t (a INTEGER, b INTEGER, v INTEGER, PRIMARY KEY(a, b))]])
box.execute([[INSERT INTO pk_not_null_composite_t VALUES (2, 20, NULL), (1, 10, 10), (3, 30, 30)]])
function sorted_key_rows(rows) table.sort(rows, function(x, y) if x[1] ~= y[1] then return x[1] < y[1] end return x[2] < y[2] end); return rows end
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
composite_disabled = box.execute([[SELECT a, b FROM pk_not_null_composite_t WHERE b IS NOT NULL]]).rows
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT a, b FROM pk_not_null_composite_t WHERE b IS NOT NULL]])
assert(err == nil and summary.rows[1][3] == 'new_planner')
composite_enabled = box.execute([[SELECT a, b FROM pk_not_null_composite_t WHERE b IS NOT NULL]]).rows
assert(#composite_enabled == 3)
assert(#composite_disabled == 3)
composite_enabled_sorted = sorted_key_rows(composite_enabled)
composite_disabled_sorted = sorted_key_rows(composite_disabled)
for i = 1, 3 do assert(composite_enabled_sorted[i][1] == composite_disabled_sorted[i][1] and composite_enabled_sorted[i][2] == composite_disabled_sorted[i][2]) end
composite_disabled_null = box.execute([[SELECT a FROM pk_not_null_composite_t WHERE b IS NULL]]).rows
assert(#composite_disabled_null == 0)
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT a FROM pk_not_null_composite_t WHERE b IS NULL]])
assert(err == nil and summary.rows[1][3] == 'new_planner')
composite_null_rows = box.execute([[SELECT a FROM pk_not_null_composite_t WHERE b IS NULL]]).rows
assert(#composite_null_rows == 0)
box.execute([[DROP TABLE pk_not_null_composite_t]])

-- A primary-key field cannot be NULL, so IS NULL is exactly empty.
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
legacy_null_result, legacy_null_error = box.execute([[SELECT id, v FROM pk_not_null_t WHERE id IS NULL]])
assert(legacy_null_error == nil and #legacy_null_result.rows == 0)
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id, v FROM pk_not_null_t WHERE id IS NULL]])
assert(err == nil and summary.rows[1][3] == 'new_planner')
enabled_null_rows = box.execute([[SELECT id, v FROM pk_not_null_t WHERE id IS NULL]]).rows
assert(#enabled_null_rows == 0)

-- A non-primary column may be NULL; preserve the unsupported-filter fallback.
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM pk_not_null_t WHERE v IS NULL]])
assert(err == nil and summary.rows[1][3] == 'fallback')
assert(summary.rows[2][3] == 'UNSUPPORTED_FILTER')
enabled_non_primary_null_rows = box.execute([[SELECT id FROM pk_not_null_t WHERE v IS NULL]]).rows
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
disabled_non_primary_null_rows = box.execute([[SELECT id FROM pk_not_null_t WHERE v IS NULL]]).rows
assert(#enabled_non_primary_null_rows == 1 and #disabled_non_primary_null_rows == 1)
assert(enabled_non_primary_null_rows[1][1] == 1 and disabled_non_primary_null_rows[1][1] == 1)

-- A non-primary column has no non-null guarantee; it must remain legacy.
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM pk_not_null_t WHERE v IS NOT NULL]])
assert(err == nil and summary.rows[1][3] == 'fallback')
assert(summary.rows[2][3] == 'UNSUPPORTED_FILTER')
enabled_non_primary_rows = box.execute([[SELECT id FROM pk_not_null_t WHERE v IS NOT NULL]]).rows
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
disabled_non_primary_rows = box.execute([[SELECT id FROM pk_not_null_t WHERE v IS NOT NULL]]).rows
assert(#enabled_non_primary_rows == #disabled_non_primary_rows)
assert(enabled_non_primary_rows[1][1] == 2 and enabled_non_primary_rows[2][1] == 3)
assert(disabled_non_primary_rows[1][1] == enabled_non_primary_rows[1][1] and disabled_non_primary_rows[2][1] == enabled_non_primary_rows[2][1])
