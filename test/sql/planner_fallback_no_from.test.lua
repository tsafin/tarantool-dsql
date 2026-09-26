test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})

-- Constant SELECTs enter sqlWhereBegin() with zero source relations. They are
-- deliberately outside the new single-relation planner contract.
before = box.stat.sql()
summary, err = box.execute([[EXPLAIN (planner = 'summary') SELECT 1]])
assert(err == nil)
assert(summary.rows[1][1] == 'planner')
assert(summary.rows[1][2] == 'path_class')
assert(summary.rows[1][3] == 'fallback')
assert(summary.rows[2][2] == 'fallback_reason')
assert(summary.rows[2][3] == 'UNSUPPORTED_RELATION_COUNT')
after = box.stat.sql()
assert(after.sql_planner_fallback_total == before.sql_planner_fallback_total + 1)
reason_before = before.sql_planner_fallback_UNSUPPORTED_RELATION_COUNT_total
reason_after = after.sql_planner_fallback_UNSUPPORTED_RELATION_COUNT_total
assert(reason_after == reason_before + 1)
