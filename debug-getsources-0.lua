local tarantool = (true)
local luaGetSources = tarantool and tarantool.debug and 
                tarantool.debug.getsources or
                function(filepath) return nil end

return luaGetSources
