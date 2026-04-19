# P2-Based Branching Implementation Checklist

**Purpose**: Prevent incomplete if/else implementations in new VDBE opcode handlers
**Reference**: OP_Clear bug (commit 2fb0c16c34) - missing P2==0 branch caused DELETE to fail silently

---

## Pre-Implementation Review Checklist

### Step 1: Identify Branching Pattern
Before implementing a new opcode handler, determine how control flow is managed:

- [ ] **P2-Value Branching**: Handler uses P2 parameter value to determine path
  - Example: `if(pOp->p2 > 0)` or `if(pOp->p2 == X)`
  - **RISK LEVEL**: HIGH - Requires complete if/else blocks
  - Action: Proceed to Step 2

- [ ] **Return Code Branching**: Handler returns different codes to signal dispatcher
  - Example: Return 0 (continue), 1 (jump), -1 (error)
  - **RISK LEVEL**: LOW - Single code path
  - Action: Verify return codes are correct

- [ ] **Flag-Based Branching**: Handler checks P5 flags, not P2 value
  - Example: `if((pOp->p5 & FLAG) != 0)`
  - **RISK LEVEL**: MEDIUM - Verify both flag states handled
  - Action: Proceed to Step 3

- [ ] **Register-Based Branching**: Handler checks register contents, not P2
  - Example: `if(mem_is_null(pIn1))`
  - **RISK LEVEL**: LOW - Clear data flow
  - Action: Document in comments

---

## Step 2: P2-Value Branching Implementation

### If you selected "P2-Value Branching":

#### 2a. Document All P2 Values
```c
/* P2 values and their meaning:
 * P2 > 0:  [describe what happens]
 * P2 == 0: [describe what happens]
 * P2 < 0:  [describe what happens or mark as invalid]
 */
```

#### 2b. Implement All Branches
```c
if (pOp->p2 > 0) {
    // Handle P2 > 0 case
    // [implementation here]
} else if (pOp->p2 == 0) {
    // Handle P2 == 0 case
    // [implementation here]
} else {
    // Handle error case or document as invalid
    // [error handling]
}
```

#### 2c. Validate Every Branch
- [ ] Does P2 > 0 path have complete implementation?
- [ ] Does P2 == 0 path have complete implementation?
- [ ] Does P2 < 0 (if applicable) have proper handling?
- [ ] Is error handling present in each path?
- [ ] Are side effects (nChange, cacheStatus) updated in each path?

#### 2d. Test Every Branch
```lua
-- Test case 1: P2 > 0
box.execute("... statement that uses P2 > 0 path ...")
assert(expected_result_1)

-- Test case 2: P2 == 0
box.execute("... statement that uses P2 == 0 path ...")
assert(expected_result_2)

-- Test case 3: P2 < 0 (if applicable)
-- [error case testing]
```

---

## Step 3: Flag-Based Branching Implementation

### If you selected "Flag-Based Branching":

#### 3a. Document All Flags
```c
/* P5 flags and their meaning:
 * SQL_STOREP2:      [describe behavior when set]
 * SQL_OTHER_FLAG:   [describe behavior when set]
 * [no flags]:       [describe default behavior]
 */
```

#### 3b. Implement All States
```c
if ((pOp->p5 & SQL_STOREP2) != 0) {
    // Handle with SQL_STOREP2 flag set
    // [implementation here]
}
if ((pOp->p5 & SQL_OTHER_FLAG) != 0) {
    // Handle with SQL_OTHER_FLAG flag set
    // [implementation here]
}
// Handle default case (no flags)
```

#### 3c. Validate Flag Combinations
- [ ] Are all flag combinations tested?
- [ ] Are mutually exclusive flags handled properly?
- [ ] Is default (no flags) behavior correct?
- [ ] Are return codes consistent across all paths?

#### 3d. Test All Flag States
```lua
-- Test without flags
box.execute("... statement without flags ...")
assert(expected_result_1)

-- Test with SQL_STOREP2 flag
-- [requires VDBE bytecode inspection or testing through SQL compiler]

-- Test with multiple flags
-- [if applicable]
```

---

## Step 4: Code Review Checklist

### For ALL New Handlers:

- [ ] **Completeness**: All possible parameter values handled?
  - [ ] P2 > 0 case
  - [ ] P2 == 0 case
  - [ ] P2 < 0 case (if applicable)
  - [ ] Error conditions

- [ ] **Error Handling**: What happens on error?
  - [ ] diag_set() called with appropriate error?
  - [ ] Returns -1 on error?
  - [ ] No silent failures?

- [ ] **Side Effects**: Are all modifications tracked?
  - [ ] p->nChange updated (if OPFLAG_NCHANGE)?
  - [ ] Cursor cache invalidated (cacheStatus = CACHE_STALE)?
  - [ ] Register modified (if output register)?

- [ ] **Comments**: Is the behavior documented?
  - [ ] P2 semantics clearly stated?
  - [ ] Return value documented?
  - [ ] Preconditions listed?

- [ ] **Testing**: Is every branch tested?
  - [ ] P2 > 0 case tested?
  - [ ] P2 == 0 case tested?
  - [ ] Error conditions tested?
  - [ ] Edge cases tested?

### For P2-Value Branching Specifically:

- [ ] **Missing Else**: Is there an else block for EVERY if?
  - [ ] `if(P2>0) { ... } else { ... }` ✓
  - [ ] `if(condition) { ... } else { ... }` ✓
  - [ ] No naked if without else ✗

- [ ] **Silent Failures**: Can operation fail silently?
  - [ ] Is result checked? `if(function() != 0)`
  - [ ] Is error reported? `diag_set(...)`
  - [ ] Is error returned? `return -1`

---

## Reference Pattern: OP_Clear (Correct Implementation)

```c
int
vdbe_op_clear_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
    uint32_t space_id;
    struct space *space;

    (void)aMem;

    assert(pOp->p1 > 0);
    space_id = pOp->p1;
    space = space_by_id(space_id);
    assert(space != NULL);

    // ✓ COMPLETE IF/ELSE STRUCTURE
    if (pOp->p2 > 0) {
        // ✓ FULL IMPLEMENTATION: Fast truncate path
        if (box_truncate(space_id) != 0) {
            return -1;  // ✓ ERROR HANDLING
        }
    } else {
        // ✓ ELSE BLOCK: Normal clear path for P2==0
        uint32_t tuple_count;
        if (tarantoolsqlClearTable(space, &tuple_count) != 0) {
            return -1;  // ✓ ERROR HANDLING
        }
        // ✓ SIDE EFFECTS: Track changes
        if ((pOp->p5 & OPFLAG_NCHANGE) != 0) {
            p->nChange += tuple_count;
        }
    }

    return 0;  // ✓ SUCCESS PATH
}
```

**What makes this correct**:
1. Both P2 > 0 and P2 == 0 branches implemented ✓
2. Error handling in both paths ✓
3. Side effects tracked (nChange) ✓
4. Comments document semantics ✓
5. Each branch is complete, not stubbed ✓

---

## Anti-Pattern: OP_Clear Before Fix (Incomplete)

```c
// ❌ DANGEROUS - INCOMPLETE IMPLEMENTATION
if (pOp->p2 > 0) {
    /* Fast truncate path (TRUNCATE TABLE) */
    if (box_truncate(space_id) != 0) {
        return -1;
    }
}
// ❌ NO ELSE BLOCK - P2==0 case missing!
// ❌ SILENT FAILURE - Operation silently fails

return 0;  // Returns success even though P2==0 did nothing!
```

**What's wrong**:
1. Missing else block for P2 == 0 ✗
2. No implementation for normal DELETE case ✗
3. Silent failure - no error reported ✗
4. Result: DELETE statements don't work ✗

---

## Testing Template for P2-Based Handlers

### Test Structure
```lua
#!/usr/bin/env tarantool

-- Setup
box.cfg{}

-- Create test table
box.execute([[
    CREATE TABLE test_table (
        id INT PRIMARY KEY,
        value TEXT
    )
]])

-- Test Case 1: P2 > 0 path
print("Test 1: P2 > 0 case")
box.execute("INSERT INTO test_table VALUES (1, 'a'), (2, 'b')")
-- [Execute operation with P2 > 0]
local result = box.execute("SELECT COUNT(*) FROM test_table")
assert(result.rows[1][1] == expected_count_1, "Test 1 failed")
print("✓ Test 1 passed")

-- Test Case 2: P2 == 0 path
print("Test 2: P2 == 0 case")
box.execute("DELETE FROM test_table")
box.execute("INSERT INTO test_table VALUES (3, 'c'), (4, 'd')")
-- [Execute operation with P2 == 0]
local result = box.execute("SELECT COUNT(*) FROM test_table")
assert(result.rows[1][1] == expected_count_2, "Test 2 failed")
print("✓ Test 2 passed")

-- Cleanup
box.execute("DROP TABLE test_table")

print("\nAll tests passed!")
os.exit(0)
```

---

## Red Flags During Code Review

### If you see these patterns, investigate further:

1. **Naked if without else**
   ```c
   if (pOp->p2 > 0) {
       // ...
   }
   return 0;  // ← What if P2 <= 0?
   ```
   **ACTION**: Add else block or document why it's safe

2. **Same operation in both branches** (probably shouldn't branch)
   ```c
   if (pOp->p2 > 0) {
       box_operation();
   } else {
       box_operation();  // Same code - why branch?
   }
   ```
   **ACTION**: Consolidate or clarify the difference

3. **Error handling missing from one branch**
   ```c
   if (pOp->p2 > 0) {
       if (function() != 0) return -1;  // ✓
   } else {
       function();  // ✗ No error check!
   }
   ```
   **ACTION**: Add error handling to all branches

4. **Return codes differ inconsistently**
   ```c
   if (pOp->p2 > 0) {
       return (condition) ? 1 : 0;  // Sometimes jump
   } else {
       return 0;  // Always continue
   }
   ```
   **ACTION**: Document why returns differ, verify intentional

5. **Side effects in only one branch**
   ```c
   if (pOp->p2 > 0) {
       result = do_work();
       p->nChange += result;  // ✓
   } else {
       do_work();  // ✗ No nChange tracking!
   }
   ```
   **ACTION**: Apply side effects consistently or document why different

---

## Common P2 Semantics Patterns

### Pattern 1: Operation Mode (TRUNCATE vs DELETE)
```c
if (pOp->p2 > 0) {
    // TRUNCATE: Fast path (drop all rows)
} else {
    // DELETE: Normal path (track changes, trigger events)
}
```
**Examples**: OP_Clear, OP_OpenSpace
**Usage Frequency**: Medium

### Pattern 2: Optimization Flag
```c
if (pOp->p2 > 0) {
    // Optimized path (cached value, shortcut)
} else {
    // Full path (compute value, no optimization)
}
```
**Examples**: OP_Sequence, OP_Param
**Usage Frequency**: Low

### Pattern 3: Alternate Operation
```c
if (pOp->p2 > 0) {
    // Operation A
} else {
    // Operation B
}
```
**Examples**: Potential for future ops
**Usage Frequency**: Low

---

## Sign-Off Checklist

Before committing handler code:

- [ ] Both if and else blocks implemented (for P2-value branching)
- [ ] Error handling present in all paths
- [ ] All tests passing (P2>0 and P2==0 cases)
- [ ] Code reviewed for completeness
- [ ] Comments document P2 semantics
- [ ] No silent failures possible
- [ ] Side effects consistent across branches
- [ ] Edge cases tested (P2=0, P2=1, P2=max_int, etc.)

---

## References

- **OP_Clear Bug**: Commit 2fb0c16c34 - Shows what happens with incomplete if/else
- **OP_Clear Fix**: Commit 2fb0c16c34 - Shows correct pattern
- **MEMORY.md**: Contains detailed history of dispatcher development
- **VDBE Dispatcher Architecture**: Understand when P2 means jump vs condition value
