# TODO: VDBE Refactor — Current Status

This file tracks progress for the `src/box/sql/vdbe.c` refactor.

- [x] Analyze `vdbe.c`
  - Map responsibilities, data structures, helpers, and the execution loop. (DONE)
- [x] Define Module Boundaries
  - Proposed modules and file mapping; low-risk helper extraction decided. (DONE)
- [x] Design DSL for Op Spec
  - YAML DSL at `tools/vdbe_dsl/opcodes.yaml` and generator `tools/vdbe_codegen.py` (DONE)
- [x] Implement Code Generator
  - Generator implemented and executed locally; writes `vdbe_opcodes_generated.h` and `vdbe_dispatch_generated.c` (DONE)
- [x] Integrate Codegen in Build
  - CMake custom command/target added; generator runs during build (DONE)
- [x] Move generated outputs to build dir
  - Generator now emits into `${CMAKE_BINARY_DIR}/src/box/sql/generated` and CMake variables updated (DONE)
- [~] Split Handlers into Files
  - Arithmetic handlers: `src/box/sql/vdbe_ops_arith.c` (DONE - functional & integrated)
    - All 5 arithmetic ops extracted and integrated into main loop
    - OP_Add, OP_Subtract, OP_Multiply, OP_Divide, OP_Remainder
    - Clean implementations using mem_add(), mem_sub(), mem_mul(), mem_div(), mem_rem()
    - Full opcode documentation added (matching vdbe.c format)
  - Data/constant handlers: `src/box/sql/vdbe_ops_data.c` (DONE - functional & integrated)
    - All 11 data/constant ops with full opcode documentation
    - OP_Integer, OP_Bool, OP_Int64, OP_Real, OP_String, OP_Null, OP_Blob, OP_Variable, OP_Move, OP_Copy, OP_SCopy
  - Comparison handlers: `src/box/sql/vdbe_ops_compare.c` (DONE - functional & integrated)
    - Successfully extracted after adding `iCompare` to `struct Vdbe` (Option A from extraction plan)
    - Handlers return special values (0=continue, 1=jump, -1=error) for jump control
    - Main loop integration complete (✓ all 6 comparison ops now use extracted handlers)
    - Build verified: compiles cleanly with -Wall -Wextra -Werror
  - Control flow handlers: `src/box/sql/vdbe_ops_control.c` (DEFERRED)
    - File created with extraction plan documentation
    - Control flow ops (Goto, Jump, If/IfNot, Gosub/Return) remain in vdbe.c
    - Reason: PC manipulation complexity - better handled with dispatcher refactoring
    - See vdbe_ops_control.c for detailed extraction options (A/B/C)
  - Logical/bitwise handlers: `src/box/sql/vdbe_ops_logical.c` (DONE - functional & integrated)
    - All 6 logical/bitwise ops extracted and integrated
    - Boolean logic: OP_And, OP_Or, OP_Not (three-valued SQL logic)
    - Bitwise ops: OP_BitAnd, OP_BitOr, OP_BitNot
    - Clean implementations with proper NULL handling
  - Remaining extraction work will continue with dispatcher refactoring (IN-PROGRESS)
  - **Next extraction phases** (32 opcodes planned):
    - Phase 1: String operations - `vdbe_ops_string.c` (1 opcode: Concat)
    - Phase 2: Type conversions - `vdbe_ops_type.c` (3 opcodes: Cast, MakeRecord, ApplyType)
    - Phase 3: Aggregate functions - `vdbe_ops_aggregate.c` (2 opcodes: AggStep, AggFinal)
    - Phase 4a: Cursor data access - `vdbe_ops_cursor_data.c` (3 opcodes: Column, RowData, ResultRow)
    - Phase 4b: Cursor navigation - `vdbe_ops_cursor_nav.c` (6 opcodes: Next, Prev, Rewind, Last, etc.)
    - Phase 4c: Cursor seek - `vdbe_ops_cursor_seek.c` (4 opcodes: SeekGE, SeekGT, SeekLE, SeekLT)
    - Phase 4d: Index operations - `vdbe_ops_index.c` (8 opcodes: IdxInsert, IdxGE, Found, etc.)
    - Phase 4e: Data modification - `vdbe_ops_modify.c` (5 opcodes: Delete, Update, SInsert, etc.)
  - See `~/.claude/plans/handler-extraction-plan.md` for detailed breakdown
- [ ] Replace Switch with Dispatcher
  - Replace big `switch` in `vdbe.c` with generated dispatcher/jump-table (NOT STARTED)
- [ ] Add Tests & Fixtures
  - Unit tests for generator outputs and integration tests for the execution loop (NOT STARTED)
- [ ] Documentation & Handoff
  - Add docs describing the DSL, generator usage, and contributor instructions (NOT STARTED)

Notes and current decisions:

- The generator has been added and run locally; generated files were produced under the build directory.
- To avoid immediate macro/enum conflicts while iterating, the generated dispatch source was temporarily removed from `sql_sources`. The generator output remains in the build tree and can be re-enabled once the generated header and opcodes are reconciled with `sql/opcodes.h`.
- Full opcode documentation has been added to all extracted handler files (vdbe_ops_arith.c and vdbe_ops_data.c), making them self-documenting and matching the format in vdbe.c.
- vdbe_ops_compare.c and vdbe_ops_logical.c already had adequate/excellent documentation.

Next actions you can request:

- Reconcile the generator output (rename or guard generated macros to avoid conflicts) and re-enable the generated dispatch in the build.
- Add unit tests for extracted opcode handlers.
- Continue extracting more opcode groups (cursor operations, aggregate functions, etc.).

Progress recorded: generator, CMake wiring, build-dir generation, handler extraction with full documentation, and fixes to keep build green.
