-- Minimal SQL statement classifier for deciding whether planner SELECT
-- metadata is applicable. This is intentionally not a SQL parser: for WITH
-- statements it looks only at unquoted top-level verbs, since every CTE body
-- is parenthesized.
local M = {}

local function top_level_words(sql)
	local words = {}
	local i = 1
	local depth = 0
	local size = #sql
	while i <= size do
		local char = sql:sub(i, i)
		local next_char = sql:sub(i + 1, i + 1)
		if char:match("%s") then
			i = i + 1
		elseif char == '-' and next_char == '-' then
			local newline = sql:find('\n', i + 2, true)
			i = newline ~= nil and newline + 1 or size + 1
		elseif char == '/' and next_char == '*' then
			local close = sql:find('*/', i + 2, true)
			i = close ~= nil and close + 2 or size + 1
		elseif char == "'" or char == '"' or char == '`' or char == '[' then
			local close_char = char == '[' and ']' or char
			i = i + 1
			while i <= size do
				if sql:sub(i, i) == close_char then
					if close_char ~= ']' and sql:sub(i + 1, i + 1) == close_char then
						i = i + 2
					else
						i = i + 1
						break
					end
				else
					i = i + 1
				end
			end
		elseif char == '(' then
			depth = depth + 1
			i = i + 1
		elseif char == ')' then
			depth = math.max(depth - 1, 0)
			i = i + 1
		elseif char:match('[%a_]') then
			local start = i
			i = i + 1
			while i <= size and sql:sub(i, i):match('[%w_$]') do
				i = i + 1
			end
			if depth == 0 then
				words[#words + 1] = sql:sub(start, i - 1):upper()
			end
		else
			i = i + 1
		end
	end
	return words
end

function M.has_select_plan(sql)
	if type(sql) ~= 'string' then
		return false
	end
	local words = top_level_words(sql)
	local first = words[1]
	if first == 'SELECT' or first == 'VALUES' then
		return true
	end
	if first ~= 'WITH' then
		return false
	end
	for i = 2, #words do
		local word = words[i]
		if word == 'SELECT' then
			return true
		end
		if word == 'INSERT' or word == 'UPDATE' or word == 'DELETE' then
			return false
		end
	end
	return false
end

return M
