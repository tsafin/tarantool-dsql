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
int vdbe_op_string8(Vdbe *p, Op *pOp, Mem *aMem);
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
int vdbe_op_compare(Vdbe *p, Op *pOp, Mem *aMem);  /* Compare multiple fields */
/* Logical and bitwise opcodes */
int vdbe_op_and(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_or(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_not(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitand(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitor(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_bitnot(Vdbe *p, Op *pOp, Mem *aMem);
/* Limit/offset opcodes */
int vdbe_op_offsetlimit(Vdbe *p, Op *pOp, Mem *aMem);
/* String opcodes */
int vdbe_op_concat(Vdbe *p, Op *pOp, Mem *aMem);
/* Type conversion opcodes */
int vdbe_op_mustbeint(Vdbe *p, Op *pOp, Mem *aMem);
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
/* Wrapper functions for generated dispatcher */
int vdbe_op_seeklt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_seekgt(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_seekle(Vdbe *p, Op *pOp, Mem *aMem);
int vdbe_op_seekge(Vdbe *p, Op *pOp, Mem *aMem);
/* Index operation opcodes */
int vdbe_op_idx_compare(Vdbe *p, Op *pOp, Mem *aMem);  /* IdxGE/GT/LE/LT */
int vdbe_op_found_notfound_noconflict(Vdbe *p, Op *pOp, Mem *aMem);  /* Found/NotFound/NoConflict */
int vdbe_op_noconflict(Vdbe *p, Op *pOp, Mem *aMem);  /* NoConflict wrapper */
int vdbe_op_idx_insert_replace(Vdbe *p, Op *pOp, Mem *aMem);  /* IdxInsert/IdxReplace */
int vdbe_op_iteratoropen(Vdbe *p, Op *pOp, Mem *aMem);  /* IteratorOpen */
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
int vdbe_op_addimm_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* AddImm */
/* Medium complexity inline opcode handlers - Phase 5.6d */
int vdbe_op_transactioncommit_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* TransactionCommit */
int vdbe_op_droptuplecheckundidocheck_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* DropTupleCheck */
int vdbe_op_droptupleforeignkey_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* DropTupleForeignKey */
int vdbe_op_dropfieldcheck_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* DropFieldCheck */
int vdbe_op_dropfieldforeignkey_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* DropFieldForeignKey */
int vdbe_op_genspaceid_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* GenSpaceid */
/* Medium complexity inline opcode handlers - Phase 5.6e */
int vdbe_op_once_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Once */
int vdbe_op_ifnot_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* IfNot */
int vdbe_op_ifpos_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* IfPos */
int vdbe_op_ifnotzero_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* IfNotZero */
int vdbe_op_decrjumpzero_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* DecrJumpZero */
int vdbe_op_nullrow_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* NullRow */
/* Medium complexity inline opcode handlers - Phase 5.6f */
int vdbe_op_showcreatettable_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* ShowCreateTable */
int vdbe_op_resetsorter_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* ResetSorter */
int vdbe_op_sort_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Sort */
int vdbe_op_clear_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Clear */
int vdbe_op_param_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Param */
/* Medium complexity inline opcode handlers - Phase 5.6g */
int vdbe_op_decimal_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Decimal */
int vdbe_op_openspace_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* OpenSpace */
int vdbe_op_sequence_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Sequence */
int vdbe_op_sequencetest_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* SequenceTest */
int vdbe_op_fetch_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Fetch */
/* Medium complexity inline opcode handlers - Phase 5.6h */
int vdbe_op_shiftleft_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* ShiftLeft */
int vdbe_op_shiftright_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* ShiftRight */
int vdbe_op_string8_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* String8 */
int vdbe_op_array_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Array */
int vdbe_op_map_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Map */
int vdbe_op_getitem_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Getitem */
int vdbe_op_openpseudo_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* OpenPseudo */
int vdbe_op_count_inline(Vdbe *p, Op *pOp, Mem *aMem);  /* Count */
/* Function and session control opcodes */
int vdbe_op_builtinfunction(Vdbe *p, Op *pOp, Mem *aMem);  /* BuiltinFunction */
int vdbe_op_functionbyname(Vdbe *p, Op *pOp, Mem *aMem);  /* FunctionByName */
int vdbe_op_setsession(Vdbe *p, Op *pOp, Mem *aMem);  /* SetSession */
/* Control flow opcodes - reserved for dispatcher refactoring
 * See vdbe_ops_control.c for extraction plan.
 * These remain in vdbe.c for now due to PC manipulation complexity. */

#endif /* SRC_BOX_SQL_VDBE_OPS_H */
