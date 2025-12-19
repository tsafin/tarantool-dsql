# Phase 5.6h - Medium Batch 7 Implementation Plan

**Status**: Ready to implement
**Date Created**: 2025-12-20
**Target**: 8 medium-complexity opcodes

---

## Executive Summary

Phase 5.6h will implement 8 medium-complexity opcodes across 3 functional groups:
- **Bitwise Arithmetic** (2): OP_ShiftLeft, OP_ShiftRight
- **Value Loading & Data Structures** (4): OP_String8, OP_Array, OP_Map, OP_Getitem
- **Cursor & Aggregation** (2): OP_OpenPseudo, OP_Count

**Expected Outcomes**:
- Inline handlers: 34 → 42 (54% → 67%)
- Total dispatcher: 97 → 105 opcodes (55% → 60%)
- New helpers: 0 expected (pattern validation continues)

---

## Implementation Checklist

### Pre-Implementation
- [ ] Create vdbe_ops_inline_medium_7.c file
- [ ] Verify all dependencies available in mem.h and cursor headers
- [ ] Review dispatcher integration pattern from Phase 5.6g

### Core Implementation
- [ ] Implement OP_ShiftLeft (249 chars)
- [ ] Implement OP_ShiftRight (250 chars)
- [ ] Implement OP_String8 (278 chars)
- [ ] Implement OP_Array (288 chars)
- [ ] Implement OP_Map (284 chars)
- [ ] Implement OP_Getitem (216 chars)
- [ ] Implement OP_OpenPseudo (231 chars)
- [ ] Implement OP_Count (224 chars)

### Integration
- [ ] Add 8 dispatcher cases to vdbe_dispatch_wrapper.c
- [ ] Update CMakeLists.txt to include new source file
- [ ] Verify prototypes in vdbe_ops.h

### Quality Assurance
- [ ] Build: `cmake --build . --target generate_sql_files`
- [ ] Brace validation (python check)
- [ ] Git status verification
- [ ] Syntax check all implementations

### Documentation
- [ ] Create PHASE_5_6h_SESSION_SUMMARY.md
- [ ] Update PHASE_5_6_QUICK_REFERENCE.md (status, phase progress)
- [ ] Update TODO.md with Phase 5.6h completion

---

## Opcode Details

### Group A: Bitwise Arithmetic (2 opcodes, ~499 chars total)

#### OP_ShiftLeft (ID: 22)
```
Handler: vdbe_op_shiftleft_inline()
Size: 249 chars
Registers: P1 (shift amount), P2 (value), P3 (output)
Pattern: r[P3] = r[P2] << r[P1]
Dependencies: mem_shift_left()
Error Handling: Standard return -1 on error
```

**Implementation Notes**:
- Get P1 and P2 registers with shift amount and value
- Call mem_shift_left() to perform bitwise operation
- Store result in P3
- Return 0 for normal flow

#### OP_ShiftRight (ID: 23)
```
Handler: vdbe_op_shiftright_inline()
Size: 250 chars
Registers: P1 (shift amount), P2 (value), P3 (output)
Pattern: r[P3] = r[P2] >> r[P1]
Dependencies: mem_shift_right()
Error Handling: Standard return -1 on error
```

**Implementation Notes**:
- Nearly identical to OP_ShiftLeft but calls mem_shift_right()
- High code reuse opportunity
- Clear error propagation

---

### Group B: Value Loading & Data Structures (4 opcodes, ~1066 chars total)

#### OP_String8 (ID: 48)
```
Handler: vdbe_op_string8_inline()
Size: 278 chars
Registers: P2 (output)
Pattern: Load C string constant, convert to OP_String
Dependencies: sqlStrlen30()
Opcode Self-Modification: Converts pOp->opcode to OP_String
Special: Falls through to OP_String handler
```

**Implementation Notes**:
- Extract C string from P4 field
- Calculate length using sqlStrlen30()
- Modify opcode to OP_String and fall through
- Next execution uses standard string handler

#### OP_Array (ID: 69)
```
Handler: vdbe_op_array_inline()
Size: 288 chars
Registers: P1 (count), P3 (first register), P2 (output)
Pattern: Create msgpack array from registers P3 to P3+P1-1
Dependencies: mem_encode_array(), mem_copy_array(), fiber_alloc()
Memory: Uses fiber GC region for temporary encoding
```

**Implementation Notes**:
- Allocate fiber GC region for encoding buffer
- Encode registers P3 through P3+P1-1 as msgpack array
- Copy encoded data to output register P2
- Clean up fiber GC region
- Return 0 for success, -1 for error

#### OP_Map (ID: 70)
```
Handler: vdbe_op_map_inline()
Size: 284 chars
Registers: P1 (pair count), P3 (first register), P2 (output)
Pattern: Create msgpack map from register pairs
Dependencies: mem_encode_map(), mem_copy_map(), fiber_alloc()
Memory: Uses fiber GC region for temporary encoding
Note: Pairs are in P3, P3+1, P3+2, P3+3, ... (key, value alternating)
```

**Implementation Notes**:
- Similar to OP_Array but encodes key-value pairs
- P1 represents number of key-value pairs (not registers)
- Allocate fiber GC region for encoding
- Encode as msgpack map
- Copy to output register P2

#### OP_Getitem (ID: 71)
```
Handler: vdbe_op_getitem_inline()
Size: 216 chars
Registers: P1 (index/key), P2 (container), P3 (output)
Pattern: r[P3] = r[P2][r[P1]]  or  r[P3] = map[key_from_r[P1]]
Dependencies: mem_is_null(), diag_set()
Array Access: Bounds checking
Error Handling: Set diagnostic on invalid index
```

**Implementation Notes**:
- Check if P2 (container) is NULL
- Extract index/key from P1 register
- Access array or map element
- Store result in P3
- Proper error messages for out-of-bounds or invalid operations

---

### Group C: Cursor & Aggregation (2 opcodes, ~455 chars total)

#### OP_OpenPseudo (ID: 99)
```
Handler: vdbe_op_openpseudo_inline()
Size: 231 chars
Registers: P1 (not used in key), P2 (cursor number)
Pattern: Allocate pseudo-cursor for memory-resident data
Dependencies: allocateCursor()
Cursor Type: CURTYPE_PSEUDO
Memory Store: P3 register holds memory data
```

**Implementation Notes**:
- Get cursor number from P2
- Call allocateCursor() to allocate new cursor
- Set cursor type to CURTYPE_PSEUDO
- Link cursor to memory register specified by P3
- Return 0 for success, -1 on allocation failure

#### OP_Count (ID: 81)
```
Handler: vdbe_op_count_inline()
Size: 224 chars
Registers: P1 (cursor), P2 (output)
Pattern: Get record count from cursor
Dependencies: Tarantool cursor count function
SQL Usage: Implements COUNT(*) aggregate
```

**Implementation Notes**:
- Get cursor from P1
- Extract Tarantool cursor object from wrapper
- Call count function on cursor
- Store count result in P2 register
- Return 0 for normal completion

---

## File Structure

### Primary Implementation File: `vdbe_ops_inline_medium_7.c`

```c
/**
 * vdbe_ops_inline_medium_7.c
 * Phase 5.6h: 8 medium-complexity opcodes (Bitwise, Value Loading, Cursor ops)
 *
 * Opcodes (8):
 *   - OP_ShiftLeft (ID: 22)    - Bitwise left shift
 *   - OP_ShiftRight (ID: 23)   - Bitwise right shift
 *   - OP_String8 (ID: 48)      - Load C string constant
 *   - OP_Array (ID: 69)        - Create msgpack array
 *   - OP_Map (ID: 70)          - Create msgpack map
 *   - OP_Getitem (ID: 71)      - Get array/map element
 *   - OP_OpenPseudo (ID: 99)   - Allocate pseudo-cursor
 *   - OP_Count (ID: 81)        - Get cursor record count
 *
 * Total: ~2020 characters
 * Helper Functions Required: 0 (all use standard mem_* interface)
 * Pattern Adherence: 100% (follows Phase 5.6g patterns)
 */
```

### Integration Points

**File**: vdbe_dispatch_wrapper.c
```c
case OP_ShiftLeft: {
    int handler_rc = vdbe_op_shiftleft_inline(p, pOp, aMem);
    if (handler_rc < 0) { rc = -1; break; }
    pc++; continue;
}
case OP_ShiftRight: {
    int handler_rc = vdbe_op_shiftright_inline(p, pOp, aMem);
    if (handler_rc < 0) { rc = -1; break; }
    pc++; continue;
}
// ... (similar for 6 remaining opcodes)
```

**File**: vdbe_ops.h
```c
int vdbe_op_shiftleft_inline(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_shiftright_inline(Vdbe *p, pOp, Mem *aMem);
int vdbe_op_string8_inline(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_array_inline(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_map_inline(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_getitem_inline(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_openpseudo_inline(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_count_inline(Vdbe *p, Op *pOp, Mem *aMem);
```

**File**: CMakeLists.txt
```cmake
# Add to SQL source files list
set(sql_sources
    # ... existing files ...
    src/box/sql/vdbe_ops_inline_medium_7.c
)
```

---

## Dependencies Verification

### Standard Interfaces (Already Available)
- `mem_shift_left()` - Bitwise left shift
- `mem_shift_right()` - Bitwise right shift
- `mem_encode_array()` - Array encoding
- `mem_encode_map()` - Map encoding
- `mem_copy_array()` - Array copy
- `mem_copy_map()` - Map copy
- `mem_is_null()` - NULL check
- `sqlStrlen30()` - String length calculation
- `allocateCursor()` - Cursor allocation
- `diag_set()` - Diagnostic error setting

### Cursor Functions
- Tarantool cursor count function (wrapped in cursor abstraction)

### Memory Management
- `fiber_alloc()` - Fiber GC region allocation
- `fiber_gc()` - Fiber GC region access

---

## Build & Verification Process

### 1. Build Code Generation
```bash
cd /home/tsafin/tarantool/build
cmake --build . --target generate_sql_files
```

### 2. Syntax Validation (Python)
```python
import re
with open('src/box/sql/vdbe_ops_inline_medium_7.c', 'r') as f:
    content = f.read()
open_braces = content.count('{')
close_braces = content.count('}')
print(f"Braces: {open_braces} open, {close_braces} close")
assert open_braces == close_braces, "Brace mismatch!"
```

### 3. Git Verification
```bash
git status                 # Verify clean working tree after implementation
git diff src/box/sql/      # Review all changes
git log --oneline -5       # Verify commit history
```

---

## Expected Timeline & Effort

### Implementation Time Allocation
- Bitwise opcodes (2): 20 minutes
- Value loading opcodes (4): 50 minutes
- Cursor opcodes (2): 30 minutes
- Integration & testing: 30 minutes
- Documentation: 20 minutes

**Total estimate**: 150 minutes (well-contained)

---

## Risk Assessment

### Low Risk ✅
- All opcodes follow established patterns from phases 5.6f-g
- All dependencies already available in mem.h and cursor headers
- Build system proven with 6 previous medium batches
- No complex branching or error conditions beyond standard checks

### Potential Issues & Mitigations
1. **Fiber GC region usage** (OP_Array, OP_Map)
   - Mitigation: Reference existing array/map handling code
   - Pattern: Allocate → encode → copy → return

2. **Pseudo-cursor allocation** (OP_OpenPseudo)
   - Mitigation: Review existing cursor allocation in inline handlers
   - Verify allocateCursor() returns proper cursor type

3. **Cursor count operation** (OP_Count)
   - Mitigation: Verify Tarantool cursor interface for count functionality
   - Test with simple COUNT(*) queries

---

## Success Criteria

- ✅ All 8 opcodes compile without errors
- ✅ Build completes successfully with `cmake --build .`
- ✅ No new compilation warnings introduced
- ✅ Dispatcher wrapper includes all 8 cases
- ✅ vdbe_ops.h has all 8 function prototypes
- ✅ CMakeLists.txt updated correctly
- ✅ Code follows established pattern from phases 5.6f-g
- ✅ Git history clean with single comprehensive commit

---

## Next Phase Preview (5.6i)

After Phase 5.6h completion:
- Total coverage: 42 inline handlers / 63 = 67%
- Dispatcher coverage: 105 / 176 = 60%
- Remaining medium opcodes: ~10
- Complex opcodes: ~30 (Phase 5.7+)

---

## References

- Phase 5.6g Plan: PHASE_5_6g_PLAN.md (most recent reference)
- Phase 5.6g Session Summary: PHASE_5_6g_SESSION_SUMMARY.md
- Quick Reference: PHASE_5_6_QUICK_REFERENCE.md
- Strategy: PHASE_5_6_INLINE_CODE_STRATEGY.md
- Master Status: VDBE_REFACTORING_STATUS.md

---

**Status**: ✅ Plan Ready - Awaiting Implementation Start

**Next Action**: Begin implementation of vdbe_ops_inline_medium_7.c
