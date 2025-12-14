# VDBE Opcode Handler Extraction - Refactoring Notes

## Overview

This document tracks the extraction of VDBE opcode handlers from the monolithic `vdbe.c` file into separate, modular files. This is part of a larger refactoring effort to introduce a DSL-based code generation system for the VDBE interpreter.

## Current Status

### Completed Extractions

1. **Arithmetic Operators** (`vdbe_ops_arith.c`)
   - OP_Add, OP_Subtract, OP_Multiply, OP_Divide, OP_Remainder
   - Status: FUNCTIONAL implementations (✓ COMPLETED & INTEGRATED)
   - All handlers use mem_xxx() functions (mem_add, mem_sub, mem_mul, mem_div, mem_rem)
   - Integrated into main loop at vdbe.c:948-1012
   - Return 0 on success, -1 on error

2. **Data/Constant Operators** (`vdbe_ops_data.c`)
   - OP_Integer, OP_Bool, OP_Int64, OP_Real, OP_String
   - OP_Null, OP_Blob, OP_Variable
   - OP_Move, OP_Copy, OP_SCopy
   - Status: FUNCTIONAL implementations
   - These are simple register operations with no jumps
   - Already extracted and working

### Completed

3. **Comparison Operators** (`vdbe_ops_compare.c`)
   - OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge
   - Status: FUNCTIONAL implementations (✓ COMPLETED)
   - **Solution Implemented**: Option A - Added `iCompare` to `struct Vdbe`
     - Moved `int iCompare` from local variable to `struct Vdbe` at line 234 in vdbeInt.h
     - Updated all references in vdbe.c to use `p->iCompare`
     - Handlers return special values for jump control:
       - 0 = VDBE_CMP_CONTINUE (continue to next instruction)
       - 1 = VDBE_CMP_JUMP (jump to P2)
       - -1 = VDBE_CMP_ERROR (error occurred)

## The iCompare/Jump Problem - SOLVED ✓

### Previous Architecture

In `vdbe.c`, the main execution loop `sqlVdbeExec()` had:

```c
int iCompare = 0;  // Local variable - NO LONGER EXISTS
```

Comparison operators (Eq, Ne, Lt, Le, Gt, Ge) needed to:
1. Set `iCompare` to the comparison result
2. Use this value in subsequent operations (OP_ElseNotEq, OP_Jump)
3. Conditionally jump using `JUMP_P2()` macro

### Implemented Solution (Option A)

#### Option A: Add iCompare to struct Vdbe (✓ IMPLEMENTED)

**Changes made:**
- ✓ Added `int iCompare` to `struct Vdbe` in `vdbeInt.h` line 234
- ✓ Modified all 10 references in vdbe.c to use `p->iCompare`
- ✓ Implemented all 6 comparison handlers in vdbe_ops_compare.c
- ✓ Handlers use return values for jump control (0/1/-1)

**Result:**
- Clean handler interface (no extra parameters needed)
- Consistent with other Vdbe execution state (like `pc`)
- Successfully compiled and linked with no warnings

**Trade-offs:**
- Adds 4 bytes to every Vdbe struct (acceptable overhead)
- iCompare is properly execution state, similar to program counter (pc)

#### Option B: Pass iCompare as pointer parameter

**Changes required:**
- Change all handler signatures: `int vdbe_op_xxx(Vdbe *p, Op *pOp, Mem *aMem, int *iCompare)`
- Update all existing handlers
- Pass `&iCompare` from main loop

**Pros:**
- No struct changes
- Explicit data flow

**Cons:**
- Changes all handler signatures
- More parameters to pass
- Inconsistent with existing handlers

#### Option C: Special return value encoding

**Changes required:**
- Use return value to encode: error, success, jump target, iCompare value
- E.g., `return (jump_target << 16) | (iCompare & 0xFFFF)`

**Pros:**
- No signature changes
- No struct changes

**Cons:**
- Complex and error-prone
- Limits range of return values
- Hard to understand and maintain

#### Option D: Keep comparison ops in vdbe.c for now

**Changes required:**
- None immediately
- Extract after dispatcher refactoring is complete

**Pros:**
- No immediate work needed
- Can revisit with better understanding

**Cons:**
- Delays refactoring
- Comparison ops remain in monolithic file

## Recommended Next Steps

1. **Implement Option A** (add iCompare to struct Vdbe)
   - Modify `struct Vdbe` in `vdbeInt.h`
   - Update `sqlVdbeExec()` to use `p->iCompare`
   - Update all uses of `iCompare` in vdbe.c
   - Implement real handlers in `vdbe_ops_compare.c`

2. **Handle Jump Mechanism**
   - Comparison operators need to signal jumps to the main loop
   - Options:
     a. Special return value (e.g., 1 = jump, 0 = continue, -1 = error)
     b. Set a flag in Vdbe struct: `p->shouldJump = true; p->jumpTarget = P2;`
     c. Return jump address directly (requires convention)

3. **Extract Remaining Opcodes**
   - Group by category:
     - Control flow: OP_Jump, OP_Gosub, OP_Return, OP_Yield
     - Cursor operations: OP_OpenRead, OP_OpenWrite, OP_SeekGT, etc.
     - Aggregate functions: OP_AggStep, OP_AggFinal
     - etc.

4. **Generate Dispatcher**
   - Once handlers are extracted, generate the dispatcher
   - Options:
     - Jump table (computed goto)
     - Function pointer array
     - Generated switch statement

5. **Add Tests**
   - Unit tests for individual handlers
   - Integration tests for full execution
   - Performance benchmarks

## File Organization

```
src/box/sql/
├── vdbe.c                     # Main execution loop
├── vdbe_ops.h                 # Handler function prototypes
├── vdbe_ops_arith.c           # Arithmetic operators
├── vdbe_ops_data.c            # Data/constant operators
├── vdbe_ops_compare.c         # Comparison operators (STUBS)
├── vdbe_helpers.c             # Shared helper functions
└── VDBE_REFACTORING.md        # This file
```

## References

- Main refactoring TODO: `/home/tsafin/tarantool/TODO.md`
- VDBE internals: `src/box/sql/vdbeInt.h`
- Original implementation: `src/box/sql/vdbe.c` lines 1351-1463
