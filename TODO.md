```markdown
# TODO: VDBE Refactor — Current Status

This file tracks progress for the `src/box/sql/vdbe.c` refactor.

- [x] Analyze `vdbe.c`
  - Map responsibilities, data structures, helpers, and the execution loop. (DONE)
- [x] Define Module Boundaries
  - Proposed modules and file mapping; low-risk helper extraction decided. (DONE)
- [~] Design DSL for Op Spec
  - Small YAML sketch created at `tools/vdbe_dsl/opcodes.yaml` and a generator `tools/vdbe_codegen.py` (IN-PROGRESS)
- [ ] Implement Code Generator
  - Emit `vdbe_opcodes_generated.h` and `vdbe_dispatch_generated.c` from the DSL (NOT STARTED — generator added, needs run)
- [ ] Split Handlers into Files
  - Move opcode handlers to `vdbe_ops_*.c/.h` and extract shared utilities (NOT STARTED)
- [ ] Replace Switch with Dispatcher
  - Generate dispatcher/jump-table and integrate into `vdbe.c` (NOT STARTED)
- [ ] Add Tests & Fixtures
  - Unit and integration tests for generator outputs and execution loop (NOT STARTED)
- [ ] Integrate Codegen in Build
  - Wire generator into CMake so generated sources are produced during build (NOT STARTED)
- [ ] Documentation & Handoff
  - Add `docs/VDBe-Refactor.md` and contributor instructions (NOT STARTED)

Notes:
- The DSL generator and a small opcode sample were added under `tools/`.
- Next concrete actions: run the generator to produce skeletal generated files, review outputs, then incrementally move a small group of opcode handlers into `vdbe_ops_*.c`.

```

