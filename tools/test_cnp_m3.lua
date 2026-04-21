box.cfg{log_level='warn'}
box.execute("SET SESSION \"sql_seq_scan\" = true")

local pass = 0
local fail = 0

local function check(label, sql, expected)
    local r, err = box.execute(sql)
    if err then
        print("FAIL ["..label.."]: error: "..tostring(err))
        fail = fail + 1
        return
    end
    local got = r and r.rows and r.rows[1] and r.rows[1][1]
    if got == expected then
        pass = pass + 1
    else
        print("FAIL ["..label.."]: expected="..tostring(expected).." got="..tostring(got))
        fail = fail + 1
    end
end

-- Basic expressions
check("select int", "SELECT 42", 42)
check("select arith", "SELECT 2+3*4", 14)
check("select string", "SELECT 'hello'", 'hello')
check("select null is null", "SELECT NULL IS NULL", true)
check("select 1<2", "SELECT 1<2", true)

-- DDL
local _, e1 = box.execute("CREATE TABLE m3t (id INT PRIMARY KEY, name TEXT, val DOUBLE)")
if not e1 then pass = pass + 1 else print("FAIL: CREATE TABLE:", e1) fail = fail + 1 end

-- DML
box.execute("INSERT INTO m3t VALUES (1, 'Alice', 1.5)")
box.execute("INSERT INTO m3t VALUES (2, 'Bob', 2.5)")
box.execute("INSERT INTO m3t VALUES (3, 'Charlie', 3.5)")
pass = pass + 1

-- SELECT with WHERE
check("where eq", "SELECT name FROM m3t WHERE id = 2", 'Bob')
check("where gt count", "SELECT COUNT(*) FROM m3t WHERE val > 2.0", 2)

-- Aggregate
check("count all", "SELECT COUNT(*) FROM m3t", 3)
check("min", "SELECT MIN(id) FROM m3t", 1)
check("max", "SELECT MAX(id) FROM m3t", 3)

-- UPDATE
box.execute("UPDATE m3t SET val = 10.0 WHERE id = 1")
check("after update", "SELECT val FROM m3t WHERE id = 1", 10.0)

-- DELETE
box.execute("DELETE FROM m3t WHERE id = 2")
check("after delete count", "SELECT COUNT(*) FROM m3t", 2)

-- ORDER BY
check("order asc", "SELECT id FROM m3t ORDER BY id ASC LIMIT 1", 1)
check("order desc", "SELECT id FROM m3t ORDER BY id DESC LIMIT 1", 3)

-- Conflict handling
local _, e2 = box.execute("INSERT INTO m3t VALUES (1, 'Dup', 0.0)")
if e2 then pass = pass + 1 else print("FAIL: duplicate insert should fail") fail = fail + 1 end

-- CREATE TABLE already exists should fail
local _, e3 = box.execute("CREATE TABLE m3t (id INT PRIMARY KEY)")
if e3 then pass = pass + 1 else print("FAIL: CREATE TABLE duplicate should fail") fail = fail + 1 end

-- DDL cleanup
local _, e4 = box.execute("DROP TABLE m3t")
if not e4 then pass = pass + 1 else print("FAIL: DROP TABLE:", e4) fail = fail + 1 end

local stats = box.stat.sql()
print(string.format("M3 gate: %d passed, %d failed | cnp_exec=%d",
    pass, fail, stats.sql_cnp_exec_count or 0))
os.exit(fail > 0 and 1 or 0)
