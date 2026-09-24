-- Execute the SQL-only subset of test-run's .test.sql format.
--
-- The test-run console consumes one non-comment input line at a time. This
-- adapter deliberately rejects language switches, other console directives,
-- and expected SQL errors: none can be inferred safely from a .result file.
-- Tests needing those features must use the normal runner or a richer adapter.
local M = {}

function M.run(path)
    local file, open_err = io.open(path, 'r')
    if not file then
        error(open_err, 0)
    end
    local statements = 0
    for line in file:lines() do
        local source_line = line:match('^%s*(.-)%s*$')
        if source_line ~= '' and not source_line:match('^%-%-') then
            if source_line:match('^\\') then
                file:close()
                error('unsupported SQL console directive in ' .. path .. ': ' ..
                      source_line, 0)
            end
            local result, err = box.execute(source_line)
            if result == nil then
                file:close()
                error('unexpected SQL error in ' .. path .. ': ' ..
                      tostring(err), 0)
            end
            statements = statements + 1
        end
    end
    file:close()
    if statements == 0 then
        error('SQL file has no statements: ' .. path, 0)
    end
    return statements
end

return M
