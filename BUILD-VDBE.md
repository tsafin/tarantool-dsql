# VDBE Refactoring - Build Workflow

## Quick Build Commands

For VDBE development, we only need to build the core components, not the full test suite.

### From the `build/` directory:

```bash
# Build just the box library (contains all VDBE code)
make box -j12

# Build the server library
make server -j12

# Clean and rebuild VDBE files specifically
rm -f src/box/CMakeFiles/box.dir/sql/vdbe*.c.o
make box -j12
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

✅ **All VDBE components build successfully with zero warnings**

Last verified: 2024-12-14
