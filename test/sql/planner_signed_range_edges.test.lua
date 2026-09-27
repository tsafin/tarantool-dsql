test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})
box.execute([[SET SESSION "sql_seq_scan" = true]])

box.execute([[CREATE TABLE planner_signed_range_edges_t (id INTEGER PRIMARY KEY, v INTEGER)]])
box.execute([[INSERT INTO planner_signed_range_edges_t VALUES (-9223372036854775808, 10), (-1, 20), (0, 30), (1, 40), (9223372036854775807, 50)]])

function assert_no_fallback_delta(before, after, label) assert(after.sql_planner_fallback_total == before.sql_planner_fallback_total, label .. ': fallback total changed'); for key, value in pairs(before) do if key:match('^sql_planner_fallback_.*_total$') and key ~= 'sql_planner_fallback_total' then assert(after[key] == value, label .. ': ' .. key .. ' changed') end end end

function capture_signed_boundary(sql, route, label) before = box.stat.sql(); summary, err = box.execute([[EXPLAIN (planner = 'summary') ]] .. sql); assert(err == nil and summary.rows[1][3] == route, label .. ': unexpected route ' .. tostring(summary and summary.rows[1][3])); result, err = box.execute(sql); assert(err == nil, label .. ': execution failed: ' .. tostring(err)); after = box.stat.sql(); assert_no_fallback_delta(before, after, label); return result.rows end

box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
disabled_cross_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM planner_signed_range_edges_t WHERE id >= -1 AND id <= 1]])
assert(err == nil and disabled_cross_summary.rows[1][3] == 'current_where_c')
disabled_cross = box.execute([[SELECT id FROM planner_signed_range_edges_t WHERE id >= -1 AND id <= 1]]).rows

box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
enabled_cross_summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT id FROM planner_signed_range_edges_t WHERE id >= -1 AND id <= 1]])
assert(err == nil and enabled_cross_summary.rows[1][3] == 'new_planner')
enabled_cross = box.execute([[SELECT id FROM planner_signed_range_edges_t WHERE id >= -1 AND id <= 1]]).rows
assert(#enabled_cross == 3 and #disabled_cross == 3)
assert(enabled_cross[1][1] == -1 and enabled_cross[2][1] == 0 and enabled_cross[3][1] == 1)
assert(enabled_cross[1][1] == disabled_cross[1][1] and enabled_cross[2][1] == disabled_cross[2][1] and enabled_cross[3][1] == disabled_cross[3][1])
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
full_domain_sql = [[SELECT id FROM planner_signed_range_edges_t WHERE id >= -9223372036854775808 AND id <= 9223372036854775807]]
full_domain_disabled = capture_signed_boundary(full_domain_sql, 'current_where_c', 'off full domain')
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
full_domain_enabled = capture_signed_boundary(full_domain_sql, 'new_planner', 'on full domain')
assert(#full_domain_disabled == 5, 'legacy full signed domain row count: '..#full_domain_disabled)
assert(#full_domain_enabled == 5, 'new-planner full signed domain row count: '..#full_domain_enabled)
box.execute([[SET SESSION "sql_new_planner_single_table" = false]])
min_inclusive_sql = [[SELECT id FROM planner_signed_range_edges_t WHERE id <= -9223372036854775808]]
min_exclusive_sql = [[SELECT id FROM planner_signed_range_edges_t WHERE id < -9223372036854775808]]
max_inclusive_sql = [[SELECT id FROM planner_signed_range_edges_t WHERE id >= 9223372036854775807]]
max_exclusive_sql = [[SELECT id FROM planner_signed_range_edges_t WHERE id > 9223372036854775807]]
min_inclusive_disabled = capture_signed_boundary(min_inclusive_sql, 'current_where_c', 'off min inclusive')
min_exclusive_disabled = capture_signed_boundary(min_exclusive_sql, 'current_where_c', 'off min exclusive')
max_inclusive_disabled = capture_signed_boundary(max_inclusive_sql, 'current_where_c', 'off max inclusive')
max_exclusive_disabled = capture_signed_boundary(max_exclusive_sql, 'current_where_c', 'off max exclusive')
box.execute([[SET SESSION "sql_new_planner_single_table" = true]])
min_inclusive_enabled = capture_signed_boundary(min_inclusive_sql, 'new_planner', 'on min inclusive')
min_exclusive_enabled = capture_signed_boundary(min_exclusive_sql, 'new_planner', 'on min exclusive')
max_inclusive_enabled = capture_signed_boundary(max_inclusive_sql, 'new_planner', 'on max inclusive')
max_exclusive_enabled = capture_signed_boundary(max_exclusive_sql, 'new_planner', 'on max exclusive')
assert(#min_inclusive_disabled == 1 and min_inclusive_disabled[1][1] == -9223372036854775808)
assert(#min_inclusive_enabled == 1 and min_inclusive_enabled[1][1] == min_inclusive_disabled[1][1])
assert(#min_exclusive_disabled == 0 and #min_exclusive_enabled == 0)
assert(#max_inclusive_disabled == 1 and max_inclusive_disabled[1][1] == 9223372036854775807)
assert(#max_inclusive_enabled == 1 and max_inclusive_enabled[1][1] == max_inclusive_disabled[1][1])
assert(#max_exclusive_disabled == 0 and #max_exclusive_enabled == 0)

box.execute([[DROP TABLE planner_signed_range_edges_t]])
