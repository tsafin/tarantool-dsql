/* Sorter and ephemeral table opcode handlers extracted from vdbe.c
 *
 * Implements:
 * - OP_SorterOpen: Initialize a sorter cursor
 * - OP_SorterInsert: Insert record into sorter
 * - OP_SorterNext: Advance to next sorter record
 * - OP_SorterData: Extract current sorter data
 * - OP_SorterCompare: Compare sorter data
 */

#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "key_def.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"

/* Opcode: SorterOpen P1 P2 P3 P4 *
 * Synopsis: Opens a cursor P1 on a sorter for sorting.
 *
 * P1 is the cursor number. P2 is the register containing memory.
 * P3 is the index of the key_def. P4 is the key definition
 * to use for sorting (a key_info structure).
 *
 * This opcode opens a cursor on a sorter, which is a temporary
 * data structure that sorts records as they are inserted into it.
 * The sorter is later used to deliver the data in sorted order.
 */
int
vdbe_op_sorteropen(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pCx;

	assert(pOp->p1 >= 0);
	assert(pOp->p2 >= 0);

	/* Convert key_info structure to key_def */
	struct key_def *def = sql_key_info_to_key_def(pOp->p4.key_info);
	if (def == NULL)
		return -1;

	/* Allocate and initialize sorter cursor */
	pCx = allocateCursor(p, pOp->p1, pOp->p2, CURTYPE_SORTER);
	if (pCx == NULL)
		return -1;

	pCx->key_def = def;

	/* Initialize the sorter */
	if (sqlVdbeSorterInit(pCx) != 0)
		return -1;

	return 0;
}

/* Opcode: SorterInsert P1 P2 P3 * *
 * Synopsis: key=r[P2]
 *
 * P1 is the index of a sorter cursor.
 *
 * If P3 is 0, P2 is the index of a memory cell whose value is the
 * prebuilt key for the sorter entry to be inserted.
 *
 * If P3 is not 0, registers r[P2@P3] are encoded directly into the
 * sorter entry without building an intermediate MakeRecord blob.
 *
 * Insert the record at P2 into the sorter at P1. The record is
 * inserted such that it will be emitted in sorted order by the
 * sorter's comparison function.
 */
int
vdbe_op_sorterinsert(Vdbe *p, Op *pOp, Mem *aMem)
{
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	struct VdbeCursor *cursor = p->apCsr[pOp->p1];
	assert(cursor != NULL);
	assert(isSorter(cursor));

	if (pOp->p3 != 0) {
		assert(pOp->p2 >= 0);
		if (sqlVdbeSorterWriteFromMems(cursor, &aMem[pOp->p2],
					       pOp->p3) != 0)
			return -1;
	} else {
		Mem *pIn2 = &aMem[pOp->p2];
		assert(mem_is_bin(pIn2));
		if (sqlVdbeSorterWrite(cursor, pIn2) != 0)
			return -1;
	}

	return 0;
}

/* Opcode: SorterNext P1 P2 * * *
 * Synopsis: Jump if finished.
 *
 * P1 is a sorter cursor. This opcode advances the cursor to the next
 * sorted record. If there are no more records to return, jump to P2.
 *
 * This is essentially identical to OP_Next except that it applies to
 * a sorter rather than a b-tree cursor.
 */
int
vdbe_op_sorternext(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC = p->apCsr[pOp->p1];
	assert(isSorter(pC));

	int res = 0;
	if (sqlVdbeSorterNext(pC, &res) != 0)
		return -1;

	/* Return 0 when more rows available, 1 when exhausted (like OP_Next) */
	return res;
}

int
vdbe_op_sorternext_jit(Vdbe *p, Op *pOp, Mem *aMem)
{
	int rc = vdbe_op_sorternext(p, pOp, aMem);
	if (rc < 0)
		return -1;
	return rc == 0 ? 1 : 0;
}

/* Opcode: SorterData P1 P2 P3 * *
 * Synopsis: r[P2]=data
 *
 * Write into register P2 the current sorter data for sorter cursor P1.
 * Then clear the column header cache on cursor P3.
 *
 * This opcode is normally used to move a record out of the sorter and
 * into a register that is the source for a pseudo-table cursor created
 * using OpenPseudo. That pseudo-table cursor is the one identified by
 * parameter P3. Clearing the P3 column cache as part of this opcode saves
 * us from having to issue a separate NullRow instruction to clear that cache.
 */
int
vdbe_op_sorterdata(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;

	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	pC = p->apCsr[pOp->p1];
	assert(isSorter(pC));

	/* Get the row key from the sorter */
	if (sqlVdbeSorterRowkey(pC, pOut) != 0)
		return -1;

	assert(mem_is_bin(pOut));
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);

	/* Invalidate the column header cache for P3 cursor */
	if (pOp->p3 >= 0 && pOp->p3 < p->nCursor && p->apCsr[pOp->p3] != NULL)
		p->apCsr[pOp->p3]->cacheStatus = CACHE_STALE;

	return 0;
}

/* Opcode: SorterCompare P1 P2 P3 P4 *
 * Synopsis: if (cmp(key in P3, current sorter key) != 0) jump to P2
 *
 * P1 is a sorter cursor. P3 is a memory cell containing a key from
 * somewhere else. P4 is the number of key columns.
 *
 * Compare the key in memory cell P3 against the current sorter key.
 * If they are different, jump to P2. The current sorter key is the
 * key for the current entry in the sorter.
 *
 * The comparison is done using the sort order of the sorter's key_def.
 */
int
vdbe_op_sortercompare(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeCursor *pC;
	int res;
	int nKeyCol;

	pC = p->apCsr[pOp->p1];
	assert(isSorter(pC));
	assert(pOp->p4type == P4_INT32);

	Mem *pIn3 = &aMem[pOp->p3];
	nKeyCol = pOp->p4.i;

	/* Compare current sorter key with key in pIn3 */
	if (sqlVdbeSorterCompare(pC, pIn3, nKeyCol, &res) != 0)
		return -1;

	/* Return 1 to jump if different, 0 to continue */
	return res != 0 ? 1 : 0;
}

static int
vdbe_op_sortercompare_raw(Vdbe *p, Op *pOp, Mem *aMem,
			  int (*cmp_fn)(const VdbeSorter *, uint32_t,
					const void *, const void *, bool),
			  uint32_t part_count)
{
	VdbeCursor *pC = p->apCsr[pOp->p1];
	assert(isSorter(pC));
	assert(pOp->p4type == P4_INT32);
	Mem *pIn3 = &aMem[pOp->p3];
	if (!mem_is_bin(pIn3))
		return vdbe_op_sortercompare(p, pOp, aMem);
	int nKey = 0;
	const void *pKey = sqlVdbeSorterRowkeyRaw(pC, &nKey);
	if (pKey == NULL)
		return -1;
	int rc = cmp_fn(pC->uc.pSorter, part_count, pIn3->z, pKey, true);
	if (rc == -2)
		return vdbe_op_sortercompare(p, pOp, aMem);
	return rc != 0 ? 1 : 0;
}

int
vdbe_op_sortercompare_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_sortercompare_raw(p, pOp, aMem, sqlVdbeSorterCompareRawKey,
					 (uint32_t)pOp->p4.i);
}

int
vdbe_op_sortercompare_intlike2(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_sortercompare_raw(p, pOp, aMem,
					 sqlVdbeSorterCompareRawKeyIntLike2, 2);
}

int
vdbe_op_sortercompare_intlike3(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_sortercompare_raw(p, pOp, aMem,
					 sqlVdbeSorterCompareRawKeyIntLike3, 3);
}

int
vdbe_op_sortercompare_intlike4(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_sortercompare_raw(p, pOp, aMem,
					 sqlVdbeSorterCompareRawKeyIntLike4, 4);
}

/* Opcode: SorterSort P1 P2 * * *
 * Synopsis: Sorter P1 has just finished loading all its data.
 * Jump to P2 if there are no records to be sorted.
 *
 * After all records have been inserted into the Sorter object
 * identified by P1, invoke this opcode to actually do the sorting.
 * This is essentially the same as OP_Rewind but for a sorter.
 * Jump to P2 if the sorter is empty (no records to return).
 *
 * OP_SorterSort is an alias for OP_Rewind when used with a sorter cursor,
 * so we just delegate to the standard Rewind handler which already knows
 * how to handle both regular cursors and sorter cursors.
 */
int
vdbe_op_sortersort(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)pOp;
	(void)aMem;
#ifdef SQL_TEST
	extern int sql_sort_count;
	extern int sql_search_count;
	sql_sort_count++;
	sql_search_count--;
#endif
	/* Delegate to the standard Rewind handler which handles sorters */
	return vdbe_op_rewind(p, pOp, aMem);
}
