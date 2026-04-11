local function vendored_root()
    local test_run_dir = os.getenv('TEST_RUN_DIR')
    if test_run_dir == nil then
        error('TEST_RUN_DIR is not set, unable to load vendored luatest')
    end
    return test_run_dir .. '/lib/luatest/luatest'
end

local function vendored_searcher(modname)
    if modname == 'luatest._compat' then
        return nil
    end
    if modname ~= 'luatest' and not modname:startswith('luatest.') then
        return nil
    end
    local root = vendored_root()
    local rel = modname == 'luatest' and 'init.lua' or
        modname:sub(#'luatest.' + 1):gsub('%.', '/') .. '.lua'
    local path = root .. '/' .. rel
    local chunk = loadfile(path)
    if chunk == nil then
        return nil
    end
    return chunk, path
end

local function use_vendored_path()
    local searchers = package.searchers or package.loaders
    for _, searcher in ipairs(searchers) do
        if searcher == vendored_searcher then
            return vendored_root()
        end
    end
    table.insert(searchers, 1, vendored_searcher)
    return vendored_root()
end

local function load_vendored(file_name)
    local root = use_vendored_path()
    local chunk, err = loadfile(root .. '/' .. file_name)
    if chunk == nil then
        error(err)
    end
    return chunk()
end

return {
    load_vendored = load_vendored,
    use_vendored_path = use_vendored_path,
}
