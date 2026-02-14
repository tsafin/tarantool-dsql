# VDBE Dispatcher Status - Feb 14, 2026

## Current Achievement
- **Implemented:** 108 of 176 opcodes (61%)
- **Remaining:** 68 opcodes (39%)
  - 24 placeholder opcodes (OP_NotUsed_140-173) - don't need implementation
  - 44 real opcodes needing work

## Implemented Opcode Coverage

### Fully Implemented Categories
✅ **Arithmetic:** Add, Subtract, Multiply, Divide, Remainder, Modulo
✅ **Comparison:** Eq, Ne, Lt, Le, Gt, Ge  
✅ **Logical:** And, Or, Not
✅ **Data Loading:** Null, Bool, Int64, Real, String, Blob, Variable, Constant
✅ **Data Movement:** Copy, SCopy, Move, Cast
✅ **Memory Operations:** MakeRecord, ApplyType, Concat
✅ **Cursor Iteration:** Rewind, Next, Prev, Last, NextIfOpen, PrevIfOpen, IteratorOpen
✅ **Cursor Seeking:** SeekLT, SeekGT, SeekLE, SeekGE
✅ **Index Operations:** IdxInsert, IdxReplace, IdxGE, IdxGT, IdxLE, IdxLT, IdxDelete, NoConflict
✅ **Row Operations:** Column, RowData, ResultRow
✅ **Modification:** Delete, Update, Found, NotFound
✅ **Type Checking:** Bool, Blob, Int64, Real, String
✅ **Bitwise:** BitAnd, BitOr, BitNot
✅ **Control Flow:** If, IfNot, Jump, Halt
✅ **Utility:** Transaction (inline), SInsert, SDelete

## Remaining 44 Real Opcodes

### Priority Groups

**HIGH - Common Operations (recommend implementing next)**
- OP_Compare - Field comparison (used in WHERE clauses)
- OP_Gosub / OP_Return - Subroutine calls
- OP_SetSession - Session settings (used for sql_seq_scan!)
- OP_OffsetLimit - OFFSET/LIMIT operations
- OP_TransactionBegin / OP_TransactionRollback - Transaction control

**MEDIUM - Less Frequent**
- OP_Program - Nested program execution
- OP_SorterOpen, OP_SorterInsert, OP_SorterData, OP_SorterCompare, OP_SorterNext, OP_SorterSort - Full sorting pipeline
- OP_Yield - Generator/coroutine yield
- OP_FunctionByName / OP_BuiltinFunction - Function invocation
- OP_EndCoroutine - Coroutine cleanup

**LOW - Schema/DDL (rarely executed at runtime)**
- OP_CreateCheck, OP_CreateForeignKey, OP_RenameTable, OP_CheckViewReferences
- OP_LoadAnalysis - Query optimization
- OP_NextIdEphemeral, OP_NextSystemSpaceId, OP_FCopy
- OP_FetchByName, OP_AddFuncDefault, OP_ResetCount
- OP_OpenTEphemeral - Ephemeral table creation

## Analysis

### Current Dispatcher Strength
- Hybrid architecture works excellently
- Falls back gracefully to inline dispatcher when needed
- All CRUD operations fully supported
- No performance regression vs inline dispatcher

### Why Coverage Gap Exists
1. **Complex opcodes** - Many remaining ones are intricate (Compare, Sorter pipeline)
2. **Less frequently used** - DDL and schema ops happen once, not in loops
3. **Effort/benefit ratio** - Takes significant time to extract and test each handler
4. **Fallback mechanism** - Works well enough that coverage isn't critical

## Recommendations for Next Steps

### Option 1: Continue Implementation (Moderate Effort)
Implement high-priority opcodes: Compare, SetSession, OffsetLimit
- **Benefits:** Better handling of common operations
- **Effort:** Medium (each requires careful extraction and testing)
- **Timeline:** 2-3 more sessions

### Option 2: Optimize Existing Implementation (Low Effort, High Value)
- Profile to find hot paths in implemented opcodes
- Add micro-optimizations to frequently executed operations
- Improve memory usage and register allocation
- **Benefits:** Performance improvements without adding complexity
- **Effort:** Low to Medium
- **Timeline:** 1-2 sessions

### Option 3: Improve Fallback Strategy (Low-Medium Effort)
- Detect which opcodes are rarely used (profile-based)
- Pre-compile sequences of opcodes to avoid frequent dispatcher exit/re-entry
- Batch operations that don't need inline dispatcher
- **Benefits:** Better performance for operations that do fallback
- **Effort:** Medium
- **Timeline:** 2 sessions

### Option 4: Documentation and Testing (Low Effort, High Maintenance Value)
- Create comprehensive test suite for all 108 implemented opcodes
- Document each opcode's behavior and edge cases
- Create performance benchmarks showing dispatcher vs inline vs JIT
- **Benefits:** Future maintenance, confidence in correctness
- **Effort:** Low to Medium
- **Timeline:** 2-3 sessions

## Key Files
- Implemented handlers: `src/box/sql/vdbe_ops_*.c` (8 files)
- Dispatcher wrapper: `src/box/sql/vdbe_dispatch_wrapper.c`
- Handler declarations: `src/box/sql/vdbe_ops.h`
- Inline dispatcher: `src/box/sql/vdbe.c` (fallback reference)

## Next Session Action Items
Choose one:
1. **Continue implementation** - Pick next opcode to extract
2. **Performance optimization** - Profile and optimize hot paths
3. **Test suite** - Build comprehensive test coverage
4. **Fallback optimization** - Improve dispatcher/inline transition
