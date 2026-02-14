# Claude Notes

## Git Operations

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
- **Always clean *.snap files before running**: `rm -f *.snap` to avoid state from previous runs
- The tarantool executable expects to run from the build root, not the source root

## Lua Test Scripts (Tarantool)

- Always run `box.cfg{}` before any `box.execute` operations.
- Always call `os.exit(rc)` at the end of the script to exit the event loop.
## SQL JIT Implementation (Feb 5, 2026)

### CMake Build System

- Added `ENABLE_SQL_JIT` option in main CMakeLists.txt (default: OFF)
- When enabled, requires LLVM 11+ (12+ recommended) with OrcJIT components
- Bitcode files (.bc) generated for handler modules: vdbe_ops_arith.c, vdbe_ops_compare.c, vdbe_ops_logical.c, vdbe_ops_data.c, vdbe_ops_cursor_data.c, vdbe_ops_index.c, vdbe_ops_string.c, vdbe_ops_type.c
- Install location: `${CMAKE_INSTALL_DATAROOTDIR}/tarantool/sql_handlers/*.bc`
- Build requires: LLVM development packages (llvm-11-dev or higher on Debian/Ubuntu)
- Fixed compiler compatibility: gnu-alignof-expression warning now only for Clang

### Implementation Status

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
- [x] Phase 1: Basic JIT compilation infrastructure
  - Created LLVM module and function for each VDBE program
  - Implemented JIT function signature: int(struct Vdbe *, int start_pc)
  - Added module verification and compilation to native code
  - Minimal implementation returns -1 (execution complete) for testing
- [x] Phase 2: Opcode analysis and classification (jitable/callable/unsupported)
  - Opcode classification table (JIT_MODE_INLINE/CALL/UNSUPPORTED)
  - Opcode handler name mapping table
  - Scan program to decide if JIT compilation is worthwhile
- [x] Phase 3: Load and link handler bitcode modules
  - Clone and link all handler .bc modules into JIT module
  - Handler functions available by name for call generation
- [x] Phase 4: Compute actual pOp and aMem pointers for handler calls
  - Use offsetof(struct Vdbe, aOp/aMem) and sizeof(Op) constants
  - LLVM GEP/Load to compute &p->aOp[i] and p->aMem at runtime
  - Pass actual pointers to handler functions instead of NULL
- [x] Phase 5: Apply LLVM optimization passes
  - Per-function passes: mem2reg, instcombine, reassociate, GVN, CFG simplification
  - Module-level passes: function inlining, global DCE, instcombine, CFG simplification
  - Added LLVM components: Analysis, BitReader, Linker, ScalarOpts, InstCombine, TransformUtils, IPO, MCJIT
- [x] Fixed CMake: moved LLVM setup before add_subdirectory(src) so LLVM_LIBS is available at link time

**Step 4 - COMPLETED**: Integrate JIT into VDBE execution loop
- [x] Call vdbe_jit_compile() in sqlVdbeMakeReady() at PREPARE time
  - Compilation happens once per prepared statement
  - Failure is non-fatal; interpreter used as fallback
- [x] Invoke JIT function in sqlVdbeExec() before dispatcher
  - JIT executes from p->pc until unsupported opcode
  - Returns PC of unsupported opcode for interpreter continuation
  - Returns -1 if all opcodes handled (returns SQL_DONE)
- [x] Clean up JIT resources in sqlVdbeClearObject()
  - Called when VDBE is finalized
- [x] Mark OP_ResultRow as UNSUPPORTED (requires special SQL_ROW return handling)
- [x] Add vdbe_jit.h includes to vdbe.c and vdbeaux.c

**Generated Dispatcher Status (Feb 14, 2026)**:
- Currently handles 108 of 176 opcodes (61% complete) - up from 52 (30%)
- Batch 1 (commit ab21afec42): Added 26 critical opcodes
  - Comparison: Eq, Ne, Lt, Le, Gt, Ge
  - Logical: And, Or, Not, If
  - Data ops: Bool, Blob, Copy, Cast, ApplyType, Concat
  - Cursor: Column, MakeRecord, Found, NotFound
  - Modification: Delete, Update, AggStep, AggFinal
  - Transaction: TTransaction (inline implementation)
- Batch 2 (current): Added 30 cursor/iteration opcodes
  - Iterator: IteratorOpen, Rewind, Next, Prev, Last, NextIfOpen, PrevIfOpen
  - Seek: SeekLT, SeekGT, SeekLE, SeekGE
  - Index: IdxInsert, IdxReplace, IdxGE, IdxGT, IdxLE, IdxLT, IdxDelete
  - System: SInsert, SDelete, RowData
  - Data types: Int64, Real, Null, Variable, Move, SCopy
  - Bitwise: BitAnd, BitOr, BitNot
- **Known Issue**: OP_NoConflict disabled - operand validation differs between generated/inline dispatchers
- Still need 68 more opcodes for full coverage
- See DISPATCHER_STATUS.md for detailed tracking
