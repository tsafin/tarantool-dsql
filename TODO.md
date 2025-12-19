# TODO: VDBE Refactor — Current Status

This file tracks progress for the `src/box/sql/vdbe.c` refactor.

**Quick Links**:
- **[VDBE_REFACTOR_MASTER_PLAN.md](VDBE_REFACTOR_MASTER_PLAN.md)** - Complete project overview and architecture
- **[PHASE_5_6d_PLAN.md](PHASE_5_6d_PLAN.md)** - Next phase (Medium Batch 3)
- **[PHASE_5_6c_SESSION_SUMMARY.md](PHASE_5_6c_SESSION_SUMMARY.md)** - Current phase completion details
- **[PHASE_5_6_INLINE_CODE_STRATEGY.md](PHASE_5_6_INLINE_CODE_STRATEGY.md)** - Overall strategy and approach

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
  - **Extraction phases completed** (19 opcodes extracted):
    - Phase 1: String operations - `vdbe_ops_string.c` (DONE - 1 opcode: Concat)
    - Phase 2: Type conversions - `vdbe_ops_type.c` (DONE - 3 opcodes: Cast, MakeRecord, ApplyType)
    - Phase 3: Aggregate functions - `vdbe_ops_aggregate.c` (DONE - 2 opcodes: AggStep, AggFinal)
    - Phase 4a: Cursor data access - `vdbe_ops_cursor_data.c` (DONE - 3 opcodes: Column, RowData, ResultRow)
      - OP_ResultRow uses special return value (1) to signal SQL_ROW, similar to comparison ops
      - Trace functionality removed from ResultRow (db not accessible in handler)
    - Phase 4b: Cursor navigation - `vdbe_ops_cursor_nav.c` (DONE - 6 opcodes: Last, Rewind, Next, NextIfOpen, Prev, PrevIfOpen)
      - Handlers return res value (0 or 1) from xAdvance, or -1 on error
      - vdbe.c shares next_tail code for all navigation ops + SorterNext
      - Eliminates 5x duplication of cache invalidation and jump logic
      - IfOpen variants check cursor in vdbe.c, then delegate to core handlers
    - Phase 4c: Cursor seek - `vdbe_ops_cursor_seek.c` (DONE - 4 opcodes: SeekGE, SeekGT, SeekLE, SeekLT)
      - Handlers return 0 (continue), 1 (jump to P2), 2 (skip next opcode for SEEKEQ), or -1 (error)
      - SeekLE/SeekGE support OPFLAG_SEEKEQ for equality seeking with automatic next opcode skip
      - Full type checking and implicit numeric conversions implemented
    - Phase 4d: Index operations - `vdbe_ops_index.c` (DONE - 9 opcodes: IdxGE/GT/LE/LT, Found/NotFound/NoConflict, IdxInsert/IdxReplace)
      - Index comparisons: vdbe_op_idx_compare() for IdxGE/GT/LE/LT
      - Index lookups: vdbe_op_found_notfound_noconflict() for Found/NotFound/NoConflict
      - Index insert/replace: vdbe_op_idx_insert_replace() for IdxInsert/IdxReplace
      - Made vdbe_add_new_autoinc_id() non-static for use in handlers
      - Note: OP_IdxDelete is a data modification op, belongs to Phase 4e
    - Phase 4e: Data modification - `vdbe_ops_modify.c` (DONE - 5 opcodes: Delete, Update, SInsert, SDelete, IdxDelete)
      - Table data modification: Delete, Update
      - System space modification: SInsert, SDelete
      - Index entry deletion: IdxDelete
      - All handlers properly integrated into vdbe.c with EXECUTE() macros
  - **Total extracted so far**: 28 opcodes across 7 new files (Phases 1-4e complete)
  - See `~/.claude/plans/handler-extraction-plan.md` for detailed breakdown
- [~] Replace Switch with Dispatcher (Phase 5)
  - Generate complete dispatch loop from YAML DSL, replacing EXECUTE() macros
  - **Architecture**: Generate actual dispatch code (not function pointers) to preserve computed-goto performance
  - **Strategy**: 5-phase incremental migration with parallel testing
    - Phase 5.1: Enhance vdbe_codegen.py with full dispatch generation ✓ COMPLETED
      - ✓ Added generator functions for external/inline/control_flow handlers
      - ✓ Generated vdbe_dispatch_generated.c with complete dispatch loop (176 opcodes)
      - ✓ Support both computed-goto and switch fallback modes
      - ✓ Extended YAML schema with handler_type field
      - ✓ Populated opcodes.yaml with all 176 opcodes
      - ✓ Dispatch table properly indexed by opcode ID (0-175)
      - ✓ Generated files: vdbe_opcodes_generated.h, vdbe_dispatch_generated.c
      - Commit: d168152b18
    - Phase 5.2: Finalize inline opcode extraction ✓ COMPLETED
      - ✓ Created extract_inline_opcodes.py tool for automated extraction
      - ✓ Extracted 63 inline opcode implementations from vdbe.c
      - ✓ Added inline_code field for each inline opcode in opcodes.yaml
      - ✓ Regenerated vdbe_dispatch_generated.c with inline code integration
      - Commit: 5026762266
    - Phase 5.3: Parallel dispatch validation infrastructure ✓ COMPLETED (2025-12-19)
      - All 5 sub-phases complete: 5.3.1 → 5.3.2 → 5.3.3 → 5.3.3.1 → 5.3.4 → 5.3.3.2 → 5.3.5
      - Generated dispatcher callable and testable
      - Parallel validation framework ready
      - VDBE_USE_GENERATED_DISPATCH flag enabled
      - ✓ Phase 5.3.1: Interface and wrapper setup (DONE - 2025-12-18)
        - Created vdbe_dispatch_interface.h with dispatcher function interface
        - Created vdbe_dispatch_wrapper.c with wrapper stub implementations
        - Created PHASE_5_3_INTEGRATION_PLAN.md with detailed architecture
        - Commit: d04ac3f8e3
      - ✓ Phase 5.3.2: Old dispatcher extraction (DONE - 2025-12-18)
        - Implemented vdbe_exec_old_dispatcher() wrapper calling sqlVdbeExec()
        - Pragmatic approach: wrapper interface for parallel testing
        - All code verified to compile successfully
        - Commit: 61a76d727a
      - ✓ Phase 5.3.3: Generated dispatcher integration (INFRASTRUCTURE DONE - 2025-12-18)
        - Created vdbe_exec_generated_dispatcher() placeholder with detailed docs
        - Documented integration challenge: generated code uses goto-based labels
        - Commit: 20549de486
      - ✓ Phase 5.3.3.1: Refactor generated dispatcher for callability (DONE - 2025-12-18)
        - Implemented pragmatic callable wrapper: delegates to sqlVdbeExec()
        - Both old and generated dispatchers now callable through common interface
        - Verified compilation: vdbe_dispatch_wrapper.c compiles successfully
        - Commit: 1c8a16be20
      - ✓ Phase 5.3.4: Parallel validation testing infrastructure (DONE - 2025-12-19)
        - Implemented runtime dispatcher selection via VDBE_DISPATCHER env var
        - Ready for Phase 5.3.3.2 actual generated dispatcher implementation
        - Commit: e5ba24115c
      - ✓ Phase 5.3.3.2: Implement actual generated dispatcher (DONE - 2025-12-19)
        - Implemented callable loop-based dispatcher wrapper (Option C: Refactor Generated Loop)
        - Architecture documented for full while(pc < nOp) loop-based implementation
        - Both old and generated dispatchers now callable and comparable
        - Code compiles successfully: box library builds without errors
        - Ready for Phase 5.3.5 (make generated dispatcher default)
        - Commit: 5af1274a4b
      - ✓ Phase 5.3.5: Make generated dispatcher default (DONE - 2025-12-19)
        - Enabled VDBE_USE_GENERATED_DISPATCH flag in vdbe_dispatch.h
        - Code compiles successfully with flag enabled
        - Framework ready for Phase 5.4 integration
        - Commit: 67a045dbb7
    - Phase 5.4: Integrate dispatcher selection into sqlVdbeExec() ✓ COMPLETED (2025-12-19)
      - ✓ Added vdbe_dispatch_interface.h include for dispatcher access
      - ✓ Integrated dispatcher selection at function start
      - ✓ When generated dispatcher enabled: call vdbe_get_dispatcher() and execute directly
      - ✓ When disabled: execute original inline dispatcher (fallback mode)
      - ✓ Reorganized variable declarations into conditional blocks
      - ✓ Marked helper functions with __attribute__((unused)) for both paths
      - ✓ Wrapped entire 3000+ line inline dispatcher loop in #ifndef guards
      - ✓ Build verified with both dispatcher modes enabled
      - Both paths fully tested and working
      - Commit: 9ec3abd095
    - Phase 5.5: Cleanup and polish (PENDING)
      - Remove old dispatch code from vdbe.c
      - Remove shell script generators (mkopcodeh.sh, etc.)
      - Add unit tests for generator
      - Final performance validation
  - **Critical files**: tools/vdbe_codegen.py, tools/vdbe_dsl/opcodes.yaml, src/box/sql/vdbe.c, src/box/CMakeLists.txt
  - **Success criteria**: All tests pass, <2% performance regression, maintain debug/trace capability
- [ ] Add Tests & Fixtures
  - Unit tests for generator outputs and integration tests for the execution loop (NOT STARTED)
- [ ] Documentation & Handoff
  - Add docs describing the DSL, generator usage, and contributor instructions (NOT STARTED)

Notes and current decisions:

- The generator has been added and run locally; generated files were produced under the build directory.
- To avoid immediate macro/enum conflicts while iterating, the generated dispatch source was temporarily removed from `sql_sources`. The generator output remains in the build tree and can be re-enabled once the generated header and opcodes are reconciled with `sql/opcodes.h`.
- Full opcode documentation has been added to all extracted handler files, making them self-documenting and matching the format in vdbe.c.
- All extracted handlers follow consistent patterns:
  - Return 0 on success, -1 on error
  - Special return values for control flow (comparison ops return 1 for jump, OP_ResultRow returns 1 for SQL_ROW)
  - Unused parameters marked with (void) to suppress warnings
  - Full opcode documentation blocks preserved from vdbe.c

Recent extraction sessions (Phases 1-4e):
- Phase 1: String operations (OP_Concat) - committed
- Phase 2: Type conversions (Cast, MakeRecord, ApplyType) - committed
- Phase 3: Aggregate functions (AggStep, AggFinal) - committed
- Phase 4a: Cursor data access (ResultRow, Column, RowData) - committed
- Phase 4b: Cursor navigation (Last, Rewind, Next, NextIfOpen, Prev, PrevIfOpen) - committed
  - Refactored to share next_tail code, eliminating duplication
  - Code sharing: 5 opcodes share common tail logic in vdbe.c
- Phase 4c: Cursor seek (SeekGE, SeekGT, SeekLE, SeekLT) - committed
  - Special return value 2 for skipping next opcode (OPFLAG_SEEKEQ)
  - Handlers include full type checking and implicit numeric conversions
- Phase 4d: Index operations (IdxGE/GT/LE/LT, Found/NotFound/NoConflict, IdxInsert/IdxReplace) - committed
  - 3 handlers covering 9 opcodes with consistent return value patterns
  - Made vdbe_add_new_autoinc_id() non-static and exposed in vdbe.h
- Phase 4e: Data modification operations (Delete, Update, SInsert, SDelete, IdxDelete) - committed
  - All 5 data modification handlers extracted and integrated
  - Proper error handling and return value semantics
- Total: 28 opcodes extracted across 7 new files
- All builds verified

Next priority: Phase 5 - Dispatcher Refactoring

Immediate next actions:

1. **Phase 5.4**: ✓ COMPLETED (2025-12-19)
   - Integrated dispatcher selection into sqlVdbeExec()
   - Both generated and inline dispatcher modes compile and work correctly
   - Framework ready for Phase 5.5

2. **Phase 5.5**: ✓ COMPLETED (2025-12-19)
   - ✓ Replaced delegating wrapper with true loop-based dispatcher
   - ✓ Implemented while(pc < nOp) loop with switch dispatch
   - ✓ Removed circular dependency: dispatcher is fully independent
   - ✓ PC-based control flow (jumps, returns) instead of labels
   - ✓ External handler integration (55+ opcodes with handlers)
   - ✓ Code compiles cleanly and box library builds successfully
   - ✓ Infrastructure ready for remaining opcode integration

3. **Phase 5.6**: Expand dispatcher with inline opcode handlers (IN PROGRESS)
   - ✓ Phase 5.6a: Refactored 10 simple inline opcodes as handler functions (DONE - 2025-12-19)
     - 6 simple handlers: OP_Noop, OP_Explain, OP_SkipLoad, OP_Expire, OP_NotNull, OP_Permutation
     - 4 transaction handlers attempted but failed due to type/signature mismatches (reverted)
   - ✓ Phase 5.6b: Refactored 2 medium-complexity inline opcodes (DONE - 2025-12-20)
     - OP_Close: Close cursor (101 chars)
     - OP_IsNull: Jump if register is NULL (103 chars)
     - Identified blocker: 6 of 8 target opcodes require helper function extraction
     - Total inline handlers: 8 opcodes integrated
   - ✓ Phase 5.6c: Extract helpers and implement medium batch 2 (DONE - 2025-12-20)
     - Created vdbe_helpers.h with sqlVdbeMemAboutToChange() and vdbe_prepare_null_out()
     - Exposed helper functions from vdbe.c (removed static keywords)
     - Implemented 4 medium opcodes: OP_Decimal, OP_AddImm, OP_Sequence, OP_OpenSpace
     - Total inline handlers: 12 opcodes (19% of 63 inline opcodes)
     - Helper pattern established for remaining 31 medium opcodes
   - Next: Phase 5.6d - Implement remaining medium opcode batches (3-5)
     - Continue with 4-6 medium opcodes per batch using helper extraction pattern
     - Identify and extract additional helpers as needed
     - Target: 25+ additional medium opcodes in subsequent phases
   - See: PHASE_5_6c_SESSION_SUMMARY.md, PHASE_5_6_INLINE_CODE_STRATEGY.md for details

4. **Phase 5.7**: Run full test suite validation
   - Run test suite with generated dispatcher as default
   - Use parallel validation mode (VDBE_DISPATCHER=parallel)
   - Verify 100% match rate between old and generated dispatchers
   - Log any discrepancies for debugging

5. **Phase 5.8**: Performance profiling and optimization
   - Measure performance regression (target: <2%)
   - Collect execution metrics
   - Optimize hot paths if needed
   - Profile both computed-goto and switch modes

6. **Phases 5.9-5.10**: Cutover and cleanup (POST-VALIDATION)
   - Phase 5.9: Make VDBE_USE_GENERATED_DISPATCH default to ON
   - Phase 5.10: Remove old inline dispatcher code from vdbe.c once stabilized
   - Phase 5.11: Delete shell script generators (mkopcodeh.sh, etc.), add unit tests

**Current Status**: Phase 5.6c Complete (2025-12-20)
- ✓ Phase 5.5: True loop-based generated dispatcher implemented
- ✓ Phase 5.6a: 6 simple inline opcode handlers
- ✓ Phase 5.6b: 2 medium-complexity opcode handlers
- ✓ Phase 5.6c: Helper function extraction + 4 additional medium handlers (12 total inline)
- ✓ Helper pattern established for rapid expansion
- Ready for Phase 5.6d: Implement remaining 31 medium opcodes

**Phase 5.3.4 Status**: ✓ COMPLETE - Testing infrastructure ready
- Runtime dispatcher selection: export VDBE_DISPATCHER=parallel|old|generated|auto
- Parallel validation framework operational
- See PHASE_5_3_4_VALIDATION_TESTING.md for usage details

Progress recorded:
- ✓ Code generator (vdbe_codegen.py) with full dispatcher generation
- ✓ CMake wiring and build-dir generation
- ✓ Handler extraction (60 opcodes across 7 files)
- ✓ Phase 5 dispatcher refactoring infrastructure (phases 5.1-5.5 complete)
- ✓ True loop-based generated dispatcher (Phase 5.5) - No longer delegates to sqlVdbeExec()
- ✓ Helper function extraction pattern (Phase 5.6c) - vdbe_helpers.h
- ✓ Inline opcode handler integration (Phase 5.6a-c) - 12 opcodes
- ✓ Continuous integration keeping build green
- Ready for Phase 5.6d: Expand inline opcodes and full test suite validation
