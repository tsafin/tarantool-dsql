/* Cursor seek opcode handlers extracted from vdbe.c
 *
 * These handlers implement cursor seeking operations (SeekLT, SeekGT, SeekLE, SeekGE).
 * They position cursors at specific locations in B-trees and indexes based on key values.
 *
 * Return values:
 * - 0: Continue normally (DISPATCH)
 * - 1: Jump to P2 (key not found)
 * - 2: Skip next opcode (only for SeekLE/SeekGE with OPFLAG_SEEKEQ)
 * - -1: Error occurred
 */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "box/error.h"
#include "tarantoolInt.h"

#ifdef SQL_TEST
extern int sql_search_count;
#endif

/* Return values for seek handlers */
#define VDBE_SEEK_CONTINUE  0   /* Continue to next instruction */
#define VDBE_SEEK_JUMP      1   /* Jump to P2 (not found) */
#define VDBE_SEEK_SKIP      2   /* Skip next opcode (SeekLE/GE with SEEKEQ) */
#define VDBE_SEEK_ERROR    -1   /* Error occurred */

/* Opcode: SeekLT P1 P2 P3 P4 *
 * Synopsis: key=r[P3@P4]
 *
 * If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
 * use the value in register P3 as a key. If cursor P1 refers
 * to an SQL index, then P3 is the first in an array of P4 registers
 * that are used as an unpacked index key.
 *
 * Reposition cursor P1 so that  it points to the largest entry that
 * is less than the key value. If there are no records less than
 * the key and P2 is not zero, then jump to P2.
 *
 * This opcode leaves the cursor configured to move in reverse order,
 * from the end toward the beginning.  In other words, the cursor is
 * configured to use Prev, not Next.
 *
 * See also: Found, NotFound, SeekGt, SeekGe, SeekLe
 */
/* Opcode: SeekGT P1 P2 P3 P4 *
 * Synopsis: key=r[P3@P4]
 *
 * If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
 * use the value in register P3 as a key. If cursor P1 refers
 * to an SQL index, then P3 is the first in an array of P4 registers
 * that are used as an unpacked index key.
 *
 * Reposition cursor P1 so that  it points to the smallest entry that
 * is greater than the key value. If there are no records greater than
 * the key and P2 is not zero, then jump to P2.
 *
 * This opcode leaves the cursor configured to move in forward order,
 * from the beginning toward the end.  In other words, the cursor is
 * configured to use Next, not Prev.
 */
int
vdbe_op_seek_lt_gt(Vdbe *p, Op *pOp, Mem *aMem)
{
	bool is_lt = pOp->opcode == OP_SeekLT;
	struct VdbeCursor *cur = p->apCsr[pOp->p1];
#ifdef SQL_DEBUG
	cur->seekOp = pOp->opcode;
#endif
	cur->nullRow = 0;
	cur->uc.pCursor->iter_type = is_lt ? ITER_LT : ITER_GT;

	uint32_t len = pOp->p4.i;
	assert(pOp->p4type == P4_INT32);
	assert(len <= cur->key_def->part_count);
	struct Mem *mems = &aMem[pOp->p3];
	bool is_op_change = false;
	for (uint32_t i = 0; i < len; ++i) {
		enum field_type type = cur->key_def->parts[i].type;
		struct Mem *mem = &mems[i];
		if (mem_is_field_compatible(mem, type))
			continue;
		if (!sql_type_is_numeric(type) || !mem_is_num(mem)) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 mem_str(mem), field_type_strs[type]);
			return VDBE_SEEK_ERROR;
		}
		int cmp = mem_cast_implicit_number(mem, type);
		is_op_change = is_op_change || (is_lt && cmp > 0) ||
			       (!is_lt && cmp < 0);
	}
	if (is_op_change)
		cur->uc.pCursor->iter_type = is_lt ? ITER_LE : ITER_GE;

	int res;
	if (sql_cursor_seek(cur->uc.pCursor, mems, len, &res) != 0)
		return VDBE_SEEK_ERROR;
	assert((res != 0) == (cur->uc.pCursor->eState == CURSOR_INVALID));
	cur->cacheStatus = CACHE_STALE;
#ifdef SQL_TEST
	sql_search_count++;
#endif
	if (res != 0)
		return VDBE_SEEK_JUMP;
	return VDBE_SEEK_CONTINUE;
}

/* Opcode: SeekLE P1 P2 P3 P4 *
 * Synopsis: key=r[P3@P4]
 *
 * If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
 * use the value in register P3 as a key. If cursor P1 refers
 * to an SQL index, then P3 is the first in an array of P4 registers
 * that are used as an unpacked index key.
 *
 * Reposition cursor P1 so that it points to the largest entry that
 * is less than or equal to the key value. If there are no records
 * less than or equal to the key and P2 is not zero, then jump to P2.
 *
 * This opcode leaves the cursor configured to move in reverse order,
 * from the end toward the beginning.  In other words, the cursor is
 * configured to use Prev, not Next.
 *
 * If the cursor P1 was opened using the OPFLAG_SEEKEQ flag, then this
 * opcode will always land on a record that equally equals the key, or
 * else jump immediately to P2.  When the cursor is OPFLAG_SEEKEQ, this
 * opcode must be followed by an IdxGE opcode with the same arguments.
 * The IdxGE opcode will be skipped if this opcode succeeds, but the
 * IdxGE opcode will be used on subsequent loop iterations.
 *
 * See also: Found, NotFound, SeekGt, SeekGe, SeekLt
 */
/* Opcode: SeekGE P1 P2 P3 P4 *
 * Synopsis: key=r[P3@P4]
 *
 * If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
 * use the value in register P3 as the key.  If cursor P1 refers
 * to an SQL index, then P3 is the first in an array of P4 registers
 * that are used as an unpacked index key.
 *
 * Reposition cursor P1 so that  it points to the smallest entry that
 * is greater than or equal to the key value. If there are no records
 * greater than or equal to the key and P2 is not zero, then jump to P2.
 *
 * If the cursor P1 was opened using the OPFLAG_SEEKEQ flag, then this
 * opcode will always land on a record that equally equals the key, or
 * else jump immediately to P2.  When the cursor is OPFLAG_SEEKEQ, this
 * opcode must be followed by an IdxLE opcode with the same arguments.
 * The IdxLE opcode will be skipped if this opcode succeeds, but the
 * IdxLE opcode will be used on subsequent loop iterations.
 *
 * This opcode leaves the cursor configured to move in forward order,
 * from the beginning toward the end.  In other words, the cursor is
 * configured to use Next, not Prev.
 *
 * See also: Found, NotFound, SeekLt, SeekGt, SeekLe
 */
int
vdbe_op_seek_le_ge(Vdbe *p, Op *pOp, Mem *aMem)
{
	bool is_le = pOp->opcode == OP_SeekLE;
	struct VdbeCursor *cur = p->apCsr[pOp->p1];
#ifdef SQL_DEBUG
	cur->seekOp = pOp->opcode;
#endif
	cur->nullRow = 0;
	bool is_eq = (cur->uc.pCursor->hints & OPFLAG_SEEKEQ) != 0;
	if (is_le)
		cur->uc.pCursor->iter_type = is_eq ? ITER_REQ : ITER_LE;
	else
		cur->uc.pCursor->iter_type = is_eq ? ITER_EQ : ITER_GE;
	assert(!is_eq || pOp[1].opcode == OP_IdxLT ||
	       pOp[1].opcode == OP_IdxGT);

	uint32_t len = pOp->p4.i;
	assert(pOp->p4type == P4_INT32);
	assert(len <= cur->key_def->part_count);
	struct Mem *mems = &aMem[pOp->p3];
	bool is_op_change = false;
	bool is_zero = false;
	for (uint32_t i = 0; i < len; ++i) {
		enum field_type type = cur->key_def->parts[i].type;
		struct Mem *mem = &mems[i];
		if (mem_is_field_compatible(mem, type))
			continue;
		if (!sql_type_is_numeric(type) || !mem_is_num(mem)) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 mem_str(mem), field_type_strs[type]);
			return VDBE_SEEK_ERROR;
		}
		int cmp = mem_cast_implicit_number(mem, type);
		is_op_change = is_op_change || (is_le && cmp < 0) ||
			       (!is_le && cmp > 0);
		/*
		 * In case search using EQ or REQ, we will not find anything if
		 * conversion cannot be precise.
		 */
		is_zero = is_zero || (is_eq && cmp != 0);
	}
	if (is_zero) {
		/* Jump to P2 immediately */
		return VDBE_SEEK_JUMP;
	}
	if (!is_eq && is_op_change)
		cur->uc.pCursor->iter_type = is_le ? ITER_LT : ITER_GT;

	int res;
	if (sql_cursor_seek(cur->uc.pCursor, mems, len, &res) != 0)
		return VDBE_SEEK_ERROR;
	assert((res != 0) == (cur->uc.pCursor->eState == CURSOR_INVALID));
	cur->cacheStatus = CACHE_STALE;
#ifdef SQL_TEST
	sql_search_count++;
#endif
	if (res != 0)
		return VDBE_SEEK_JUMP;
	/* Skip the OP_IdxLT/OP_IdxGT that follows if we have EQ. */
	if (is_eq)
		return VDBE_SEEK_SKIP;
	return VDBE_SEEK_CONTINUE;
}

/* Wrapper functions for generated dispatcher */

int
vdbe_op_seeklt(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_seek_lt_gt(p, pOp, aMem);
}

int
vdbe_op_seekgt(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_seek_lt_gt(p, pOp, aMem);
}

int
vdbe_op_seekle(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_seek_le_ge(p, pOp, aMem);
}

int
vdbe_op_seekge(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_seek_le_ge(p, pOp, aMem);
}
