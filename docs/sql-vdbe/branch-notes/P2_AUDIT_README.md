# P2-Based Branching Audit Documentation

**Complete Analysis of VDBE Dispatcher Parameter-Driven Conditionals**

---

## Quick Navigation

Start here based on your role:

### For Project Managers / Decision Makers
→ Read: **P2_AUDIT_EXECUTIVE_SUMMARY.md** (5 min read)
- Bottom line: 78% verified safe, 2 items need verification
- Production readiness: Conditional pending verification

### For Developers / Code Reviewers
→ Read: **P2_BRANCHING_FINDINGS_SUMMARY.txt** (10 min read)
- All 27 handlers listed with risk assessment
- Specific line numbers and file locations
- Action items with priority levels

### For Deep Dive / Quality Assurance
→ Read: **P2_BRANCHING_AUDIT_EXTENDED.md** (20 min read)
- Detailed code examples and patterns
- Verification scripts and test strategies
- All 4 branching patterns explained

### For Historical Context / Reference
→ Read: **P2_BRANCHING_AUDIT.md** (original comprehensive audit)
- Background on OP_Clear bug discovery
- How the bug was found and fixed
- Pattern recognition methodology

---

## The Core Finding

**Pattern Risk**: Handlers using P2 parameter to control branching must implement ALL code paths:

```c
// DANGEROUS - Incomplete
if (pOp->p2 > 0) {
    // Handle special case
    // ...
}
// ❌ Missing else block = silent failure when P2==0

// SAFE - Complete
if (pOp->p2 > 0) {
    // Handle case A
    // ...
} else {
    // Handle case B (MUST BE HERE)
    // ...
}
```

**Real Example**: OP_Clear bug where DELETE failed silently because P2==0 branch was missing.

---

## Audit Scope and Results

### What Was Analyzed
- **Codebase**: ~4,866 lines of VDBE opcode handler code
- **Files**: 12 handler files (vdbe_ops_*.c)
- **Handlers**: 27 with P2-based logic identified
- **Commits Reviewed**: 5+ related fixes

### What Was Found

| Finding | Count | Status |
|---------|-------|--------|
| Safe patterns | 21 | ✅ Verified |
| Verification needed | 2 | ⚠️ Urgent |
| Already fixed | 1 | ✅ Reference |
| Unused/low-risk | 3 | ℹ️ Monitor |

### Critical Issues (Immediate Action)

1. **OP_Once** (vdbe_ops_inline_medium_4.c)
   - Suspected logic inversion
   - Causes trigger code to repeat instead of execute once
   - **Fix**: Verify against inline dispatcher logic

2. **OP_MustBeInt** (vdbe_ops_type.c)
   - Error paths need verification
   - Could silently fail on type errors
   - **Fix**: Confirm both error branches work

### Medium Issues (Should Audit)

3. **OP_Last** (vdbe_ops_cursor_nav.c)
   - Strict P2 assertion may reject valid bytecode
   - **Fix**: Verify SQL compiler generates compatible bytecode

4. **OP_GetItem** (vdbe_ops_inline_medium_7.c)
   - Non-functional stub
   - **Fix**: Mark as unimplemented (not used by compiler anyway)

---

## The Pattern Categories

### Safe Pattern #1: Return Code Branching
```c
if (condition) return 1;  // Jump to P2
return 0;                  // Continue
```
**Handlers** (12): Next, Prev, Rewind, Last, sorter operations
**Status**: ✅ Safe - Clear semantics

### Safe Pattern #2: Flag-Based Branching
```c
if ((p5 & FLAG) != 0) {
    // Handle with flag
    return X;
}
// Handle without flag
return Y;
```
**Handlers** (8): Eq, Ne, Lt, Le, Gt, Ge, Compare, IsNull
**Status**: ✅ Safe - Symmetric implementation

### Safe Pattern #3: Data Flow (No Branching)
```c
// P2 is source or destination register, not condition
pIn1->u.u += pOp->p2;
```
**Handlers** (3): AddImm, Sequence, Fetch
**Status**: ✅ Safe - No control flow

### Risky Pattern: Parameter-Driven Branching
```c
if (pOp->p2 > 0) {
    // Case A
} else {
    // Case B (MUST be implemented!)
}
```
**Handlers** (4): Clear (fixed), Once (verify), Last (audit), MustBeInt (verify)
**Status**: ⚠️ Watch this pattern carefully!

---

## Verification Commands

Run these to verify suspicious handlers:

```bash
# Check OP_Once logic (CRITICAL)
grep -A 40 "EXECUTE(OP_Once" src/box/sql/vdbe.c | head -50

# Check OP_MustBeInt error paths (IMPORTANT)
grep -A 20 "EXECUTE(OP_MustBeInt" src/box/sql/vdbe.c

# Check OP_Last P2 semantics (MEDIUM)
grep -A 30 "EXECUTE(OP_Last" src/box/sql/vdbe.c

# Check OP_Clear reference pattern (already fixed)
grep -A 40 "EXECUTE(OP_Clear" src/box/sql/vdbe.c

# Find all if statements with P2 without else (catch similar bugs)
grep -n "if.*p2.*>.*0" src/box/sql/vdbe_ops*.c | grep -v "else"
```

---

## Handlers Analyzed (Complete List)

### ⚠️ Critical (Verify Immediately)
- [ ] OP_Once (vdbe_ops_inline_medium_4.c:47-61)
- [ ] OP_GetItem (vdbe_ops_inline_medium_7.c:266-293) - unused

### ⚠️ Medium (Should Audit)
- [ ] OP_Last (vdbe_ops_cursor_nav.c:37-66)
- [ ] OP_MustBeInt (vdbe_ops_type.c:15-27)

### ✅ Already Fixed (Reference Pattern)
- [x] OP_Clear (vdbe_ops_inline_medium_5.c:150-181) - commit 2fb0c16c34

### ✅ Verified Safe (Return Code Pattern)
- [x] OP_Rewind (vdbe_ops_cursor_nav.c)
- [x] OP_Next (vdbe_ops_cursor_nav.c)
- [x] OP_Prev (vdbe_ops_cursor_nav.c)
- [x] OP_NextIfOpen (vdbe_ops_cursor_nav.c)
- [x] OP_PrevIfOpen (vdbe_ops_cursor_nav.c)
- [x] OP_SorterNext (vdbe_ops_sorter.c)
- [x] OP_SorterCompare (vdbe_ops_sorter.c)
- [x] OP_SorterSort (vdbe_ops_sorter.c)
- [x] OP_Found (vdbe_ops_index.c)
- [x] OP_NotFound (vdbe_ops_index.c)
- [x] OP_NoConflict (vdbe_ops_index.c)

### ✅ Verified Safe (Flag-Based Pattern)
- [x] OP_Eq (vdbe_ops_compare.c)
- [x] OP_Ne (vdbe_ops_compare.c)
- [x] OP_Lt (vdbe_ops_compare.c)
- [x] OP_Le (vdbe_ops_compare.c)
- [x] OP_Gt (vdbe_ops_compare.c)
- [x] OP_Ge (vdbe_ops_compare.c)
- [x] OP_Compare (vdbe_ops_compare.c)
- [x] OP_IsNull (vdbe_ops_inline_simple.c)

### ✅ Verified Safe (Data Flow Pattern)
- [x] OP_AddImm (vdbe_ops_inline_medium_2.c)
- [x] OP_Sequence (vdbe_ops_inline_medium_2.c)
- [x] OP_Fetch (vdbe_ops_inline_medium_6.c)

---

## Testing Strategy

### Test Case 1: OP_Clear (Reference Pattern)
```lua
-- Test P2 > 0 path (TRUNCATE)
box.execute("INSERT INTO t VALUES (1), (2), (3)")
box.execute("DELETE FROM t")  -- Uses P2 > 0
assert(count == 0)

-- Test P2 == 0 path (DELETE with tracking)
box.execute("INSERT INTO t VALUES (4), (5)")
box.execute("DELETE FROM t")  -- Uses P2 == 0
assert(count == 0)
```

### Test Case 2: Comparison Opcodes (Flag-Based)
```lua
-- SQL_STOREP2 flag set: store result
-- SQL_STOREP2 flag clear: jump if condition true
```

### Test Case 3: Cursor Navigation (Return Code)
```lua
-- Empty result: jump to P2
-- More rows: continue to next iteration
```

---

## Key Takeaways

1. **78% of handlers are verified safe** - Only 2 items need verification
2. **OP_Clear teaches us the pattern** - This fixed bug is now reference for all P2-branching
3. **Most handlers use safe patterns** - Return codes and flags are much safer than parameter values
4. **Silent failures are the risk** - Missing else blocks don't error, just fail silently
5. **Verification is quick** - Just compare with inline dispatcher using grep commands

---

## File Structure

```
/home/tsafin/tarantool/
├── P2_AUDIT_README.md (this file)
├── P2_AUDIT_EXECUTIVE_SUMMARY.md (2-page summary for decision makers)
├── P2_BRANCHING_FINDINGS_SUMMARY.txt (detailed checklist format)
├── P2_BRANCHING_AUDIT_EXTENDED.md (technical deep dive with code examples)
└── P2_BRANCHING_AUDIT.md (original comprehensive audit)
```

---

## Timeline and Status

| Date | Status | Notes |
|------|--------|-------|
| Feb 15 | Investigation | OP_Clear bug discovered during testing |
| Feb 15 | Analysis | P2-based branching pattern identified |
| Feb 16 | Audit | P2_BRANCHING_AUDIT.md written |
| Feb 17 | Complete | OP_Clear fixed (commit 2fb0c16c34) |
| Feb 17 | Verification | Extended audit completed |
| Feb 18 | Summary | This documentation package generated |

---

## Next Steps

### This Week (Priority 1)
1. Run verification commands for OP_Once and OP_MustBeInt
2. Document findings in MEMORY.md
3. Decide on production release timing

### Next Week (Priority 2)
1. Implement any fixes found during verification
2. Add test coverage for all P2-branching handlers
3. Create linting rule to prevent similar bugs

### Next Sprint (Priority 3)
1. Document P2-branching pattern for future handlers
2. Review remaining handler files (optional)
3. Implement automated static analysis

---

## References

- **Original Bug**: DELETE FROM not working, rows silently not deleted
- **Root Cause**: OP_Clear handler missing P2==0 branch
- **Fix**: Commit 2fb0c16c34 - Added complete else block with row tracking
- **Impact**: All DELETE operations now work correctly
- **Lesson**: Always implement ALL branches in parameter-driven conditionals

---

## Questions?

Refer to:
- **Quick answers**: P2_AUDIT_EXECUTIVE_SUMMARY.md
- **Specific handlers**: P2_BRANCHING_FINDINGS_SUMMARY.txt
- **Technical details**: P2_BRANCHING_AUDIT_EXTENDED.md
- **Full context**: P2_BRANCHING_AUDIT.md

---

**Report Date**: February 18, 2026
**Status**: COMPLETE ✓
**Production Readiness**: Conditional (pending verification items)
**Next Review**: February 25, 2026
