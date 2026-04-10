#!/usr/bin/env tarantool
--[[
    Simple Dual Dispatcher Verification Test
    Phase 5.9 Verification - Basic SQL operations that work with both dispatchers
]]

box.cfg{
    memtx_memory = 268435456,
}
box.execute([[SET SESSION "sql_seq_scan" = true;]])

print("=== Dual Dispatcher Verification - SQL Test Suite ===")
print("Timestamp: " .. os.date("%Y-%m-%d %H:%M:%S"))
print("")

-- Test 1: Basic CREATE and INSERT
print("[Test 1] CREATE TABLE and INSERT")
local create_ok = false
local insert_ok = false

if pcall(function()
    box.execute[[DROP TABLE IF EXISTS test_basic]]
    box.execute[[CREATE TABLE test_basic (id INTEGER PRIMARY KEY, value INTEGER)]]
    create_ok = true
end) then
    print("  ✓ CREATE TABLE successful")
else
    print("  ✗ CREATE TABLE failed")
end

if pcall(function()
    for i = 1, 50 do
        box.execute("INSERT INTO test_basic VALUES (?, ?)", {i, i * 10})
    end
    insert_ok = true
end) then
    print("  ✓ INSERT successful (50 rows)")
else
    print("  ✗ INSERT failed")
end

-- Test 2: Basic SELECT
print("[Test 2] SELECT Operations")
local select_ok = false
local select_count = 0

if pcall(function()
    local result = box.execute("SELECT * FROM test_basic")
    select_count = #result.rows
    select_ok = true
end) then
    print("  ✓ SELECT successful (" .. select_count .. " rows)")
else
    print("  ✗ SELECT failed")
end

-- Test 3: SELECT with WHERE
print("[Test 3] SELECT with WHERE")
local where_ok = false
local where_count = 0

if pcall(function()
    local result = box.execute("SELECT * FROM test_basic WHERE value > 250")
    where_count = #result.rows
    where_ok = true
end) then
    print("  ✓ SELECT WHERE successful (" .. where_count .. " rows)")
else
    print("  ✗ SELECT WHERE failed")
end

-- Test 4: SELECT with COUNT
print("[Test 4] SELECT COUNT")
local count_ok = false
local total_count = 0

if pcall(function()
    local result = box.execute("SELECT COUNT(*) FROM test_basic")
    total_count = result.rows[1][1]
    count_ok = true
end) then
    print("  ✓ SELECT COUNT successful (count=" .. total_count .. ")")
else
    print("  ✗ SELECT COUNT failed")
end

-- Test 5: SELECT with arithmetic
print("[Test 5] SELECT with Arithmetic")
local arith_ok = false
local arith_count = 0

if pcall(function()
    local result = box.execute("SELECT id, value * 2 as doubled FROM test_basic LIMIT 5")
    arith_count = #result.rows
    arith_ok = true
end) then
    print("  ✓ SELECT arithmetic successful (" .. arith_count .. " rows with expressions)")
else
    print("  ✗ SELECT arithmetic failed")
end

-- Test 6: UPDATE
print("[Test 6] UPDATE Operations")
local update_ok = false

if pcall(function()
    box.execute("UPDATE test_basic SET value = value + 100 WHERE id <= 10")
    update_ok = true
end) then
    print("  ✓ UPDATE successful")
else
    print("  ✗ UPDATE failed")
end

-- Test 7: DELETE
print("[Test 7] DELETE Operations")
local delete_ok = false

if pcall(function()
    box.execute("DELETE FROM test_basic WHERE id > 40")
    delete_ok = true
end) then
    print("  ✓ DELETE successful")
else
    print("  ✗ DELETE failed")
end

-- Test 8: Verify data after DELETE
print("[Test 8] Verify Data After DELETE")
local verify_ok = false
local verify_count = 0

if pcall(function()
    local result = box.execute("SELECT COUNT(*) FROM test_basic")
    verify_count = result.rows[1][1]
    verify_ok = true
end) then
    print("  ✓ Verification successful (remaining rows: " .. verify_count .. ")")
else
    print("  ✗ Verification failed")
end

-- Summary
print("")
print("=== Test Summary ===")
local all_ok = create_ok and insert_ok and select_ok and where_ok and 
               count_ok and arith_ok and update_ok and delete_ok and verify_ok

if all_ok then
    print("✅ All tests PASSED")
    print("Results:")
    print("  - CREATE TABLE: OK")
    print("  - INSERT: OK (50 rows)")
    print("  - SELECT: OK (" .. select_count .. " rows)")
    print("  - SELECT WHERE: OK (" .. where_count .. " rows)")
    print("  - SELECT COUNT: OK (count=" .. total_count .. ")")
    print("  - SELECT arithmetic: OK (" .. arith_count .. " rows)")
    print("  - UPDATE: OK")
    print("  - DELETE: OK")
    print("  - Verification: OK (" .. verify_count .. " remaining rows)")
    os.exit(0)
else
    print("❌ Some tests FAILED")
    print("Results:")
    print("  - CREATE TABLE: " .. (create_ok and "OK" or "FAILED"))
    print("  - INSERT: " .. (insert_ok and "OK" or "FAILED"))
    print("  - SELECT: " .. (select_ok and "OK" or "FAILED"))
    print("  - SELECT WHERE: " .. (where_ok and "OK" or "FAILED"))
    print("  - SELECT COUNT: " .. (count_ok and "OK" or "FAILED"))
    print("  - SELECT arithmetic: " .. (arith_ok and "OK" or "FAILED"))
    print("  - UPDATE: " .. (update_ok and "OK" or "FAILED"))
    print("  - DELETE: " .. (delete_ok and "OK" or "FAILED"))
    print("  - Verification: " .. (verify_ok and "OK" or "FAILED"))
    os.exit(1)
end
