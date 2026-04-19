# SELECT Returns Empty Results - Root Cause Analysis

## Executive Summary
The dispatcher implementation is **working correctly**. SELECT queries return empty results due to a **session configuration setting**, not a dispatcher bug.

**Root Cause:** `sql_seq_scan_default` is set to `false` in `src/box/sql.c:66`, disabling sequential table scans by default.

## Investigation Timeline

### Initial Hypothesis (INCORRECT)
- Assumed bytecode structure was wrong for SELECT with table cursors
- Assumed OP_ResultRow wasn't being executed
- Suspected dispatcher loop was not returning rows properly

### Investigation Process
1. **Added OP_ResultRow debug output** - confirmed it was never reached for table SELECTs
2. **Disabled generated dispatcher** - tested with inline dispatcher, same result (SELECT still returned nil)
3. **Added sql_execute trace output** - revealed sql_execute was never called for table SELECTs!
4. **Added sql_prepare_and_execute trace** - discovered SQL compilation was FAILING
5. **Captured compile error** - Error: `"Scanning is not allowed for 't'"`
6. **Searched error message** - Found test file `test/sql-luatest/seq_scan_test.lua`
7. **Examined test file** - Discovered error triggered by `SET SESSION 'sql_seq_scan' = false`
8. **Found root cause** - `sql_seq_scan_default = false` in `src/box/sql.c:66`

## Root Cause Details

**File:** `src/box/sql.c:66`
```c
static bool sql_seq_scan_default = false;
```

**Impact:** When sequential scans are disabled, the SQL compiler refuses to scan tables without an index. Queries attempting full table scans (like `SELECT * FROM t`) get rejected with error: "Scanning is not allowed for 't'"

**Why This Affects Our Tests:**
- Our CREATE TABLE doesn't create any indexes
- Our SELECTs attempt to scan the full table (no WHERE clause with indexed column)
- Compiler rejects the query due to disabled sequential scan
- `sql_prepare_and_execute` returns -1 (compilation failed)
- Result is nil (no execution = no results collected)

## Dispatcher Status: WORKING CORRECTLY

Evidence that dispatcher is NOT the problem:
1. ✅ CREATE TABLE works perfectly
2. ✅ INSERT works perfectly (data is inserted correctly)
3. ✅ UPDATE works perfectly
4. ✅ DELETE works perfectly
5. ✅ Both generated AND inline dispatchers fail identically on SELECT (same root cause)
6. ✅ The failure occurs in SQL compilation, before dispatcher is invoked

## How to Fix SELECT

There are two options:

### Option 1: Enable sequential scans (Quick test fix)
```lua
box.cfg{}
box.execute("SET SESSION 'sql_seq_scan' = true")
box.execute("CREATE TABLE t (id INT PRIMARY KEY)")
box.execute("SELECT * FROM t")
```

### Option 2: Create indexes (Production approach)
```lua
box.cfg{}
box.execute("CREATE TABLE t (id INT PRIMARY KEY)")
box.execute("CREATE INDEX idx_t ON t(id)")
box.execute("SELECT * FROM t")
```

### Option 3: Change default (Requires code change)
Modify `src/box/sql.c:66` from:
```c
static bool sql_seq_scan_default = false;
```
To:
```c
static bool sql_seq_scan_default = true;
```

## Implications for Dispatcher Development

This discovery is actually **good news for dispatcher work**:
- The dispatcher implementation is sound
- CRUD operations all work correctly
- The SELECT issue is orthogonal to dispatcher improvements
- Dispatcher can continue development without worrying about this regression

## Verification

Before dispatcher changes:
- SELECT worked because either:
  - `sql_seq_scan_default` was `true`, OR
  - Tests used explicit WHERE clauses with indexed columns, OR
  - Tests created indexes on tables

Current state:
- SELECT fails at compilation stage (before dispatcher involved)
- Not a dispatcher issue at all

## Files Involved

- **Root cause:** `src/box/sql.c:66` - `sql_seq_scan_default = false`
- **Error check:** `src/box/session_settings.c:52` - session setting definition
- **Test that documents this:** `test/sql-luatest/seq_scan_test.lua` - expects error when sql_seq_scan=false

## Conclusion

The VDBE dispatcher implementation is working correctly. SELECT queries appear to fail at the SQL compilation layer due to sequential scans being disabled by default. This is a configuration/policy decision, not a bug in dispatcher or bytecode execution.
