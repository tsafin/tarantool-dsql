# SQL Debug Facilities - VDBE Tracing and Listing

This document describes how to enable and use Tarantool's SQL debugging
facilities for bytecode inspection, execution tracing, and runtime opcode
profiling.

## Overview

The SQL debugging facilities are controlled by several bit flags in the `sql_flags` field:

| Flag | Value | Purpose |
|------|-------|---------|
| `SQL_VdbeTrace` | 0x00000001 | Trace VDBE execution (opcode-by-opcode) |
| `SQL_SqlTrace` | 0x00000200 | Print SQL statement as it executes |
| `SQL_VdbeListing` | 0x00000400 | Print complete VDBE program bytecode listing |

These flags control whether debug output is printed to stdout during SQL statement compilation and execution.

## How Flags Are Applied

The `sql_flags` field is managed in three layers:

1. **Session Level**: Stored in `struct session::sql_flags`
   - Set via `SET SESSION` SQL commands
   - Persists for all SQL statements in the session
   - Controls what flags are passed to statement compiler

2. **Parser Level**: Stored in `struct Parse::sql_flags`
   - Set during `sql_parser_create()` from session flags
   - Controls whether debug output is generated during parsing
   - Propagated to VDBE during compilation

3. **VDBE Level**: Stored in `struct Vdbe::sql_flags`
   - Set during VDBE creation from parser flags
   - Controls whether debug output is printed during execution
   - Checked at start of `sqlVdbeExec()` to print listings

## Method 1: Session Settings (Runtime)

### Via SQL SET SESSION

Set debugging flags for the current session using session settings:

```sql
-- Enable VDBE debug listing and tracing
SET SESSION 'sql_vdbe_debug' = true;

-- Enable just sequential scan debugging
SET SESSION 'sql_seq_scan' = true;

-- Enable parser debug output
SET SESSION 'sql_parser_debug' = true;

-- Enable SELECT statement debug output
SET SESSION 'sql_select_debug' = true;
```

### Mapping to Bit Flags

The session settings map to SQL flag combinations defined in `src/box/sql/build.c`:

```c
// sql_vdbe_debug maps to:
SQL_SqlTrace | SQL_VdbeListing | SQL_VdbeTrace

// sql_parser_debug maps to:
SQL_SqlTrace | PARSER_TRACE_FLAG

// sql_select_debug maps to:
SQL_SqlTrace | SQL_SelectTrace | SQL_WhereTrace
```

### How It Works

1. User executes: `SET SESSION 'sql_vdbe_debug' = true`
2. SQL compiler generates `OP_SetSession` opcode
3. During VDBE execution, `OP_SetSession` opcode handler:
   - Looks up session setting by name
   - Calls `sql_session_setting_set(sid, mp_value)`
   - Handler sets bits in `current_session()->sql_flags`
4. Next SQL statement compiled in same session:
   - Parser created with `sql_parser_create(&parser, current_session()->sql_flags)`
   - Flags propagated to VDBE
   - Debug output printed during execution

### Important: Session-First Requirement

**The SET SESSION statement must execute BEFORE subsequent statements to take effect.**

For testing, this means:

```lua
-- Correct: SET SESSION executes first
box.execute("SET SESSION 'sql_vdbe_debug' = true")
-- Subsequent statements now have debug output
box.execute("SELECT * FROM table")

-- Incorrect: Debug flags not yet set for CREATE TABLE
box.execute("CREATE TABLE t(id INTEGER PRIMARY KEY)")
box.execute("SET SESSION 'sql_vdbe_debug' = true")
```

## Method 2: Code-Level Debugging (Compile-Time)

### Via SQL_DEBUG Conditional Compilation

The `vdbe.c` file contains `#ifdef SQL_DEBUG` blocks that control debug output:

```c
#ifdef SQL_DEBUG
if (p->pc == 0 &&
    (p->sql_flags & (SQL_VdbeListing|SQL_VdbeEQP|SQL_VdbeTrace)) != 0) {
    int i;
    sqlVdbePrintSql(p);
    if ((p->sql_flags & SQL_VdbeListing) != 0) {
        printf("VDBE Program Listing:\n");
        for(i=0; i<p->nOp; i++) {
            sqlVdbePrintOp(stdout, i, &aOp[i]);
        }
    }
    // ... more debug output ...
}
#endif
```

**Note**: This code is **only compiled if SQL_DEBUG is defined at build time**.
The build system defines this via CMake for Debug builds.

## Opcode Profiling via `box.stat.sql()`

### Compile-time switch

Per-opcode profiling is controlled by `SQL_VDBE_OP_PROFILE`.

- **Debug builds**: enabled by default in `src/box/CMakeLists.txt`
- **Release builds**: disabled by default

The intent is to keep the per-op timing/counter overhead out of normal release
builds while making it available automatically in debug/JIT verification work.

### What it exposes

`box.stat.sql()` always returns the generic SQL counters. When
`SQL_VDBE_OP_PROFILE` is enabled it also returns:

```lua
{
  sql_opcode_profile_enabled = 1,
  interpreter_opcode_profile = {
    count = { Add = 1, SeekGE = 1, ResultRow = 1, ... },
    time_us = { Add = 0, SeekGE = 3, ResultRow = 0, ... },
  },
  jit_opcode_profile = {
    count = { Add = 4, Integer = 2, ... },
    time_us = { Add = 1, Integer = 0, ... },
  },
}
```

The values in `time_us` are accumulated microseconds measured with
`fiber_clock64()`.

`box.stat.sql()` also exposes the last **native compile failure** recorded by
each backend:

- `sql_cnp_last_compile_error`
- `sql_jit_last_compile_error`

These fields are always present, even when opcode profiling is disabled.
They are meant for the exact case where SQL execution stays correct by falling
back to the interpreter, but native compilation failed somewhere underneath.

### Native compile error fields

The two fields are **backend-specific** on purpose:

- a CnP fragment/stencil compile failure updates only
  `sql_cnp_last_compile_error`;
- an LLVM MCJIT compile failure updates only
  `sql_jit_last_compile_error`.

A successful compile of that backend clears its own field back to the empty
string. One backend does not overwrite the other backend's last error.

Typical healthy output looks like this:

```lua
{
  sql_cnp_last_compile_error = "",
  sql_jit_last_compile_error = "",
}
```

Typical failure output looks like this:

```lua
{
  sql_cnp_last_compile_error =
    "fragment compile: unresolved symbol 'mem_to_int_precise' for MustBeInt at pc 27",
  sql_jit_last_compile_error = "",
}
```

or:

```lua
{
  sql_cnp_last_compile_error = "",
  sql_jit_last_compile_error =
    "compile: LLVM module verification failed: PHI node entries do not match predecessors",
}
```

The exact message text is intentionally practical rather than stable API. Expect
it to contain the most useful local detail available at the failure site, such
as:

- the backend stage (`fragment compile`, `stencil compile`, `compile`);
- the missing symbol name;
- the opcode name and PC for CnP relocation failures;
- the allocation site or LLVM verifier text for MCJIT failures.

### Coverage

Interpreter profiling covers both execution engines:

1. the old inline dispatcher in `src/box/sql/vdbe.c`;
2. the generated loop dispatcher in `src/box/sql/vdbe_dispatch_wrapper.c`.

JIT profiling is emitted from LLVM-generated blocks in `src/box/sql/vdbe_jit.c`.

### Important interpretation note

The JIT opcode tables move only when native execution actually begins.

So this combination:

```lua
sql_jit_exec_count == 0
jit_opcode_profile.count == {}
```

does **not** mean the instrumentation is broken. It means the current runtime
guards kept the statement on the interpreter path, so only interpreter opcode
tables advanced.

### Typical workflow

```bash
cd build-jit-debug2
rm -f *.snap *.xlog
SQL_JIT_ENABLE=1 VDBE_DISPATCHER=generated ./src/tarantool /absolute/path/to/probe.lua
```

Example probe:

```lua
box.cfg{}
box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])
box.execute([[INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);]])
box.execute([[SELECT a + 1 FROM t WHERE id = 2;]])
print(require('yaml').encode(box.stat.sql()))
os.exit(0)
```

If the statement silently falls back to the interpreter, inspect the last-error
fields in the same dump before reaching for a debugger. For example, a CnP
fallback caused by a missing fragment export will usually leave a message like:

```lua
sql_cnp_last_compile_error =
  "fragment compile: unresolved symbol 'mem_to_int_precise' for MustBeInt at pc 27"
```

That is enough to tell you the problem is not SQL prepare and not runtime
execution, but native code generation for a specific opcode site.

## Debug Output Types

### VDBE Program Listing (SQL_VdbeListing)

**When Printed**: At start of `sqlVdbeExec()` if `p->pc == 0` and `SQL_VdbeListing` flag set

**Output Format**:
```
VDBE Program Listing:
  0 Init              1  13  0  0  -1  00
  1 String8           0  3  0  0  -1  00
  2 OpenSpace         1  3  0  0  -1  00
 ...
```

**What It Shows**:
- Complete bytecode program before execution
- All opcodes with their operands (P1, P2, P3, P4)
- Full view of what the compiler generated
- **Useful for**: Understanding what bytecode was generated for a SQL statement

### VDBE Execution Trace (SQL_VdbeTrace)

**When Printed**: During opcode execution in inline dispatcher

**Output Format**:
```
[opcode execution trace - printed during DISPATCH macro]
```

**What It Shows**:
- Step-by-step execution of each opcode
- Timing and state information
- Register values before/after opcode

**Useful for**: Debugging why opcodes execute in unexpected order

### SQL Statement Trace (SQL_SqlTrace)

**When Printed**: Before VDBE execution and at various compiler stages

**Output Format**:
```
[SQL statement being traced]
```

**Useful for**: Verifying which SQL statements are being compiled

## Dispatcher Architecture Impact

### Generated Dispatcher (VDBE_USE_GENERATED_DISPATCH=ON)

- Falls back to inline dispatcher for unhandled opcodes
- Debug output goes through inline dispatcher DISPATCH macro
- May not capture all output in hybrid mode

### Inline Dispatcher (VDBE_USE_GENERATED_DISPATCH=OFF)

- All opcodes go through same DISPATCH macro
- Consistent debug output capture
- Simpler tracing

### Recommendation for Debugging

**Use inline dispatcher for cleaner debug output**:

```bash
cmake -DVDBE_USE_GENERATED_DISPATCH=OFF ..
make -j tarantool
```

## Testing Strategy

### Complete Debug Test Flow

```lua
#!/usr/bin/env tarantool

box.cfg{}

-- Step 1: Enable debugging for subsequent statements
box.execute("SET SESSION 'sql_vdbe_debug' = true")

-- Step 2: Execute SQL - should show VDBE listing + trace
box.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT)")
box.execute("INSERT INTO t VALUES (1, 'test')")
box.execute("SELECT * FROM t")

os.exit(0)
```

### Expected Debug Output

When running with SQL_DEBUG enabled and sql_vdbe_debug=true:

1. **SET SESSION statement execution** (no listing, sets flags)
2. **CREATE TABLE**: VDBE listing printed to stdout
3. **INSERT**: VDBE listing printed to stdout
4. **SELECT**: VDBE listing printed to stdout

Each listing shows the complete bytecode program generated by the compiler.

## Checking if Flags Are Set

### At Session Level

Check current session flags via C code:

```c
struct session *session = current_session();
uint32_t flags = session->sql_flags;
bool vdbe_debug = (flags & (SQL_VdbeListing | SQL_VdbeTrace)) != 0;
```

### Verify Setting Applied

After `SET SESSION`, subsequent SQL statements inherit the flags through this chain:

1. `sql_parser_create(parser, current_session()->sql_flags)`
2. `v->sql_flags = pParse->sql_flags`
3. Checked in `sqlVdbeExec()` at line ~295

## Common Issues and Solutions

### Debug Output Not Appearing

**Symptom**: `SET SESSION 'sql_vdbe_debug' = true` executes but subsequent statements show no debug output

**Causes**:
1. **SQL_DEBUG not compiled in**: Rebuild with CMake debug mode
2. **Flags set after statements**: SET SESSION must execute FIRST
3. **Wrong dispatcher mode**: Generated dispatcher may suppress output
4. **Output redirected**: stdout might be captured elsewhere

**Solutions**:
- Verify SQL_DEBUG is defined: Check build flags
- Reorder statements: SET SESSION before queries
- Use inline dispatcher: `cmake -DVDBE_USE_GENERATED_DISPATCH=OFF`
- Check stdout capture: Verify output not redirected

### OP_SetSession Not Appearing in Bytecode

**Symptom**: `SET SESSION` statement doesn't generate `OP_SetSession` opcode

**Causes**:
1. SQL compiler optimization removed it
2. Setting not recognized by compiler

**Solution**:
- Use valid session setting names (from session_setting_strs array)
- Valid names: `sql_vdbe_debug`, `sql_seq_scan`, `sql_parser_debug`, `sql_select_debug`

## Implementation Details

### Key Files

- `src/box/sql/vdbe.c`: Main VDBE executor, contains SQL_DEBUG blocks
- `src/box/sql/build.c`: Session setting implementation, OP_SetSession handler
- `src/box/session_settings.c`: Session setting registration
- `src/box/session_settings.h`: Session setting definitions

### Relevant Structures

```c
// Session flags
struct session {
    uint32_t sql_flags;  // Bit flags for SQL debugging
};

// VDBE flags
struct Vdbe {
    uint32_t sql_flags;  // Copied from parser
};

// Parser flags
struct Parse {
    uint32_t sql_flags;  // Copied from session at parse time
};
```

### Flag Constants

Defined in `src/box/sql/sqlInt.h`:

```c
#define SQL_VdbeTrace      0x00000001  // VDBE execution trace
#define SQL_SqlTrace       0x00000200  // SQL execution trace
#define SQL_VdbeListing    0x00000400  // VDBE program listing
```

## Future Improvements

1. **Bytecode Caching**: Cache compiled bytecode to avoid recompilation
2. **Selective Tracing**: Trace only specific opcodes (currently all-or-nothing)
3. **Performance Profiling**: Integration with timing measurements
4. **Format Options**: JSON/structured output in addition to text
5. **Dispatcher-Agnostic Output**: Ensure consistent output regardless of dispatcher mode

## References

- VDBE Architecture: See VDBE_REFACTORING.md
- Opcode Reference: See opcodes.h for all opcode definitions
- Dispatcher Implementation: See vdbe_dispatch_wrapper.c and vdbe.c
