# Phase 5.6c - Helper Function Extraction & Medium Opcode Batch 2

## Objective
Extract static helper functions from vdbe.c and external API dependencies to unblock implementation of the remaining 31 medium-complexity inline opcodes (100-300 chars).

## Current Status (End of Phase 5.6b)

### What Works
- ✓ 8 inline opcode handlers integrated (6 from 5.6a + 2 from 5.6b)
- ✓ 55+ external handlers extracted and working
- ✓ Generated dispatcher framework in place
- ✓ Build system compiles cleanly

### What's Blocked
Phase 5.6b analysis identified 6 opcodes that require helper function extraction:

1. **OP_AddImm** (160 chars) - Add immediate value to register
   - Requires: `memAboutToChange(Vdbe *p, Mem *mem)` - static helper in vdbe.c:869
   - Purpose: Marks register as having changed for trace logging
   - Dependency: Internal VDBE state management

2. **OP_OpenSpace** (182 chars) - Open table space cursor
   - Requires: `space_by_id(uint32_t space_id)` from box/space.h
   - Purpose: Lookup space object by ID
   - Dependency: box subsystem integration (need #include "box/space.h")

3. **OP_Sequence** (195 chars) - Get next sequence value
   - Requires: `vdbe_prepare_null_out()` - static helper in vdbe.c:2156
   - Purpose: Initialize register for NULL output
   - Dependency: Shared with OP_Decimal and others

4. **OP_ShowCreateTable** (240 chars) - Show CREATE TABLE statement
   - Requires: `sql_show_create_table()` external function
   - Purpose: Generate SQL for CREATE TABLE
   - Dependency: SQL schema functions

5. **OP_Decimal** (114 chars) - Decimal constant
   - Requires: `vdbe_prepare_null_out()` - same as OP_Sequence
   - Purpose: Initialize register for NULL output
   - Dependency: Shared with OP_Sequence

### Transaction Opcodes (Deferred)
- OP_TransactionBegin, OP_TransactionCommit, OP_TransactionRollback
- Issue: Complex goto-based implementation + type signature mismatch (Parse * vs Vdbe *)
- Decision: Redesign needed - marked for later phase

## Implementation Plan

### Step 1: Extract `memAboutToChange()` Helper

**File**: `src/box/sql/vdbe_helpers.h` (NEW)

**Current location in vdbe.c**:
```c
static void
memAboutToChange(Vdbe *p, Mem *mem)
{
  mem->szMalloc = -mem->szMalloc;
  mem->zMalloc = 0;
  p->aMem[0].flags |= MEM_VmReadOnly;
}
```

**Action**:
1. Copy function to new vdbe_helpers.h as static inline
2. Add comprehensive opcode documentation
3. Update includes in vdbe.c and handler files
4. Verify no new dependencies introduced

**Files affected**:
- Create: `src/box/sql/vdbe_helpers.h`
- Modify: `src/box/CMakeLists.txt` (add to headers)
- Modify: `src/box/sql/vdbeInt.h` (add #include vdbe_helpers.h)

**Opcodes unblocked**: OP_AddImm + future register modification opcodes

---

### Step 2: Extract `vdbe_prepare_null_out()` Helper

**File**: `src/box/sql/vdbe_helpers.h` (extend)

**Current location in vdbe.c**:
```c
static void
vdbe_prepare_null_out(Mem *pOut)
{
  mem_set_null(pOut);
  pOut->flags |= MEM_Cleared;
}
```

**Action**:
1. Add to vdbe_helpers.h as static inline function
2. Document purpose: Pre-initialize output register as cleared NULL
3. No additional dependencies

**Files affected**:
- Modify: `src/box/sql/vdbe_helpers.h` (add function)
- Update includes in vdbe.c if needed

**Opcodes unblocked**: OP_Sequence, OP_Decimal

---

### Step 3: Resolve `space_by_id()` Integration

**File**: `src/box/sql/vdbe_ops_inline_medium_2.c` (NEW)

**Issue**: OP_OpenSpace needs `space_by_id()` from box/space.h
- This is an external API, not a helper to extract
- Need to determine if safe to #include box/space.h in vdbe ops handlers

**Action**:
1. Check if vdbe.c already includes box/space.h
2. If yes: Safe to include in handlers
3. If no: Investigate why it was avoided (potential circular dependency)
4. Create wrapper if needed to avoid circular dependencies

**Investigation checklist**:
- [ ] grep for "space_by_id" in src/box/sql/vdbe.c
- [ ] grep for "#include.*box/space" in src/box/sql/*.c
- [ ] Check include graph: sql → space dependency direction

**Decision needed**: User input on safe inclusion pattern

---

### Step 4: Implement Medium Opcode Batch 2

**File**: `src/box/sql/vdbe_ops_inline_medium_2.c` (NEW, ~200-300 lines)

**Opcodes to implement** (in order of simplicity):

1. **OP_Decimal** (114 chars) - Decimal constant
   ```
   Opcode: Decimal P1 P2 P3

   Write a decimal number consisting of P1 or P2 digits, with the actual digits
   of the number specified by P3.
   ```
   - Uses: `vdbe_prepare_null_out()` ✓ (from Step 2)
   - No complex control flow

2. **OP_AddImm** (160 chars) - Add immediate value to register
   ```
   Opcode: AddImm P1 P2 P3

   Add the constant P2 to the value in register P1. Store the result in P3.
   ```
   - Uses: `memAboutToChange()` ✓ (from Step 1)
   - Arithmetic operation on memory register

3. **OP_Sequence** (195 chars) - Get next sequence value
   ```
   Opcode: Sequence P1 P2

   Find the next unused sequence number for the sequence generator
   associated with cursor P1 and write the integer to register P2.
   ```
   - Uses: `vdbe_prepare_null_out()` ✓ (from Step 2)
   - Straightforward register assignment

4. **OP_OpenSpace** (182 chars) - Open table space cursor
   ```
   Opcode: OpenSpace P1 P2 P3

   Open a new cursor for a table. P1 is the cursor number. P2 is the
   table space ID. P3 is the write flag (used for table vs index cursor).
   ```
   - Uses: `space_by_id()` - depends on Step 3
   - Cursor initialization

**Build order**:
- Implement in vdbe_ops_inline_medium_2.c (all 4 opcodes)
- Update vdbe_ops.h with 4 new prototypes
- Update vdbe_dispatch_wrapper.c with 4 new cases
- Update CMakeLists.txt to include new file
- Test build and verify no compilation errors

---

### Step 5: Address `sql_show_create_table()`

**For later consideration** (Phase 5.6d):
- OP_ShowCreateTable requires `sql_show_create_table()`
- This is a more complex SQL schema function
- Defer to Phase 5.6d after verifying other opcodes work correctly

---

## Implementation Checklist

### Phase 5.6c - Step 1: Extract memAboutToChange
- [ ] Create src/box/sql/vdbe_helpers.h with static inline functions
- [ ] Copy memAboutToChange() to vdbe_helpers.h
- [ ] Add opcode documentation
- [ ] Update vdbeInt.h to #include vdbe_helpers.h
- [ ] Update CMakeLists.txt (add to headers list)
- [ ] Verify vdbe.c still builds without modification
- [ ] Build and test successful compilation

### Phase 5.6c - Step 2: Extract vdbe_prepare_null_out
- [ ] Add vdbe_prepare_null_out() to vdbe_helpers.h
- [ ] Document purpose and usage
- [ ] Verify no new dependencies
- [ ] Build and test successful compilation

### Phase 5.6c - Step 3: Resolve space_by_id Integration
- [ ] Investigate space_by_id usage in vdbe.c
- [ ] Check include dependencies
- [ ] Make decision on safe inclusion pattern
- [ ] Document decision in code comments

### Phase 5.6c - Step 4: Implement Medium Batch 2
- [ ] Create vdbe_ops_inline_medium_2.c with OP_Decimal handler
- [ ] Add OP_AddImm handler
- [ ] Add OP_Sequence handler
- [ ] Add OP_OpenSpace handler (if space_by_id decision made)
- [ ] Update vdbe_ops.h with 4 prototypes
- [ ] Update vdbe_dispatch_wrapper.c with 4 cases
- [ ] Update CMakeLists.txt
- [ ] Build and test all 4 opcodes compile
- [ ] Total inline handlers: 12 (8 from 5.6a-b + 4 from 5.6c)

### Phase 5.6c - Step 5: Session Documentation
- [ ] Update TODO.md with Phase 5.6c completion
- [ ] Create PHASE_5_6c_SESSION_SUMMARY.md with results
- [ ] Commit with detailed message
- [ ] Update overall progress metrics

## Expected Outcomes

### Build Status
- ✓ Zero compilation errors
- ✓ All 12 inline handlers callable
- ✓ vdbe_helpers.h available for future opcodes

### Coverage Improvement
- Current: 8 of 63 inline opcodes (12.7%)
- After Phase 5.6c: 12 of 63 inline opcodes (19%)
- Total dispatcher coverage: ~75 of 176 opcodes (42.6%)

### Dependencies Established
- Helper extraction pattern documented
- space_by_id integration decision made
- Foundation for remaining 27 medium opcodes (Phase 5.6d-f)

## Risk Assessment

### Low Risk
- memAboutToChange() and vdbe_prepare_null_out() are simple static inlines
- No new external dependencies introduced
- Can be reverted if issues arise

### Medium Risk
- space_by_id integration needs careful investigation
- May need wrapper if circular dependencies exist
- Requires user decision on safe include pattern

### High Risk
- None identified at this stage

## Next Phases

### Phase 5.6d: Medium Handlers Batch 3
- Implement remaining ~27 medium-complexity opcodes
- Use lessons from Phase 5.6c for similar dependencies

### Phase 5.6e: Complex Handlers
- Address 14 complex opcodes (>300 chars)
- Handle OP_Program, OP_RenameTable (1000+ chars)
- May require significant refactoring

### Phase 5.7: Full Test Suite
- Run complete test suite with generated dispatcher
- Parallel validation mode: VDBE_DISPATCHER=parallel
- Verify 100% match rate with original dispatcher

## References
- PHASE_5_6b_SESSION_SUMMARY.md - Previous phase results
- PHASE_5_6_INLINE_CODE_STRATEGY.md - Overall inline handler strategy
- TODO.md - Master project status
- src/box/sql/vdbe.c - Source of helper functions (lines 869, 2156)
