# Claude Notes

## Git Operations

### Commit Messages
- Write practical, factual commit messages describing what was actually done
- No emoji symbols in commit messages
- No "Co-Authored-By:" / "Co-authored-by:" footer, including Copilot trailers
- Avoid exaggerations and advertising language
- Focus on technical implementation details and actual changes
- Example: "sql: fix OP_NoConflict register initialization" vs "sql: fix OP_NoConflict bug ✅"

### File Staging
- **NEVER use `git add -A` or `git add .`** - Always specify explicit file names/paths
- Always use `git add <filename>` or `git add <path/to/file>` for clarity and safety
- This prevents accidentally staging unwanted files (IDE settings, build artifacts, etc.)

## Build System

- **CRITICAL**: `make box` only builds the static libbox.a library, NOT the tarantool executable
- **Always use `make tarantool`** when you need to build/rebuild the executable after code changes
- This is essential after modifying src/box/sql/* files or any source that affects the executable
- `make box` is useful only for checking if the library compiles, but won't update the binary

## Running Tarantool Executable

- **CRITICAL**: Always run `src/tarantool` from within the build directory
- **Always clean *.snap and *.xlog files before running**: `rm -f *.snap *.xlog` to avoid state from previous runs
  - *.snap files contain database snapshots
  - *.xlog files contain transaction logs (write-ahead log)
- The tarantool executable expects to run from the build root, not the source root

## Lua Test Scripts (Tarantool)

- Always run `box.cfg{}` before any `box.execute` operations.
- Always call `os.exit(rc)` at the end of the script to exit the event loop.

## SQL Test Running

- Use `python3 test/test-run.py --builddir /absolute/path/to/build --suite sql` for the `test/sql/` suite.
- Use `python3 test/test-run.py --builddir /absolute/path/to/build --suite sql-tap` for the `test/sql-tap/` suite.
- Use `python3 test/test-run.py --builddir /absolute/path/to/build --suite sql-luatest` for the `test/sql-luatest/` suite.
- For focused debugging, change to test directory and use in a form: `--builddir ../relative-build-dir <relative-test-path>` (for example `sql-tap/seot1.test.lua`).
- Always pass `--builddir` explicitly when checking JIT or alternate builds, otherwise `test-run.py` may pick the wrong executable.
- SQL TAP harness enables `sql_seq_scan`; keep it enabled for SQL debugging unless a test explicitly checks the opposite behavior.

## Standalone Lua Debug Scripts

- For standalone repro/debug scripts, run tarantool from the build directory:
  `cd <builddir> && rm -f *.snap *.xlog && ./src/tarantool /absolute/path/to/script.lua`
- For direct `test/sql-tap/*.test.lua` runs outside `test-run.py`, extend `LUA_PATH` so harness helpers resolve:
  `cd test/sql-tap && LUA_PATH='./?.lua;./lua/?.lua;;' <builddir>/src/tarantool <test>.test.lua`
- If the TAP file also needs tokenizer/helpers copied by the harness, prefer `test-run.py`; direct execution is best for quick focused debugging after reproducing the environment it expects.
- When debugging the generated dispatcher or JIT, add the runtime selector explicitly, for example:
  `cd <builddir> && rm -f *.snap *.xlog && VDBE_DISPATCHER=generated SQL_JIT_ENABLE=1 ./src/tarantool /absolute/path/to/script.lua`
- Use standalone scripts for fast repros outside the test harness; use `test-run.py` when you need suite setup, result checking, or memtx/vinyl coverage.

## SQL Runtime Statistics

- `box.stat.sql()` exposes persistent SQL execution counters from `src/box/sql.c:sql_debug_info()`.
- Generic counters now include:
  - `sql_interpreter_step_count`
  - `sql_jit_step_count`
  - `sql_jit_compile_count`
  - `sql_jit_compile_success_count`
  - `sql_jit_exec_count`
  - `sql_jit_full_run_count`
  - `sql_jit_fallback_count`
  - `sql_jit_resume_skip_count`
  - `sql_jit_guard_skip_count`
- Per-opcode profiling is controlled by `SQL_VDBE_OP_PROFILE`.
  - Debug builds enable it by default in `src/box/CMakeLists.txt`.
  - Release builds leave it off unless you define `SQL_VDBE_OP_PROFILE=1` explicitly.
- When enabled, `box.stat.sql()` also contains nested maps:
  - `interpreter_opcode_profile.count`
  - `interpreter_opcode_profile.time_us`
  - `jit_opcode_profile.count`
  - `jit_opcode_profile.time_us`
- Per-opcode timing is accumulated in microseconds via `fiber_clock64()`.
- Interpreter per-opcode profiling covers both:
  - the old inline dispatcher in `vdbe.c`;
  - the generated dispatcher loop in `vdbe_dispatch_wrapper.c`.
- JIT per-opcode stats move only when native execution actually starts, so if
  `sql_jit_exec_count == 0`, expect the JIT opcode maps to stay empty too.

## Debugging Assertions

When encountering an assertion failure:
1. **First attempt**: Quick code examination in the immediate area where assertion fires
2. **If that doesn't find the root cause**: Use debugger to get full stack trace
   - Run with `gdb -batch -ex run -ex "bt full" --args ./src/tarantool <script>`
   - Full backtrace often reveals the real problem is several levels up the call stack
   - Don't assume the assertion location is the root cause - it's usually a symptom
   - Example: assertion in OP_NoConflict handler may actually be caused by uninitialized registers in OP_MakeRecord from bad bytecode generation

## VS Code Terminal Notes

- VS Code sets `GIT_PAGER=cat` in its integrated terminal, overriding `core.pager`
- Environment variables take precedence over all git config settings for pager
- To restore paging in VS Code terminal: `unset GIT_PAGER` (or add to ~/.bashrc)

## VDBE Work: Two Independent Tracks

### Track 1: Generated Threaded Interpreter Dispatcher (DSL-based)

This track is about generating the opcode dispatch loop from a YAML DSL
(`tools/vdbe_dsl/opcodes.yaml`) using `tools/vdbe_codegen.py`. The generator
produces `src/box/sql/generated/vdbe_dispatch_generated.c` and
`src/box/sql/generated/vdbe_opcodes_generated.h`.

**This is NOT the JIT.** It produces a conventional threaded/switch interpreter.

#### DSL Build Target
- Use `make vdbe_codegen` (NOT `python3 tools/vdbe_codegen.py ...` directly)
- CMake target defined in `src/box/CMakeLists.txt` with proper DEPENDS on:
  - `tools/vdbe_dsl/opcodes.yaml`
  - `tools/vdbe_codegen.py`
  - `src/box/sql/opcodes.h` (for opcode ID sync)
- Touching opcodes.yaml triggers automatic regeneration on next build

#### Handler Types in DSL
- `external`: call external handler function (returns rc)
- `external_inline`: call `vdbe_op_<name>_inline(p, pOp, aMem)` — rc<0 = error, rc=1 = jump P2, rc=0 = continue
- `inline`: embed dispatcher-state-touching code directly (uses `aOp`, `pc` etc.)
- `fallthrough`: shared fallthrough case, no break
- `control_flow`: deferred to future phase

#### Current Status (Feb 19, 2026)
- **142/142 opcodes** covered in the generated dispatcher (100%)
- **70/70 tests pass** (`test_phase58.lua` in build root)
- Generated dispatcher is built and validated but **NOT yet the default execution path**
  - `VDBE_USE_GENERATED_DISPATCH` in `vdbe_dispatch.h` is still commented out
  - Old inline dispatcher in `vdbe.c` remains primary
  - Runtime switching via `VDBE_DISPATCHER` env var: `old|generated|parallel|auto`
- opcodes.yaml.bak was accidentally committed in fc4ed97ef6, removed in f0c808daac

#### Remaining Work (Interpreter Track)
1. **Activate generated dispatcher as default** — flip `VDBE_USE_GENERATED_DISPATCH` on
2. **Clean up `inline` stubs** — 9 opcodes still have partial `inline_code` blobs:
   OP_OffsetLimit, OP_SetSession, OP_ShowCreateTable, OP_SorterSort, OP_Program,
   OP_Compare, OP_Permutation, OP_TTransaction, OP_IteratorOpen, OP_SorterOpen etc.
   Extract to proper `_inline` C handlers, switch to `external_inline`
3. **Remove old dispatcher from vdbe.c** after default switch is stable
4. **Benchmark** generated vs old dispatcher on TPC-H queries

---

### Track 2: LLVM JIT (vdbe_jit.c)

Compiles VDBE programs to native code at prepare time using LLVM OrcJIT.
Completely separate from the interpreter dispatcher above.

#### CMake Build System

- Added `ENABLE_SQL_JIT` option in main CMakeLists.txt (default: OFF)
- When enabled, requires LLVM 11+ (12+ recommended) with OrcJIT components
- Bitcode files (.bc) generated for handler modules: vdbe_ops_arith.c, vdbe_ops_compare.c, vdbe_ops_logical.c, vdbe_ops_data.c, vdbe_ops_cursor_data.c, vdbe_ops_index.c, vdbe_ops_string.c, vdbe_ops_type.c
- Install location: `${CMAKE_INSTALL_DATAROOTDIR}/tarantool/sql_handlers/*.bc`
- Build requires: LLVM development packages (llvm-11-dev or higher on Debian/Ubuntu)
- Fixed compiler compatibility: gnu-alignof-expression warning now only for Clang

#### Implementation Status

**Step 1 - COMPLETED**: Capture handler IR at build time
- [x] CMake option ENABLE_SQL_JIT added
- [x] LLVM detection with version check (minimum 11)
- [x] Bitcode generation commands for 8 handler files
- [x] Installation rules for .bc files

**Step 2 - COMPLETED**: Add JIT compiler infrastructure
- [x] Created src/box/sql/vdbe_jit.c with LLVM C API wrappers
- [x] Created src/box/sql/vdbe_jit.h with public API declarations
- [x] Added JIT fields to struct Vdbe: jit_func, jit_module, jit_compiled (int types for C compatibility)
- [x] Implemented stub functions: vdbe_jit_init(), vdbe_jit_compile(), vdbe_jit_cleanup(), vdbe_jit_shutdown(), vdbe_jit_is_enabled()
- [x] Fixed bool type issues (use int for C compatibility, Bool for vdbeInt.h)
- [x] Successfully built with ENABLE_SQL_JIT=ON using LLVM 11

**Step 3 - COMPLETED**: Generate JIT function by linking handler IR
- [x] Phase 1-5: Basic infra, opcode classification, bitcode linking, pointer computation, LLVM optimization passes
- [x] Fixed CMake: moved LLVM setup before add_subdirectory(src) so LLVM_LIBS is available at link time

**Step 4 - COMPLETED**: Integrate JIT into VDBE execution loop
- [x] Call vdbe_jit_compile() in sqlVdbeMakeReady() at PREPARE time (failure non-fatal)
- [x] Invoke JIT function in sqlVdbeExec() before dispatcher; returns PC of unsupported opcode
- [x] Clean up JIT resources in sqlVdbeClearObject()
- [x] Mark OP_ResultRow as UNSUPPORTED (requires special SQL_ROW return handling)

#### JIT Dispatcher Coverage (Feb 19, 2026)
- **141/142 opcodes** handled by JIT — OP_Program is JIT_MODE_UNSUPPORTED
- OP_Program falls back to generated dispatcher (which handles it fully, 142/142)
- JIT cannot inline OP_Program: JIT pre-bakes aOp/aMem pointer offsets at compile
  time; OP_Program swaps both out at runtime for VdbeFrame sub-program execution
- Phase 5.8 (commit f8cd200b34): Added final 17 opcodes including coroutines and DDL
- Phase 5.9 (Feb 18, 2026): **45/45 tests pass**

#### JIT Remaining Work
1. **Benchmark** — real performance comparison requires Release build (`-O2`/`-O3`);
   current build is Debug (`-O0`), meaningless for JIT vs interpreter comparison
2. **OP_Program JIT support** — requires dynamic aOp/aMem tracking instead of
   pre-baked offsets; significant vdbe_jit.c architectural change

#### Key Behavioral Notes
- CHECK/FK constraints defined in DDL but not enforced at SQL layer in this build
- ANALYZE, CREATE/DROP SEQUENCE return `nil` result from `box.execute` (not an error)
- `./src/tarantool - << EOF` stdin/heredoc mode unreliable — always use file-based `.lua` scripts
- See `DISPATCHER_STATUS.md` for full opcode-by-opcode tracking
