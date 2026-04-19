# VDBE Handler Implementation Guide

## Quick Start: Adding a New Inline Opcode Handler

This guide shows how to implement a new inline opcode handler following the patterns established in Phase 5.6c.

## Step 1: Identify Your Opcode

### In opcodes.yaml
```yaml
- name: OP_YourOpcode
  id: 123
  handler_type: inline
  flags:
    - IN1
    - OUT2
  doc: Your opcode description
  inline_code: "your inline code here"
```

### In vdbe.c (Original Implementation)
Find the original `case OP_YourOpcode:` block to understand the logic.

## Step 2: Understand the Handler Pattern

### Function Signature
```c
int
vdbe_op_youropcode_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
    // Implementation here
    return 0;  // Continue to next instruction
}
```

### Return Values
- `0`: Continue to next instruction (most common)
- `1`: Jump to P2 (for comparison/control flow opcodes)
- `-1`: Error occurred
- Special cases: Some opcodes return specific values (e.g., SQL_ROW)

### Parameters
- `Vdbe *p`: The VDBE instance (machine state)
- `Op *pOp`: The operation being executed (contains P1, P2, P3, P4, etc.)
- `Mem *aMem`: The register array for operands/results

## Step 3: Helper Functions

### Available Helpers in vdbe_helpers.h
```c
void sqlVdbeMemAboutToChange(Vdbe *pVdbe, Mem *pMem);
struct Mem * vdbe_prepare_null_out(struct Vdbe *v, int n);
```

### Common Memory Operations
```c
// From mem.h
void mem_set_null(Mem *pMem);
void mem_set_int(Mem *pMem, int64_t val);
void mem_set_uint(Mem *pMem, uint64_t val);
void mem_set_dec(Mem *pMem, decimal_t *dec);
void mem_set_ptr(Mem *pMem, void *ptr);
void mem_set_str(Mem *pMem, char *str, int n);

// Query functions
bool mem_is_null(Mem *pMem);
bool mem_is_uint(Mem *pMem);
bool mem_is_int(Mem *pMem);
```

### If You Need New Helpers
1. Identify the helper function in vdbe.c
2. Extract to vdbe_helpers.h following Phase 5.6c pattern
3. Remove `static` keyword from definition in vdbe.c
4. Add declaration to vdbe_helpers.h
5. Update vdbeInt.h include if new file

## Step 4: Implementation Example

### Simple Opcode (OP_Decimal)
```c
int
vdbe_op_decimal_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p;  // Not used

    Mem *pOut = &aMem[pOp->p2];
    mem_set_null(pOut);
    mem_set_dec(pOut, pOp->p4.dec);

    return 0;  // Continue
}
```

### Medium Opcode (OP_AddImm)
```c
int
vdbe_op_addimm_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
    Mem *pIn1 = &aMem[pOp->p1];

    // Mark register as changed (for SCopy tracking)
    sqlVdbeMemAboutToChange(p, pIn1);

    // Assertions validate preconditions
    assert(mem_is_uint(pIn1) && pOp->p2 >= 0);

    // Perform operation
    pIn1->u.u += pOp->p2;

    return 0;  // Continue
}
```

### Opcode with Jump (OP_IsNull)
```c
int
vdbe_op_isnull_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p;  // Not used

    Mem *pIn1 = &aMem[pOp->p1];

    if (mem_is_null(pIn1)) {
        return 1;  // Jump to P2
    }

    return 0;  // Continue (fall through)
}
```

## Step 5: Register File

### Create Source File
Create `src/box/sql/vdbe_ops_inline_medium_X.c` with:
- File header comment
- Include statements (sqlInt.h, mem.h, vdbeInt.h)
- Handler function(s)
- Detailed opcode documentation

### Add Prototypes
In `src/box/sql/vdbe_ops.h`:
```c
/* Medium complexity inline opcode handlers - Phase 5.6X */
int vdbe_op_youropcode_inline(Vdbe *p, Op *pOp, Mem *aMem);
```

### Update Dispatcher
In `src/box/sql/vdbe_dispatch_wrapper.c`:
```c
case OP_YourOpcode: {
    /* Brief description */
    int handler_rc = vdbe_op_youropcode_inline(p, pOp, aMem);
    if (handler_rc < 0) { rc = -1; break; }  // Error handling
    if (handler_rc == 1) { pc = P2 - 1; continue; }  // Jump handling
    pc++; continue;  // Default: continue
}
```

### Update Build System
In `src/box/CMakeLists.txt`, add to sql_sources:
```
sql/vdbe_ops_inline_medium_X.c
```

## Step 6: Verify

### Syntax Check
```bash
cd build
cmake --build . --target generate_sql_files
```

### Check Generated Files
```bash
grep "OP_YourOpcode" build/src/box/sql/opcodes.h
```

### Build Attempt
```bash
cmake --build . --target box 2>&1 | grep -i "error\|warning"
```

## Common Patterns

### Output Register Pattern
```c
Mem *pOut = &aMem[pOp->p2];
mem_set_null(pOut);
mem_set_int(pOut, value);  // or appropriate type
```

### Input Register Pattern
```c
Mem *pIn1 = &aMem[pOp->p1];
if (!mem_is_int(pIn1)) {
    // Type error handling
}
value = pIn1->u.i;
```

### Cursor Access Pattern
```c
assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
assert(p->apCsr[pOp->p1] != NULL);
struct VdbeCursor *pC = p->apCsr[pOp->p1];
```

### Error Handling Pattern
```c
if (some_operation() != 0) {
    return -1;  // Signal error
}
```

### Jump/Control Pattern
```c
if (condition) {
    return 1;  // Jump to P2
}
return 0;  // Continue
```

## File Structure Template

```c
/*
 * VDBE Inline Opcode Handlers - [Category] Batch [N]
 * Phase 5.6X: [Description]
 *
 * This file contains wrapper functions for [complexity] inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file ([N] opcodes):
 * - OP_Name1: Description
 * - OP_Name2: Description
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
// Additional includes if needed (e.g., #include "box/space.h")

/*
 * Opcode: OPNAME - Brief description
 *
 * Detailed description of what this opcode does.
 * May include pseudocode or operation description.
 *
 * Flags: IN1, OUT2 (or whatever applies)
 */
int
vdbe_op_name_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
    // Implementation
    return 0;
}
```

## Documentation Standards

### Opcode Header Format
```c
/*
 * Opcode: NAME - One-line description
 *
 * Multi-line detailed description explaining:
 * - What the opcode does
 * - Parameter meanings (P1, P2, P3, P4)
 * - Any preconditions or assertions
 * - Return behavior if special
 *
 * Flags: IN1, OUT2 (register flags from opcodes.yaml)
 */
```

### Comments in Code
- Document non-obvious logic
- Explain register selections
- Note dependencies on helpers
- Mark assertions that validate preconditions

## Testing Hints

### Parallel Validation
Once implemented, test with:
```bash
export VDBE_DISPATCHER=parallel
# Run test suite or specific SQL tests
```

This compares results between old and generated dispatchers.

### Manual Testing
```sql
-- Create test that exercises your opcode
SELECT your_expression;
```

## Common Issues and Solutions

### Issue: Function not found
**Solution**: Ensure prototype added to vdbe_ops.h and source file compiled

### Issue: Register access out of bounds
**Solution**: Verify P1/P2 are within valid range with assertions

### Issue: Uninitialized register
**Solution**: Use `mem_set_null()` before setting actual value (for proper initialization)

### Issue: Control flow not working
**Solution**: Return 1 for jumps, 0 for continue; dispatcher handles pc adjustment

## References

- [Phase 5.6c Session Summary](docs/sql-vdbe/branch-notes/PHASE_5_6c_SESSION_SUMMARY.md) - Completed phase details
- [Phase 5.6d Plan](docs/sql-vdbe/branch-notes/PHASE_5_6d_PLAN.md) - Next phase targets
- [vdbe_ops_inline_medium_2.c](src/box/sql/vdbe_ops_inline_medium_2.c) - Reference implementations
- [vdbeInt.h](src/box/sql/vdbeInt.h) - VDBE structures and types
- [mem.h](src/box/sql/mem.h) - Memory register operations

## Getting Help

1. Check existing handlers for similar opcodes
2. Review Phase 5.6c session summary for pattern documentation
3. Examine original vdbe.c implementation
4. Reference mem.h for available memory operations
5. Check opcodes.yaml for opcode metadata

---

**Last Updated**: 2025-12-20
**Pattern Version**: 5.6c
**Stability**: Proven (12 opcodes using this pattern)
