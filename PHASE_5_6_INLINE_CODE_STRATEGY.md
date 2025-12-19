# Phase 5.6 - Inline Code Integration Strategy

## Overview
Phase 5.6 aims to expand the generated dispatcher from handling ~10 opcodes to all 142 opcodes. This includes 63 inline opcodes extracted from vdbe.c that have complex control flow.

## Challenge Identified

### Problem
The inline code in `opcodes.yaml` was extracted from the goto-based switch statement context in vdbe.c. The extracted fragments:
- Assume availability of labels (abort_due_to_error, done_returning_row, jump_to_p2, etc.)
- Include intermediate goto statements without surrounding control structures
- Have unbalanced braces because they're extracted code segments, not complete functions
- Reference local variables from the sqlVdbeExec() context (e.g., pIn1, pIn2, pOut, memAboutToChange)

Example problem case (OP_Array):
```c
case OP_Array: {
    pOut = &aMem[P2];
    uint32_t size;
    struct region *region = &fiber()->gc;
    // ... more code ...
    if (val == NULL || mem_copy_array(pOut, val, size) != 0) {
        region_truncate(region, svp);
        rc = -1; break;    // Assuming rc variable exists
        // Missing close brace for "if"
    }
    // Incomplete code fragment
}
```

### Why It's Hard
The extracted inline code was designed for a specific context:
1. Goto-based dispatch where control flow jumps out of the case
2. Shared variables from sqlVdbeExec() like pIn1, pIn2, pOut
3. Macros like UPDATE_MAX_BLOBSIZE(), REGISTER_TRACE()
4. Return codes: 0=continue, -1=error, but some opcodes have special returns

## Solution Strategy

### Phase 5.6a: Refactor Inline Code in opcodes.yaml (PENDING)

Instead of trying to use goto-extracted code in a while-loop dispatcher, we need to:

1. **Wrap each inline code in a proper function**
   - Create inline handler functions similar to external handlers
   - Signature: `int vdbe_op_xxx_inline(Vdbe *p, Op *pOp, Mem *aMem)`
   - Return: 0=continue, -1=error, 1=special (jump/SQL_ROW)

2. **Refactor the extracted code**
   - Remove goto statements
   - Use return codes instead
   - Move local variable declarations to function scope
   - Handle error paths explicitly

3. **Keep the generator simple**
   - Generate: `int rc = vdbe_op_xxx_inline(p, pOp, aMem);`
   - Same pattern as external handlers

### Phase 5.6b: Incremental Integration (PENDING)

Instead of converting all 63 inline opcodes at once:

1. Start with simpler opcodes (no complex control flow)
   - Example: OP_Noop, OP_AddImm, OP_SetSession
   - Just requires register manipulation

2. Add opcodes with straightforward error handling
   - Example: OP_Array, OP_Real
   - Handle rc and break properly

3. Leave complex ones for later
   - OP_OpenSpace, OP_IteratorOpen (many branches)
   - OP_Seek* operations (multiple jump targets)
   - Can stay delegated to sqlVdbeExec()

### Phase 5.6c: Validation Testing (PENDING)

1. Test each inline opcode handler in isolation
2. Compare outputs with original sqlVdbeExec()
3. Use parallel validation mode for verification
4. Gradually increase coverage

## Implementation Recommendation

### Short-term (This Session)
1. Document the challenge (✓ DONE)
2. Build and test current Phase 5.5 dispatcher
3. Verify existing external handlers work correctly
4. Run test suite to ensure no regressions

### Medium-term (Next Session)
1. Create inline handler wrapper functions for simple opcodes
2. Test 5-10 simple inline handlers
3. Integrate into dispatcher
4. Parallel validate

### Long-term (When Stable)
1. Incrementally convert remaining inline opcodes
2. Remove goto-based dispatcher from sqlVdbeExec()
3. Make generated dispatcher the default
4. Archive old code

## Current Status

- **Phase 5.5**: ✓ Complete - Loop-based dispatcher skeleton works
- **Phase 5.6a**: PENDING - Need to refactor inline code
- **Phase 5.6b**: PENDING - Incremental integration strategy
- **Phase 5.6c**: PENDING - Validation infrastructure in place

## Files Affected

- `tools/vdbe_dsl/opcodes.yaml` - Update inline_code to use handler calls
- `src/box/sql/vdbe_ops_inline.c` - NEW: Create inline handler wrappers
- `src/box/sql/vdbe_dispatch_wrapper.c` - Expanded with all opcodes
- `tools/vdbe_codegen.py` - Already generates handler calls

## Risk Assessment

- **Low Risk**: Keeping Phase 5.5 delegation to sqlVdbeExec()
  - Current system works, no performance regression
  - Parallel validation framework ready
  - Can iterate on implementation

- **High Risk**: Trying to use extracted goto code in while-loop
  - Will have compilation errors (unbalanced braces)
  - Control flow logic breaks without proper context
  - Hard to debug

## Recommendation

Proceed with Phase 5.6 using this modified strategy:
1. Keep Phase 5.5 working (delegation to sqlVdbeExec)
2. Create inline handler wrappers one at a time
3. Test each integration carefully
4. Use parallel validation before committing

This is safer and more maintainable than trying to force goto-based code into a while-loop dispatcher.
