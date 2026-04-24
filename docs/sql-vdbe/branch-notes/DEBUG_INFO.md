# M5 Phase 2 — Debuggability and Profiling for CnP and MCJIT

## Current State (implemented, Apr 2026)

| Backend | Feature | Activation | What you get |
|---------|---------|------------|--------------|
| CnP | `.eh_frame` CFI | always-on | Stack unwinding through CnP frames; `gdb bt` shows CnP frames |
| CnP | GDB JIT registration | always-on | `gdb bt` shows `vdbe_cnp_stmt_<stmt_id>_ops_<nOp>` at frame `#0`; breakpoints and disassembly work by symbol or address |
| CnP | `/tmp/perf-PID.map` | `SQL_CNP_PERF_MAP=1` | Function-level perf symbolication |
| CnP | JITDUMP | `SQL_CNP_JITDUMP=1` | Per-opcode perf attribution after `perf inject --jit` |
| MCJIT | GDB registration listener | always-on | `gdb bt` shows MCJIT function names, breakpoints inside JIT |
| MCJIT | JITDUMP (PerfJITEventListener) | `SQL_JIT_PERF_MAP=1` | Function-level perf + DWARF via `perf inject --jit` |

Both always-on features (`eh_frame` for CnP, GDB listener for MCJIT) have zero
overhead when no debugger is attached: `__register_frame` is a pure registration
call, and `__jit_debug_register_code` is a no-op stub that GDB replaces with its
own handler only when attached.

---

## Tools

All tools live in `tools/jit_bench/`.  Run them from the **build directory**:

```
cd build-jit-relwithdebinfo
```

### GDB wrapper — `gdb_jit.sh`

```
bash /path/to/tools/jit_bench/gdb_jit.sh [OPTIONS] [-- LUA_SCRIPT [ARGS...]]

Options:
  -d, --dispatcher <name>  VDBE_DISPATCHER (generated|cnp|old)  default: cnp
  -j, --jit                Enable MCJIT (SQL_JIT_ENABLE=1)
  -b, --bench              Use the built-in benchmark workload
  -w, --workload <name>    BENCH_ONLY_WORKLOAD for --bench
  -C, --case <name>        BENCH_ONLY_CASE for --bench
  -n, --runs <count>       BENCH_RUNS for --bench
  -c, --cmd <gdb-cmd>      Extra GDB -ex command (repeatable)
  -B, --batch              Run GDB in batch mode (non-interactive)
  -h, --help               Show this help
```

**Interactive session** — break on first CnP compile, inspect Vdbe state:

```bash
cd build-jit-relwithdebinfo
bash tools/jit_bench/gdb_jit.sh -d cnp -- /tmp/my_workload.lua
# Inside GDB:
(gdb) sql-break-compile       # set silent compile-event breakpoints
(gdb) run
(gdb) cnp-info $rdi           # dump CnP state at first compile breakpoint
```

**Batch mode** — log every compile event silently and exit:

```bash
cd build-jit-relwithdebinfo
bash tools/jit_bench/gdb_jit.sh --bench -d cnp --batch -c "sql-break-compile"
```

**MCJIT interactive session:**

```bash
bash tools/jit_bench/gdb_jit.sh -d generated --jit -- /tmp/my_workload.lua
(gdb) sql-break-compile       # logs every vdbe_jit_compile call
(gdb) jit-info <vdbe_ptr>     # show MCJIT state for a Vdbe
```

### Self-stop wrapper — `gdb_jit_stop.sh`

Runs the reproducible self-stop demo (`sql_jit_stop_demo.lua`) and automates:

- stopping after compilation,
- resolving the active `Vdbe`,
- printing the generated symbol name,
- breaking on the generated code entry,
- printing `bt`,
- printing a short instruction window at the JIT entry.

```bash
bash /path/to/tools/jit_bench/gdb_jit_stop.sh [OPTIONS]

Options:
  -d, --dispatcher <name>  VDBE_DISPATCHER (generated|cnp|old)  default: cnp
  -j, --jit                Enable MCJIT (SQL_JIT_ENABLE=1)
  -s, --script <path>      Override the self-stop Lua script
  -c, --cmd <gdb-cmd>      Extra GDB -ex command (repeatable)
  -B, --batch              Run GDB in batch mode and exit
  -h, --help               Show this help
```

Examples:

```bash
# CnP: automated batch output
cd build-jit-relwithdebinfo
bash tools/jit_bench/gdb_jit_stop.sh -d cnp --batch

# MCJIT: automated batch output
cd build-jit-relwithdebinfo
bash tools/jit_bench/gdb_jit_stop.sh -d generated --jit --batch
```

**Stopped benchmark example: CnP**

```bash
cd build-jit-relwithdebinfo
bash tools/jit_bench/gdb_jit.sh --bench -d cnp \
    -w hot_expr -C prepared_execute -n 1 \
    -c "break vdbe_cnp_exec"

# Inside GDB after the breakpoint fires:
(gdb) cnp-info p
(gdb) bt
(gdb) frame 0
(gdb) cnp-disas p
(gdb) cnp-disas p 0
```

Expected debugger view:

```text
Breakpoint ... vdbe_cnp_exec (p=...)
(gdb) bt
#0  vdbe_cnp_exec(...)
#1  vdbe_exec_cnp_dispatcher(...)
#2  sqlVdbeExec(...)
...

(gdb) cnp-info p
  perf symbol:  vdbe_cnp_stmt_<stmt_id>_ops_<nOp>

(gdb) cnp-disas p
Disassembly for vdbe_cnp_stmt_<stmt_id>_ops_<nOp> [...]
```

**Stopped self-stop example: CnP**

Use the helper script that compiles the statement, raises `SIGSTOP`, and then
executes it again after you resume the inferior. This is the most reliable way
to stop with a compiled `cnp_code` pointer already available.

```bash
cd /tmp/jit-gdb-demo
env VDBE_DISPATCHER=cnp SQL_JIT_ENABLE=0 \
    gdb ./tarantool \
    --args /path/to/tarantool /path/to/tools/jit_bench/sql_jit_stop_demo.lua

# Inside GDB:
(gdb) source /path/to/tools/jit_bench/vdbe_jit.gdb
(gdb) run
# inferior stops in raise(SIGSTOP)
(gdb) sql-vdbes
(gdb) set $p = sql_get()->pVdbe
(gdb) cnp-info $p
(gdb) cnp-break $p
(gdb) signal 0
(gdb) bt
(gdb) cnp-find $pc
(gdb) cnp-disas $p
```

Expected debugger view:

```text
Vdbe 0x...  stmt_id=852fab6b  sql=SELECT 1 + 2 + 3 + 4 + 5;
  CnP : compiled=1 code=0x... size=707 name=vdbe_cnp_stmt_852fab6b_ops_13

Thread ... hit Breakpoint ..., 0x... in ?? ()
#0  0x... in vdbe_cnp_stmt_852fab6b_ops_13 ()
#1  vdbe_cnp_exec(...)
#2  sqlVdbeExec(...)
...

(gdb) cnp-find $pc
CnP addr 0x... belongs to Vdbe 0x...
  SQL:          SELECT 1 + 2 + 3 + 4 + 5;
  perf symbol:  vdbe_cnp_stmt_852fab6b_ops_13
```

**Stopped benchmark example: MCJIT**

```bash
cd build-jit-relwithdebinfo
bash tools/jit_bench/gdb_jit.sh --bench -d generated --jit \
    -w hot_expr -C prepared_execute -n 1 \
    -c "break sqlVdbeExec if p->jit_func != 0"

# Inside GDB after the breakpoint fires:
(gdb) jit-info p
(gdb) break *p->jit_func
(gdb) continue
(gdb) bt
(gdb) frame 0
(gdb) jit-disas p
```

Expected debugger view:

```text
(gdb) jit-info p
  symbol:       vdbe_jit_exec_<id> + <offset> in section .text of jit module

(gdb) bt
#0  vdbe_jit_exec_<id>(...)
#1  sqlVdbeExec(...)
#2  sql_step(...)
...

(gdb) jit-disas p
Disassembly for MCJIT function at 0x...
vdbe_jit_exec_<id> + <offset> in section .text of jit module
```

**Stopped self-stop example: MCJIT**

The same helper script works for MCJIT without the CnP warmup step because
MCJIT compiles at prepare time.

```bash
cd /tmp/jit-gdb-demo
env VDBE_DISPATCHER=generated SQL_JIT_ENABLE=1 \
    gdb -batch \
    -ex run \
    -ex "info functions vdbe_jit_exec_" \
    -ex "break vdbe_jit_exec_1" \
    -ex "signal 0" \
    -ex bt \
    -ex "disassemble vdbe_jit_exec_1" \
    --args /path/to/tarantool /path/to/tools/jit_bench/sql_jit_stop_demo.lua
```

Expected debugger view:

```text
All functions matching regular expression "vdbe_jit_exec_":
0x...  vdbe_jit_exec_1

Thread ... hit Breakpoint ..., 0x... in vdbe_jit_exec_1 ()
#0  0x... in vdbe_jit_exec_1 ()
#1  sqlVdbeExec(...) at vdbe.c:505
#2  sqlStep(...)
...
```

### GDB helper commands — `vdbe_jit.gdb`

Loaded automatically by `gdb_jit.sh`.  Can also be sourced manually:

```
(gdb) source /path/to/tools/jit_bench/vdbe_jit.gdb
```

| Command | Description |
|---------|-------------|
| `cnp-info <vdbe*>` | Dump CnP compilation state: SQL, op count, cnp_compiled, code ptr, size, `.eh_frame` ptr |
| `jit-info <vdbe*>` | Dump MCJIT compilation state: SQL, op count, jit_compiled, jit_func |
| `sql-vdbes` | List active VDBEs with SQL text plus CnP and MCJIT entry addresses |
| `cnp-find <addr>` | Resolve a CnP code address back to its active `Vdbe` and SQL |
| `jit-find <addr>` | Resolve a MCJIT function address back to its active `Vdbe` and SQL |
| `cnp-break <vdbe*>` | Set a breakpoint on `p->cnp_code` |
| `jit-break <vdbe*>` | Set a breakpoint on `p->jit_func` |
| `cnp-opcodes <vdbe*>` | Print the opcode table (PC, opcode, p1, p2, p3) |
| `cnp-disas <vdbe*> [pc]` | Disassemble the full CnP code buffer or one opcode stencil by PC |
| `jit-disas <vdbe*>` | Disassemble the MCJIT native function |
| `sql-break-compile` | Set silent logging breakpoints at `vdbe_cnp_compile` and `vdbe_jit_compile`; prints `[CnP]` / `[JIT]` lines with op count and SQL text |
| `sql-break-exec` | Set silent logging breakpoints at `vdbe_cnp_exec` and `sqlVdbeExec` with 6-frame backtrace |
| `sql-break-off` | Delete all breakpoints |

`cnp-info` prints the synthetic perf/JITDUMP/GDB symbol name
`vdbe_cnp_stmt_<stmt_id>_ops_<nOp>`. `jit-info` prints the actual MCJIT symbol
resolved by GDB, for example `vdbe_jit_exec_42`.

Navigation rules:

- SQL query -> JIT code:
  use `sql-vdbes`, then `cnp-info` / `jit-info`, then `cnp-break` /
  `jit-break`, then `cnp-disas` / `jit-disas`.
- JIT address -> SQL query:
  use `cnp-find <addr>` for CnP and `jit-find <addr>` for MCJIT.
- CnP reverse lookup works by matching the address against active
  `[cnp_code, cnp_code + cnp_size)` ranges.
- MCJIT reverse lookup works by matching the address against active `jit_func`
  entries.

**Note on parameter access in optimized builds:** The `commands` blocks in
`vdbe_jit.gdb` use `p->nOp` and `p->zSql` directly. In RelWithDebInfo builds,
the named parameter `p` is accessible via its DWARF `@entry` value even when
optimized out of registers. The `$rdi` register approach does NOT work reliably
with `tarantool-gdb.py` loaded (the FiberUnwinder modifies frame context).

**Note on GDB batch mode:** The `commands...end` blocks inside GDB `define`
bodies only work correctly when the `.gdb` file is sourced via `source` or `-x`,
not via inline `-ex` argument sequences. `gdb_jit.sh` uses `source` correctly.

**Note on when `vdbe_cnp_compile` fires:** CnP compilation happens at first
**execution** of a statement, not at prepare time.  `prepare_only` benchmark
phases will not trigger `sql-break-compile`.  Use `prepared_execute` or
`execute_only` workloads to see compile events.

### Perf wrapper — `perf_jit.sh`

```
bash /path/to/tools/jit_bench/perf_jit.sh [OPTIONS] [-- LUA_SCRIPT [ARGS...]]

Options:
  -d, --dispatcher <name>  VDBE_DISPATCHER (generated|cnp|old)  default: cnp
  -j, --jit                Enable MCJIT (SQL_JIT_ENABLE=1)
  -b, --bench              Use the built-in benchmark workload
  -w, --workload <name>    BENCH_ONLY_WORKLOAD for --bench
  -C, --case <name>        BENCH_ONLY_CASE for --bench
  -n, --runs <count>       BENCH_RUNS for --bench
  -e, --event <event>      perf event(s) (default: cycles)
  -g, --callgraph          Record call-graph with DWARF (slower)
  -r, --report-args <str>  Extra args passed to perf report
  --no-report              Skip perf report (just record + inject)
  -h, --help               Show this help
```

**CnP profiling with per-opcode attribution:**

```bash
cd build-jit-relwithdebinfo
bash tools/jit_bench/perf_jit.sh --bench -d cnp
# Runs: perf record -k mono → perf inject --jit → perf report
# CnP frames show as OP_Column, OP_Compare, etc.
```

**MCJIT profiling with call-graph:**

```bash
bash tools/jit_bench/perf_jit.sh --bench -d generated --jit -g
```

**Focused benchmark example for perf:**

```bash
cd build-jit-relwithdebinfo

# CnP: perf report shows vdbe_cnp_stmt_<stmt_id>_ops_<nOp>
bash tools/jit_bench/perf_jit.sh --bench -d cnp \
    -w hot_expr -C prepared_execute -n 1 \
    -r "--stdio --sort symbol,dso"

# MCJIT: perf report shows vdbe_jit_exec_<id>
bash tools/jit_bench/perf_jit.sh --bench -d generated --jit -g \
    -w hot_expr -C prepared_execute -n 1 \
    -r "--stdio --sort symbol,dso"
```

**Requirements:** `linux-tools` (perf), `kernel.perf_event_paranoid <= 2`.

```bash
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

---

## Implementation Details

### CnP `.eh_frame` (always-on, `vdbe_cnp.c`)

CnP stencils execute with no prologue: `CFA = RSP+8`, `RA = [RSP]`.  A single
static CIE (20 bytes) encodes this uniform convention.  Each compiled program
gets one FDE (24 bytes) with 8-byte absolute `pc_begin`/`pc_range` (no `zR`
augmentation, so libgcc uses native pointer width on x86-64).  A 4-byte zero
terminator follows the FDE — required by libgcc's `__register_frame`, which
takes a whole `.eh_frame` section, not a single FDE (macOS differs).

```
.eh_frame buffer layout (48 bytes total):
  [0..19]  CIE: len=16, id=0, ver=1, aug="", code_align=1, data_align=-8,
                RA=16, DW_CFA_def_cfa RSP+8, DW_CFA_offset r16 1
  [20..43] FDE: len=20, cie_ptr=24, pc_begin=<uint64>, pc_range=<uint64>
  [44..47] zero terminator
```

Registered via `__register_frame(p->cnp_ehframe)` at the end of
`vdbe_cnp_compile()`.  Freed and deregistered via `__deregister_frame()` in
`vdbe_cnp_release()`.  Guarded by `HAVE_REGISTER_FRAME` (CMake
`check_function_exists(__register_frame HAVE_REGISTER_FRAME)`).

### MCJIT GDB listener (always-on, `vdbe_jit_perf.cc`)

`llvm::JITEventListener::createGDBRegistrationListener()` registers each
compiled MCJIT function with GDB via `__jit_debug_register_code`.  When GDB is
not attached the function is a no-op stub; overhead is ~2 ns per compile.

### CnP JITDUMP (`SQL_CNP_JITDUMP=1`, `vdbe_cnp.c`)

Writes a mmap'd `/tmp/jit-PID.dump` in Linux perf JITDUMP format.  Per
compiled program: one `JIT_CODE_LOAD` record (address, size, symbol name) and
one `JIT_CODE_DEBUG_INFO` record (one entry per opcode: stencil address,
PC index as line number, opcode mnemonic as filename).  File grows via
`ftruncate` + `mremap` as needed.  `perf inject --jit` uses this to annotate
`perf report` with per-opcode attribution.

### MCJIT PerfJIT listener (`SQL_JIT_PERF_MAP=1`, `vdbe_jit_perf.cc`)

`llvm::JITEventListener::createPerfJITEventListener()` writes a JITDUMP file
with full DWARF info for MCJIT functions.  Used the same way as CnP JITDUMP.

---

## Struct Fields Added to `Vdbe` (`vdbeInt.h`)

```c
/** Heap-allocated .eh_frame buffer registered via __register_frame */
uint8_t *cnp_ehframe;
```

---

## Build Requirements

- `ENABLE_SQL_CNP=ON` in CMake (default when `ENABLE_SQL_JIT=ON`)
- `HAVE_REGISTER_FRAME` auto-detected via `check_function_exists`
- For MCJIT: `ENABLE_SQL_JIT=ON`, LLVM 11+ development packages
- Build from a build directory, not the source root:

```bash
cd build-jit-relwithdebinfo
make tarantool -j$(nproc)
```

---

## Quick-start Cheat Sheet

```bash
cd build-jit-relwithdebinfo

# Interactive GDB with CnP:
bash tools/jit_bench/gdb_jit.sh -d cnp -- /tmp/bench.lua
(gdb) sql-break-compile
(gdb) run
# ... breakpoint fires at first vdbe_cnp_compile call ...
(gdb) cnp-info $rdi

# Batch GDB — log all compile events:
bash tools/jit_bench/gdb_jit.sh --bench -d cnp --batch -c "sql-break-compile"

# Perf profiling — CnP with per-opcode attribution:
bash tools/jit_bench/perf_jit.sh --bench -d cnp

# Perf profiling — MCJIT with call-graph:
bash tools/jit_bench/perf_jit.sh --bench -d generated --jit -g

# Manual perf workflow:
SQL_CNP_PERF_MAP=1 SQL_CNP_JITDUMP=1 VDBE_DISPATCHER=cnp \
    perf record -k mono -e cycles -o perf.data -- ./src/tarantool bench.lua
perf inject --jit -i perf.data -o perf.jit.data
perf report -i perf.jit.data --stdio
```
