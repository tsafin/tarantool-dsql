/* Cursor navigation opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"
#include "tarantoolInt.h"

#ifdef SQL_TEST
extern int sql_search_count;
#endif

/* Opcode: Last P1 P2 P3 * *
 *
 * The next use of the Column or Prev instruction for P1
 * will refer to the last entry in the database table or index.
 * If the table or index is empty and P2>0, then jump immediately to P2.
 * If P2 is 0 or if the table or index is not empty, fall through
 * to the following instruction.
 *
 * This opcode leaves the cursor configured to move in reverse order,
 * from the end toward the beginning.  In other words, the cursor is
 * configured to use Prev, not Next.
 *
 * If P3 is -1, then the cursor is positioned at the end of the btree
 * for the purpose of appending a new entry onto the btree.  In that
 * case P2 must be 0.  It is assumed that the cursor is used only for
 * appending and so if the cursor is valid, then the cursor must already
 * be pointing at the end of the btree and so no changes are made to
 * the cursor.
 *
 * Returns:
 *   0 = continue to next instruction
 *   1 = jump to P2 (when table is empty and P2 > 0)
 *  -1 = error
 */
int vdbe_op_last(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;
	BtCursor *pCrsr;
	int res;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	pCrsr = pC->uc.pCursor;
	res = 0;
	assert(pCrsr != 0);
	pC->seekResult = pOp->p3;
#ifdef SQL_DEBUG
	pC->seekOp = OP_Last;
#endif
	if (pOp->p3 == 0 || !sqlCursorIsValidNN(pCrsr)) {
		if (tarantoolsqlLast(pCrsr, &res) != 0)
			return -1;
		pC->nullRow = (u8)res;
		pC->cacheStatus = CACHE_STALE;
		if (pOp->p2 > 0 && res != 0)
			return 1;  /* Jump to P2 */
	} else {
		assert(pOp->p2 == 0);
	}
	return 0;
}

/* Opcode: Rewind P1 P2 * * *
 *
 * The next use of the Column or Next instruction for P1
 * will refer to the first entry in the database table or index.
 * If the table or index is empty, jump immediately to P2.
 * If the table or index is not empty, fall through to the following
 * instruction.
 *
 * This opcode leaves the cursor configured to move in forward order,
 * from the beginning toward the end.  In other words, the cursor is
 * configured to use Next, not Prev.
 *
 * Returns:
 *   0 = continue to next instruction (table/index not empty)
 *   1 = jump to P2 (table/index is empty)
 *  -1 = error
 */
int vdbe_op_rewind(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;
	BtCursor *pCrsr;
	int res;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);
	res = 1;
#ifdef SQL_DEBUG
	pC->seekOp = OP_Rewind;
#endif
	if (pC->eCurType == CURTYPE_SORTER) {
		if (sqlVdbeSorterRewind(pC, &res) != 0)
			return -1;
	} else {
		assert(pC->eCurType == CURTYPE_TARANTOOL);
		pCrsr = pC->uc.pCursor;
		assert(pCrsr);
		if (tarantoolsqlFirst(pCrsr, &res) != 0)
			return -1;
		pC->cacheStatus = CACHE_STALE;
	}
	pC->nullRow = (u8)res;
	assert(pOp->p2 > 0 && pOp->p2 < p->nOp);
	if (res)
		return 1;  /* Jump to P2 */
	return 0;
}

/* Opcode: Next P1 P2 P3 P4 P5
 *
 * Advance cursor P1 so that it points to the next key/data pair in its
 * table or index.  If there are no more key/value pairs then fall through
 * to the following instruction.  But if the cursor advance was successful,
 * jump immediately to P2.
 *
 * The Next opcode is only valid following an SeekGT, SeekGE, or
 * OP_Rewind opcode used to position the cursor.  Next is not allowed
 * to follow SeekLT, SeekLE, or OP_Last.
 *
 * The P1 cursor must be for a real table, not a pseudo-table.  P1 must have
 * been opened prior to this opcode or the program will segfault.
 *
 * The P3 value is a hint to the btree implementation. If P3==1, that
 * means P1 is an SQL index and that this instruction could have been
 * omitted if that index had been unique.  P3 is usually 0.  P3 is
 * always either 0 or 1.
 *
 * P4 is always of type P4_ADVANCE. The function pointer points to
 * sqlBtreeNext().
 *
 * If P5 is positive and the jump is taken, then event counter
 * number P5-1 in the prepared statement is incremented.
 *
 * See also: Prev, NextIfOpen
 *
 * Returns:
 *   res value (0 or 1) from xAdvance on success
 *  -1 = error
 */
int vdbe_op_next(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;
	int res;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	res = pOp->p3;
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	assert(res == 0 || res == 1);
	assert(pOp->p4.xAdvance == sqlCursorNext);

	/* The Next opcode is only used after SeekGT, SeekGE, and Rewind. */
	assert(pC->seekOp == OP_SeekGT || pC->seekOp == OP_SeekGE
	       || pC->seekOp == OP_Rewind || pC->seekOp == OP_Found);

	if (pOp->p4.xAdvance(pC->uc.pCursor, &res) != 0)
		return -1;

	return res;  /* Return res for next_tail to process */
}

/* Opcode: NextIfOpen P1 P2 P3 P4 P5
 *
 * This opcode works just like Next except that if cursor P1 is not
 * open it behaves a no-op.
 *
 * Returns:
 *   0 = continue to next instruction (cursor not open or no more records)
 *   1 = jump to P2 (cursor open and advance was successful)
 *  -1 = error
 */
int vdbe_op_nextifopen(Vdbe *p, Op *pOp, Mem *aMem)
{
	if (p->apCsr[pOp->p1] == 0)
		return 0;  /* Cursor not open - no-op */

	/* Cursor is open - delegate to vdbe_op_next */
	return vdbe_op_next(p, pOp, aMem);
}

/* Opcode: Prev P1 P2 P3 P4 P5
 *
 * Back up cursor P1 so that it points to the previous key/data pair in its
 * table or index.  If there is no previous key/value pairs then fall through
 * to the following instruction.
 *
 * The Prev opcode is only valid following an SeekLT, SeekLE, or
 * OP_Last opcode used to position the cursor.  Prev is not allowed
 * to follow SeekGT, SeekGE, or OP_Rewind.
 *
 * The P1 cursor must be for a real table, not a pseudo-table.  If P1 is
 * not open then the behavior is undefined.
 *
 * The P3 value is a hint to the btree implementation. If P3==1, that
 * means P1 is an SQL index and that this instruction could have been
 * omitted if that index had been unique.  P3 is usually 0.  P3 is
 * always either 0 or 1.
 *
 * P4 is always of type P4_ADVANCE. The function pointer points to
 * sqlBtreePrevious().
 *
 * If P5 is positive and the jump is taken, then event counter
 * number P5-1 in the prepared statement is incremented.
 *
 * Returns:
 *   res value (0 or 1) from xAdvance on success
 *  -1 = error
 */
int vdbe_op_prev(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;
	int res;

	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	pC = p->apCsr[pOp->p1];
	res = pOp->p3;
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	assert(res == 0 || res == 1);
	assert(pOp->p4.xAdvance == sqlCursorPrevious);

	/* The Prev opcode is only used after SeekLT, SeekLE, and Last. */
	assert(pC->seekOp == OP_SeekLT || pC->seekOp == OP_SeekLE
	       || pC->seekOp == OP_Last);

	if (pOp->p4.xAdvance(pC->uc.pCursor, &res) != 0)
		return -1;

	return res;  /* Return res for next_tail to process */
}

/* Opcode: PrevIfOpen P1 P2 P3 P4 P5
 *
 * This opcode works just like Prev except that if cursor P1 is not
 * open it behaves a no-op.
 *
 * Returns:
 *   0 = continue to next instruction (cursor not open or no more records)
 *   1 = jump to P2 (cursor open and advance was successful)
 *  -1 = error
 */
int vdbe_op_previfopen(Vdbe *p, Op *pOp, Mem *aMem)
{
	if (p->apCsr[pOp->p1] == 0)
		return 0;  /* Cursor not open - no-op */

	/* Cursor is open - delegate to vdbe_op_prev */
	return vdbe_op_prev(p, pOp, aMem);
}
