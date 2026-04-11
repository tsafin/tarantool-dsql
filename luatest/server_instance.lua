local compat = require('luatest._compat')

compat.use_vendored_path()

local chunk, err = loadfile(os.getenv('TEST_RUN_DIR') ..
    '/lib/luatest/luatest/server_instance.lua')
if chunk == nil then
    error(err)
end

return chunk()
