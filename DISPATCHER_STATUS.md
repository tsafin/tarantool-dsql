# VDBE Generated Dispatcher Completion Status

## Current Status (Feb 14, 2026)
- **Opcodes handled**: 108 / 176 (61% complete)
- **Opcodes missing**: 68
- **Build status**: ✅ Compiles successfully
- **Test status**: ⚠️  Operand validation issues with debug build

## Progress

### Commit ab21afec42: Added 26 critical opcodes (Batch 1)
Successfully added:
- Comparison: Eq, Ne, Lt, Le, Gt, Ge (6)
- Logical: And, Or, Not, If (4)
- Data: Bool, Blob (2)
- Register: Copy (1)
- Type: Cast, ApplyType, Concat (3)
- Cursor: Column, MakeRecord (2)
- Seek: Found, NotFound (2)
- Modification: Delete, Update (2)
- Aggregates: AggStep, AggFinal (2)
- Transaction: TTransaction (1)

### Current Commit: Added 30 cursor/iteration opcodes (Batch 2)
Successfully added:
- Iterator: IteratorOpen, Rewind, Next, Prev, Last, NextIfOpen, PrevIfOpen (7)
- Seek: SeekLT, SeekGT, SeekLE, SeekGE (4)
- Index: IdxInsert, IdxReplace, IdxGE, IdxGT, IdxLE, IdxLT, IdxDelete (7)
- System: SInsert, SDelete (2)
- Data access: RowData (1)
- Data types: Int64, Real, Null, Variable (4)
- Register: Move, SCopy (2)
- Bitwise: BitAnd, BitOr, BitNot (3)

**Disabled pending investigation:**
- OP_NoConflict - operand validation assertion failure

### Known Issues
- `check_vdbe_operands` validation is stricter in generated dispatcher than inline
- OP_NoConflict fails with `assert(memIsValid(&aMem[pOp->p3]))` in debug builds
- Works fine with inline dispatcher (VDBE_USE_GENERATED_DISPATCH=OFF)

## Remaining Work

### High Priority (needed for basic CRUD)
1. **OP_IteratorOpen** - open cursor for iteration
2. **OP_Insert** - insert data into space
3. **OP_OpenSpace** - already handled ✓
4. **OP_SInsert** - system space insert
5. **OP_IdxInsert** - index insert
6. **OP_IdxReplace** - index replace

### Medium Priority (needed for complex queries)
7-30. Remaining cursor ops, comparisons, type conversions
31-60. Index operations, sorter operations
61-80. Aggregate functions, subqueries

### Low Priority (advanced features)
81-98. DDL operations, system operations, special cases

## Strategy

### Approach 1: Continue Incremental (Recommended)
- Add opcodes as we encounter them in testing
- Each batch: add opcodes → build → test → commit
- Ensures working code at each step
- Current: 78/176 (53%)
- Target: ~120/176 (68%) for full CRUD support

### Approach 2: Bulk Addition
- Reference vdbe.c for all remaining opcodes
- Add all ~98 missing opcodes in one pass
- Risk: harder to debug if issues arise
- Benefit: complete coverage quickly

## Testing Plan

After each batch, verify:
```bash
./src/tarantool -e "
box.cfg{}
box.execute('CREATE TABLE t1 (id INT PRIMARY KEY, name TEXT)')
box.execute('INSERT INTO t1 VALUES (1, \"test\")')
local r = box.execute('SELECT * FROM t1')
print('Result:', r.rows[1][1], r.rows[1][2])
box.execute('UPDATE t1 SET name=\"updated\" WHERE id=1')
box.execute('DELETE FROM t1 WHERE id=1')
os.exit(0)
"
```

## Notes

- OP_If required inline implementation (lines 1600-1616 of vdbe.c)
- OP_Found/NotFound use composite handler `vdbe_op_found_notfound_noconflict`
- OP_TTransaction inline: checks box_txn(), creates savepoint if needed
- Many opcodes have `_inline` suffix handlers (e.g., vdbe_op_count_inline)

