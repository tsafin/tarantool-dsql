local msgpack = require('msgpack')

local M = {}

local SNAPSHOT_FORMAT = 'tarantool.sql.planner.snapshot'
local SNAPSHOT_VERSION = 4
local INPUT_VERSION = 5
local SELECTOR_VERSION = 1

local function decode(value, what)
    if type(value) ~= 'string' then
        error(what .. ' must be a MessagePack string or varbinary', 3)
    end
    local ok, decoded = pcall(msgpack.decode, value)
    if not ok or type(decoded) ~= 'table' then
        error('invalid ' .. what .. ' MessagePack', 3)
    end
    return decoded
end

local function select_final_path(input)
    if input.version ~= INPUT_VERSION then
        error('unsupported SQL replay input version', 3)
    end
    if type(input.planner) ~= 'table' or
       input.planner.final_path_selector_version ~= SELECTOR_VERSION then
        error('unsupported SQL final-path selector version', 3)
    end
    local paths = input.final_path_candidates
    if type(paths) ~= 'table' or #paths == 0 then
        error('SQL replay input has no complete final-path candidates', 3)
    end

    local seen = {}
    local best = nil
    for _, path in ipairs(paths) do
        if type(path) ~= 'table' or type(path.plan_fingerprint) ~= 'string' or
           #path.plan_fingerprint ~= 16 or
           type(path.path_cost_logest) ~= 'number' or
           path.path_cost_logest ~= math.floor(path.path_cost_logest) or
           path.path_cost_logest < -32768 or path.path_cost_logest > 32767 then
            error('invalid SQL final-path candidate', 3)
        end
        if seen[path.plan_fingerprint] then
            error('duplicate SQL final-path fingerprint', 3)
        end
        seen[path.plan_fingerprint] = true
        if best == nil or path.path_cost_logest < best.path_cost_logest then
            best = path
        end
    end
    return best
end

-- Selection replay intentionally starts after enumeration, dominance, and
-- beam pruning. It consumes only the self-contained captured candidate list.
function M.replay_input(input_bytes)
    local input = decode(input_bytes, 'SQL replay input')
    local selected = select_final_path(input)
    return {
        fingerprint = selected.plan_fingerprint,
        path = selected,
        selector_version = SELECTOR_VERSION,
    }
end

function M.replay_snapshot(snapshot_bytes)
    local snapshot = decode(snapshot_bytes, 'SQL planner snapshot')
    if snapshot.format ~= SNAPSHOT_FORMAT or
       snapshot.version ~= SNAPSHOT_VERSION or snapshot.replayable ~= true or
       snapshot.replay_inputs == nil then
        error('SQL planner snapshot is not selection-replayable', 2)
    end
    local captured = snapshot.planner
    if type(captured) ~= 'table' or
       captured.final_path_status ~= 'complete' or
       type(captured.final_paths) ~= 'table' then
        error('SQL planner snapshot has no complete final-path capture', 2)
    end
    local input_bytes = tostring(snapshot.replay_inputs)
    local result = M.replay_input(input_bytes)
    local input = decode(input_bytes, 'SQL replay input')
    if #captured.final_paths ~= #input.final_path_candidates then
        error('SQL replay input does not match captured final-path count', 2)
    end
    for i, path in ipairs(captured.final_paths) do
        if path.fingerprint ~=
           input.final_path_candidates[i].plan_fingerprint then
            error('SQL replay input final-path order does not match snapshot', 2)
        end
    end
    result.captured_fingerprint =
        captured.selected_final_path_fingerprint
    result.matches_captured = result.fingerprint == result.captured_fingerprint
    return result
end

return M
