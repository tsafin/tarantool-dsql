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
  - Arithmetic handlers: `src/box/sql/vdbe_ops_arith.c` (DONE - stubs)
  - Data/constant handlers: `src/box/sql/vdbe_ops_data.c` (DONE - functional)
  - Comparison handlers: `src/box/sql/vdbe_ops_compare.c` (DONE - functional & integrated)
    - Successfully extracted after adding `iCompare` to `struct Vdbe` (Option A from extraction plan)
    - Handlers return special values (0=continue, 1=jump, -1=error) for jump control
    - Main loop integration complete (✓ all 6 comparison ops now use extracted handlers)
    - Build verified: compiles cleanly with -Wall -Wextra -Werror
  - Gradual extraction of remaining opcodes planned (IN-PROGRESS)
- [ ] Replace Switch with Dispatcher
  - Replace big `switch` in `vdbe.c` with generated dispatcher/jump-table (NOT STARTED)
- [ ] Add Tests & Fixtures
  - Unit tests for generator outputs and integration tests for the execution loop (NOT STARTED)
- [ ] Documentation & Handoff
  - Add docs describing the DSL, generator usage, and contributor instructions (NOT STARTED)

Notes and current decisions:

- The generator has been added and run locally; generated files were produced under the build directory.
- To avoid immediate macro/enum conflicts while iterating, the generated dispatch source was temporarily removed from `sql_sources`. The generator output remains in the build tree and can be re-enabled once the generated header and opcodes are reconciled with `sql/opcodes.h`.
- Next recommended step: reconcile generated opcode names/flags with existing `sql/opcodes.h` (or scope the generated names to avoid macro collisions), then re-enable `vdbe_dispatch_generated.c` in the build and begin moving a small set of real handlers into separate `vdbe_ops_*.c` files with tests.

Next actions you can request:

- Reconcile the generator output (rename or guard generated macros to avoid conflicts) and re-enable the generated dispatch in the build.
- Start moving a small opcode group (arithmetic) from `vdbe.c` into `src/box/sql/vdbe_ops_arith.c` with proper implementations and add focused tests.

Progress recorded: generator, CMake wiring, build-dir generation, temporary handler stubs, and fixes to keep build green.
