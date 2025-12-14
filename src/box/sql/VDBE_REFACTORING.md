# VDBE Opcode Handler Extraction - Refactoring Notes

## Overview

This document tracks the extraction of VDBE opcode handlers from the monolithic `vdbe.c` file into separate, modular files. This is part of a larger refactoring effort to introduce a DSL-based code generation system for the VDBE interpreter.

## Current Status

### Completed Extractions (Initial Phase)

1. **Arithmetic Operators** (`vdbe_ops_arith.c`) - 5 opcodes
   - OP_Add, OP_Subtract, OP_Multiply, OP_Divide, OP_Remainder
   - Status: ✓ COMPLETED & INTEGRATED
   - All handlers use mem_xxx() functions (mem_add, mem_sub, mem_mul, mem_div, mem_rem)
   - Return 0 on success, -1 on error

2. **Data/Constant Operators** (`vdbe_ops_data.c`) - 11 opcodes
   - OP_Integer, OP_Bool, OP_Int64, OP_Real, OP_String
   - OP_Null, OP_Blob, OP_Variable
   - OP_Move, OP_Copy, OP_SCopy
   - Status: ✓ COMPLETED & INTEGRATED
   - Simple register operations with no jumps

3. **Comparison Operators** (`vdbe_ops_compare.c`) - 6 opcodes
   - OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge
   - Status: ✓ COMPLETED & INTEGRATED
   - **Solution Implemented**: Option A - Added `iCompare` to `struct Vdbe`
     - Moved `int iCompare` from local variable to `struct Vdbe` at line 234 in vdbeInt.h
     - Updated all references in vdbe.c to use `p->iCompare`
     - Handlers return special values for jump control:
       - 0 = VDBE_CMP_CONTINUE (continue to next instruction)
       - 1 = VDBE_CMP_JUMP (jump to P2)
       - -1 = VDBE_CMP_ERROR (error occurred)

4. **Logical/Bitwise Operators** (`vdbe_ops_logical.c`) - 6 opcodes
   - Boolean logic: OP_And, OP_Or, OP_Not (three-valued SQL logic)
   - Bitwise ops: OP_BitAnd, OP_BitOr, OP_BitNot
   - Status: ✓ COMPLETED & INTEGRATED
   - Clean implementations with proper NULL handling

### Recent Extraction Sessions (Phases 1-4a)

5. **Phase 1: String Operations** (`vdbe_ops_string.c`) - 1 opcode
   - OP_Concat (string concatenation)
   - Status: ✓ COMMITTED
   - Simple string concatenation using mem_concat()

6. **Phase 2: Type Conversions** (`vdbe_ops_type.c`) - 3 opcodes
   - OP_Cast (explicit type casting)
   - OP_ApplyType (implicit type checking/conversion for register ranges)
   - OP_MakeRecord (tuple encoding to msgpack format)
   - Status: ✓ COMMITTED
   - Handles type system interactions and tuple serialization

7. **Phase 3: Aggregate Functions** (`vdbe_ops_aggregate.c`) - 2 opcodes
   - OP_AggStep (execute aggregate step function)
   - OP_AggFinal (execute aggregate finalizer)
   - Status: ✓ COMMITTED
   - Aggregate state management and function execution

8. **Phase 4a: Cursor Data Access** (`vdbe_ops_cursor_data.c`) - 3 opcodes
   - OP_ResultRow (send result row to client - uses special return value 1 for SQL_ROW)
   - OP_Column (extract column from cursor row)
   - OP_RowData (fetch complete row data from cursor)
   - Status: ✓ COMMITTED
   - **Note**: OP_ResultRow returns 1 to signal SQL_ROW, similar to comparison ops
   - **Note**: Trace functionality (db->mTrace) removed from ResultRow (db not accessible in handler context)

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

## Extraction Statistics

**Total opcodes extracted: 37 across 8 files**

- Initial phase: 28 opcodes (arithmetic, data, comparison, logical/bitwise)
- Recent sessions (Phases 1-4a): 9 opcodes (string, type, aggregates, cursor data)
- All builds verified with `-Wall -Wextra -Werror`
- All extractions committed to git

## Remaining Extraction Phases

According to the extraction plan (see TODO.md), 23 opcodes remain across 4 phases:

### Phase 4b: Cursor Navigation Operations - 6 opcodes
- OP_Next, OP_NextIfOpen, OP_Prev, OP_PrevIfOpen
- OP_Rewind, OP_Last
- File: `vdbe_ops_cursor_nav.c`
- Complexity: High - iterator control and cursor state management

### Phase 4c: Cursor Seek Operations - 4 opcodes
- OP_SeekGE, OP_SeekGT, OP_SeekLE, OP_SeekLT
- File: `vdbe_ops_cursor_seek.c`
- Complexity: High - B-tree seeking

### Phase 4d: Index Operations - 8 opcodes
- OP_IdxGE, OP_IdxGT, OP_IdxLE, OP_IdxLT
- OP_Found, OP_NotFound, OP_NoConflict
- OP_IdxInsert
- File: `vdbe_ops_index.c`
- Complexity: High - index manipulation

### Phase 4e: Data Modification Operations - 5 opcodes
- OP_Delete, OP_Update
- OP_SInsert, OP_SDelete
- OP_IdxDelete
- File: `vdbe_ops_modify.c`
- Complexity: High - storage engine interaction

### Deferred Operations
- Control flow ops (OP_Goto, OP_Jump, OP_If, OP_IfNot, etc.) remain in vdbe.c
- Reason: PC manipulation complexity - better handled with dispatcher refactoring
- See `vdbe_ops_control.c` for extraction options

## Next Steps

1. **Continue Handler Extraction** (Phases 4b-4e)
   - Follow established workflow: Extract → Build → Commit → Next Phase
   - Each phase independently buildable and testable
   - Update TODO.md and this file after each phase

2. **Generate Dispatcher**
   - Once handlers are extracted, generate the dispatcher
   - Options:
     - Jump table (computed goto)
     - Function pointer array
     - Generated switch statement

3. **Add Tests**
   - Unit tests for individual handlers
   - Integration tests for full execution
   - Performance benchmarks

4. **Documentation**
   - DSL usage guide
   - Contributor instructions
   - Architecture overview

## File Organization

```
src/box/sql/
├── vdbe.c                       # Main execution loop
├── vdbe_ops.h                   # Handler function prototypes (all 37 handlers)
│
├── vdbe_ops_arith.c             # ✓ Arithmetic operators (5 opcodes)
├── vdbe_ops_data.c              # ✓ Data/constant operators (11 opcodes)
├── vdbe_ops_compare.c           # ✓ Comparison operators (6 opcodes)
├── vdbe_ops_logical.c           # ✓ Logical/bitwise operators (6 opcodes)
│
├── vdbe_ops_string.c            # ✓ String operations (1 opcode) - Phase 1
├── vdbe_ops_type.c              # ✓ Type conversions (3 opcodes) - Phase 2
├── vdbe_ops_aggregate.c         # ✓ Aggregate functions (2 opcodes) - Phase 3
├── vdbe_ops_cursor_data.c       # ✓ Cursor data access (3 opcodes) - Phase 4a
│
├── vdbe_ops_cursor_nav.c        # [ ] Cursor navigation (6 opcodes) - Phase 4b
├── vdbe_ops_cursor_seek.c       # [ ] Cursor seek (4 opcodes) - Phase 4c
├── vdbe_ops_index.c             # [ ] Index operations (8 opcodes) - Phase 4d
├── vdbe_ops_modify.c            # [ ] Data modification (5 opcodes) - Phase 4e
│
├── vdbe_ops_control.c           # [DEFERRED] Control flow ops (extraction plan only)
├── vdbe_helpers.c               # Shared helper functions
└── VDBE_REFACTORING.md          # This file
```

**Legend:**
- ✓ = Extracted and committed
- [ ] = Planned for extraction
- [DEFERRED] = Postponed until dispatcher refactoring

## Handler Patterns and Conventions

All extracted handlers follow consistent patterns established during the refactoring:

### Function Signature
```c
int vdbe_op_xxx(Vdbe *p, Op *pOp, Mem *aMem)
```

### Return Values
- **0** = Success, continue to next instruction
- **-1** = Error occurred (jump to `abort_due_to_error`)
- **1** = Special control flow (used by comparison ops and OP_ResultRow):
  - Comparison ops: jump to P2
  - OP_ResultRow: return SQL_ROW to caller

### Integration Pattern in vdbe.c
```c
EXECUTE(OP_Xxx,(P1,P2,P3)): {
    if (vdbe_op_xxx(p, pOp, aMem))
        goto abort_due_to_error;
    DISPATCH();
}
```

For handlers with special return values (comparisons, ResultRow):
```c
EXECUTE(OP_Xxx,(P1,P2)): {
    int handler_rc = vdbe_op_xxx(p, pOp, aMem);
    if (handler_rc < 0)
        goto abort_due_to_error;
    if (handler_rc == 1) {
        /* Special handling (jump or return SQL_ROW) */
    }
    DISPATCH();
}
```

### Documentation
- Every handler includes full opcode documentation block from vdbe.c
- Format: `/* Opcode: Name P1 P2 P3 P4 P5 */` with synopsis and detailed description
- Preserves all parameter explanations and special cases

### Common Practices
- Unused parameters marked with `(void)param;` to suppress warnings
- Use `vdbe_prepare_null_out()` for output register preparation
- Use `UPDATE_MAX_BLOBSIZE()` macro after writing blobs/strings
- Use `REGISTER_TRACE()` for debugging (only in SQL_DEBUG builds)
- Assert preconditions for parameter validation

### Error Handling
- Use `diag_set(ClientError, ...)` to set error diagnostics before returning -1
- Never use `goto` to labels outside the handler
- Let main loop handle error cleanup via `abort_due_to_error` label

## References

- **Main refactoring TODO**: [TODO.md](/home/tsafin/tarantool/TODO.md)
- **Extraction plan**: [~/.claude/plans/handler-extraction-plan.md](/home/tsafin/.claude/plans/handler-extraction-plan.md)
- **VDBE internals**: [vdbeInt.h](vdbeInt.h) - struct Vdbe, VdbeCursor, Op definitions
- **Main execution loop**: [vdbe.c](vdbe.c) - sqlVdbeExec() function
- **Handler prototypes**: [vdbe_ops.h](vdbe_ops.h) - all 37 handler declarations

## History

- **Initial extraction** (pre-phases): 28 opcodes across 4 files
- **Phase 1** (2025-01): String operations - 1 opcode
- **Phase 2** (2025-01): Type conversions - 3 opcodes
- **Phase 3** (2025-01): Aggregate functions - 2 opcodes
- **Phase 4a** (2025-01): Cursor data access - 3 opcodes
- **Total**: 37 opcodes extracted, 23 remaining in planned phases 4b-4e
