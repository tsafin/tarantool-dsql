/* Header declaring extracted VDBE opcode handlers */
#ifndef SRC_BOX_SQL_VDBE_OPS_H
#define SRC_BOX_SQL_VDBE_OPS_H

#include "vdbeInt.h"

/* Handler prototypes return int (0 on success, non-zero on error).
 * Implementations live in vdbe_ops_*.c files. */
int vdbe_op_noop(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_add(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_sub(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_jump(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_multiply(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_divide(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_remainder(Vdbe *p, Op *pOp, Mem *aMem);
/* Data / constant opcodes */
int vdbe_op_integer(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bool(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_int64(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_real(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_string(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_null(Vdbe *p, Op *pOp, Mem *aMem);
/* Blob / variable / copy/move opcodes */
int vdbe_op_blob(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_variable(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_move(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_copy(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_scopy(Vdbe *p, Op *pOp, Mem *aMem);
/* Comparison opcodes (FUNCTIONAL - extracted after adding iCompare to Vdbe struct) */
int vdbe_op_eq(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_ne(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_lt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_le(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_gt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_ge(Vdbe *p, Op *pOp, Mem *aMem);
/* Logical and bitwise opcodes */
int vdbe_op_and(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_or(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_not(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitand(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitor(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitnot(Vdbe *p, Op *pOp, Mem *aMem);
/* String opcodes */
int vdbe_op_concat(Vdbe *p, Op *pOp, Mem *aMem);
/* Type conversion opcodes */
int vdbe_op_cast(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_applytype(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_makerecord(Vdbe *p, Op *pOp, Mem *aMem);
/* Aggregate function opcodes */
int vdbe_op_aggstep(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_aggfinal(Vdbe *p, Op *pOp, Mem *aMem);
/* Cursor data access opcodes */
int vdbe_op_resultrow(Vdbe *p, Op *pOp, Mem *aMem);  /* Returns 1 for SQL_ROW */
int vdbe_op_column(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_rowdata(Vdbe *p, Op *pOp, Mem *aMem);
/* Cursor navigation opcodes */
int vdbe_op_last(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_rewind(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_next(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_nextifopen(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_prev(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_previfopen(Vdbe *p, Op *pOp, Mem *aMem);
/* Cursor seek opcodes - return 2 for skip next opcode (SEEKEQ) */
int vdbe_op_seek_lt_gt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_seek_le_ge(Vdbe *p, Op *pOp, Mem *aMem);
/* Index operation opcodes */
int vdbe_op_idx_compare(Vdbe *p, Op *pOp, Mem *aMem);  /* IdxGE/GT/LE/LT */
int vdbe_op_found_notfound_noconflict(Vdbe *p, Op *pOp, Mem *aMem);  /* Found/NotFound/NoConflict */
int vdbe_op_idx_insert_replace(Vdbe *p, Op *pOp, Mem *aMem);  /* IdxInsert/IdxReplace */
/* Data modification opcodes */
int vdbe_op_delete(Vdbe *p, Op *pOp, Mem *aMem);  /* Delete */
int vdbe_op_update(Vdbe *p, Op *pOp, Mem *aMem);  /* Update */
int vdbe_op_sinsert(Vdbe *p, Op *pOp, Mem *aMem);  /* SInsert */
int vdbe_op_sdelete(Vdbe *p, Op *pOp, Mem *aMem);  /* SDelete */
int vdbe_op_idxdelete(Vdbe *p, Op *pOp, Mem *aMem);  /* IdxDelete */
/* Simple inline opcode handlers - Phase 5.6a */
int vdbe_op_noop_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Noop */
int vdbe_op_explain_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Explain */
int vdbe_op_skipload_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* SkipLoad */
int vdbe_op_expire_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Expire */
int vdbe_op_notnull_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* NotNull */
int vdbe_op_permutation_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Permutation */
/* Medium complexity inline opcode handlers - Phase 5.6b */
int vdbe_op_close_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Close */
int vdbe_op_isnull_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* IsNull */
/* Medium complexity inline opcode handlers - Phase 5.6c */
int vdbe_op_decimal_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Decimal */
int vdbe_op_addimm_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* AddImm */
int vdbe_op_sequence_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Sequence */
int vdbe_op_openspace_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* OpenSpace */
/* Control flow opcodes - reserved for dispatcher refactoring
 * See vdbe_ops_control.c for extraction plan.
 * These remain in vdbe.c for now due to PC manipulation complexity. */

#endif /* SRC_BOX_SQL_VDBE_OPS_H */
