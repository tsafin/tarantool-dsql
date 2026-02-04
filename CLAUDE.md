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

**Step 3 - NEXT**: Generate JIT function by linking handler IR
- Implement actual JIT compilation logic in vdbe_jit_compile()
- Load handler bitcode modules from installation directory
- Link and inline handler functions based on opcode analysis
- Apply LLVM optimization passes and compile to native code
