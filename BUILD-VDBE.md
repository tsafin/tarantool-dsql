# VDBE Refactoring - Build Workflow

## Quick Build Commands

For VDBE development, prefer building the executable you are going to run.

### From the build directory:

```bash
# Rebuild the executable after SQL/VDBE changes
make tarantool -j12

# Optional: rebuild only the static SQL/box library to check compilation
make box -j12

# Clean and rebuild VDBE files specifically
rm -f src/box/CMakeFiles/box.dir/sql/vdbe*.c.o
make tarantool -j12
```

### Verify VDBE compilation:

```bash
# Check that all VDBE object files were built
ls -lh src/box/CMakeFiles/box.dir/sql/vdbe*.c.o

# Expected files:
# - vdbe.c.o               (main execution loop)
# - vdbe_ops_compare.c.o   (comparison operators)
# - vdbe_ops_data.c.o      (data/constant operators)
# - vdbe_ops_arith.c.o     (arithmetic operators)
# - vdbeaux.c.o            (auxiliary functions)
# - vdbeapi.c.o            (API functions)
# - vdbesort.c.o           (sorting)
# - vdbetrace.c.o          (tracing/debugging)
```

### Test compilation of a single file:

```bash
# From build/ directory
# Touch the file to force rebuild
touch ../src/box/sql/vdbe.c
make box -j12 2>&1 | grep vdbe.c
```

## SQL/JIT profiling build notes

Debug builds now enable:

- `SQL_DEBUG=1`
- `SQL_VDBE_OP_PROFILE=1`

This means `box.stat.sql()` includes per-opcode interpreter/JIT counters and
accumulated microsecond timings by default in Debug builds. Release builds keep
opcode profiling off unless `SQL_VDBE_OP_PROFILE` is defined explicitly.

### JIT debug build example

```bash
cmake -S . -B build-jit-debug2 \
      -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_SQL_JIT=ON
cmake --build build-jit-debug2 --target tarantool
```

### Quick stats probe

```bash
cd build-jit-debug2
rm -f *.snap *.xlog
SQL_JIT_ENABLE=1 ./src/tarantool /absolute/path/to/probe.lua
```

Inside `probe.lua`, inspect:

```lua
local stats = box.stat.sql()
print(require('yaml').encode(stats))
```

Relevant fields:

- aggregate counters such as `sql_interpreter_step_count` and `sql_jit_exec_count`
- `interpreter_opcode_profile.count/time_us`
- `jit_opcode_profile.count/time_us`

## Why Not `make -j12` (All Targets)?

The full build includes test targets that have pre-existing issues:
- Macro redefinition errors in test files
- GCC/Clang compiler flag incompatibilities

These are **not related to our VDBE refactoring** and don't affect the core library.

## CMake Configuration

Current build configuration:
```bash
cd build
cmake -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DENABLE_BACKTRACE=OFF \
      ..
```

Key settings:
- **Debug mode**: `-g -ggdb -O0` for debugging
- **Strict warnings**: `-Wall -Wextra -Werror`
- **compile_commands.json**: For IDE integration

## Build Status

- For SQL/VDBE changes, `make tarantool` is the reliable target because `make box`
  does not refresh the runnable binary.
