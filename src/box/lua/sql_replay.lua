local msgpack = require('msgpack')

local M = {}

local SNAPSHOT_FORMAT = 'tarantool.sql.planner.snapshot'
local SNAPSHOT_VERSION = 4
local INPUT_VERSION = 5
local SELECTOR_VERSION = 1

local function is_logest(value)
    return type(value) == 'number' and value == math.floor(value) and
        value >= -32768 and value <= 32767
end

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
           not is_logest(path.path_cost_logest) or
           not is_logest(path.unsorted_cost_logest) or
           not is_logest(path.output_rows_logest) or
           type(path.is_ordered) ~= 'number' or
           path.is_ordered ~= math.floor(path.is_ordered) or
           path.is_ordered < 0 or type(path.reverse_mask) ~= 'number' or
           path.reverse_mask ~= math.floor(path.reverse_mask) or
           path.reverse_mask < 0 then
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
    local selected_is_captured = false
    for _, path in ipairs(captured.final_paths) do
        if path.fingerprint == captured.selected_final_path_fingerprint then
            selected_is_captured = true
            break
        end
    end
    if not selected_is_captured then
        error('captured selected fingerprint is absent from final paths', 2)
    end
    result.captured_fingerprint =
        captured.selected_final_path_fingerprint
    result.matches_captured = result.fingerprint == result.captured_fingerprint
    return result
end

return M
