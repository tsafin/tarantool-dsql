/* Cursor data access opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"
#include "tarantoolInt.h"

#ifdef SQL_TEST
extern int sql_xfer_count;
#endif

/* Opcode: ResultRow P1 P2 * * *
 * Synopsis: output=r[P1@P2]
 *
 * The registers P1 through P1+P2-1 contain a single row of
 * results. This opcode causes the sql_step() call to terminate
 * with an SQL_ROW return code and it sets up the sql_stmt
 * structure to provide access to the r(P1)..r(P1+P2-1) values as
 * the result row.
 *
 * Returns 1 to signal that the VDBE should return SQL_ROW to the caller.
 */
int vdbe_op_resultrow(Vdbe *p, Op *pOp, Mem *aMem)
{
	assert(p->nResColumn == pOp->p2);
	assert(pOp->p1 > 0);
	assert(pOp->p1 + pOp->p2 <= (p->nMem+1 - p->nCursor) + 1);
	assert(p->iStatement == 0 && p->anonymous_savepoint == NULL);

	/* Invalidate all ephemeral cursor row caches */
	p->cacheCtr = (p->cacheCtr + 2) |1;

	p->pResultSet = &aMem[pOp->p1];
#ifdef SQL_DEBUG
	struct Mem *pMem = p->pResultSet;
	for (int i = 0; i < pOp->p2; i++) {
		assert(memIsValid(&pMem[i]));
		REGISTER_TRACE(p, pOp->p1+i, &pMem[i]);
	}
#endif

	/* NOTE: Trace functionality (db->mTrace) removed - db is not accessible */

	/* Set PC for return and signal that we should return SQL_ROW */
	p->pc = (int)(pOp - p->aOp) + 1;
	return 1;  /* Special return: caller should return SQL_ROW */
}

/* Opcode: Column P1 P2 P3 P4 P5
 * Synopsis: r[P3]=PX
 *
 * Interpret the data that cursor P1 points to as a structure built using
 * the MakeRecord instruction.  (See the MakeRecord opcode for additional
 * information about the format of the data.)  Extract the P2-th column
 * from this record.  If there are less that (P2+1)
 * values in the record, extract a NULL.
 *
 * The value extracted is stored in register P3.
 *
 * If the column contains fewer than P2 fields, then extract a NULL.  Or,
 * if the P4 argument is a P4_MEM use the value of the P4 argument as
 * the result.
 *
 * If the OPFLAG_CLEARCACHE bit is set on P5 and P1 is a pseudo-table cursor,
 * then the cache of the cursor is reset prior to extracting the column.
 * The first OP_Column against a pseudo-table after the value of the content
 * register has changed should have this bit set.
 *
 * If the OPFLAG_LENGTHARG and OPFLAG_TYPEOFARG bits are set on P5 when
 * the result is guaranteed to only be used as the argument of a length()
 * or typeof() function, respectively.  The loading of large blobs can be
 * skipped for length() and all content loading can be skipped for typeof().
 */
int vdbe_op_column(Vdbe *p, Op *pOp, Mem *aMem)
{
	int p2 = pOp->p2;	   /* column number to retrieve */
	VdbeCursor *pC = p->apCsr[pOp->p1]; /* The VDBE cursor */
	BtCursor *pCrsr = NULL; /* The BTree cursor */
	Mem *pDest;        /* Where to write the extracted value */
	Mem *pReg;         /* PseudoTable input register */

	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	pDest = vdbe_prepare_null_out(p, pOp->p3);
	assert(pOp->p1 >= 0 && pOp->p1 < p->nCursor);
	assert(pC != 0);
	assert(p2 < pC->nField);
	assert(pC->eCurType != CURTYPE_PSEUDO || pC->nullRow);
	assert(pC->eCurType != CURTYPE_SORTER);

	if (pC->cacheStatus != p->cacheCtr) {                /*OPTIMIZATION-IF-FALSE*/
		if (pC->nullRow) {
			if (pC->eCurType == CURTYPE_PSEUDO) {
				assert(pC->uc.pseudoTableReg > 0);
				pReg = &aMem[pC->uc.pseudoTableReg];
				assert(mem_is_bin(pReg));
				assert(memIsValid(pReg));
				vdbe_field_ref_prepare_data(&pC->field_ref,
							    pReg->z, pReg->n);
			} else {
				goto op_column_out;
			}
		} else {
			pCrsr = pC->uc.pCursor;
			assert(pC->eCurType == CURTYPE_TARANTOOL);
			assert(pCrsr);
			assert(sqlCursorIsValid(pCrsr));
			assert(pCrsr->curFlags & BTCF_TaCursor ||
			       pCrsr->curFlags & BTCF_TEphemCursor);
			vdbe_field_ref_prepare_tuple(&pC->field_ref,
						     pCrsr->last_tuple);
		}
		pC->cacheStatus = p->cacheCtr;
	}
	assert(pC->eCurType == CURTYPE_TARANTOOL ||
	       pC->eCurType == CURTYPE_PSEUDO);
	struct Mem *default_val_mem =
		pOp->p4type == P4_MEM ? pOp->p4.pMem : NULL;
	if (vdbe_field_ref_fetch(&pC->field_ref, p2, pDest) != 0)
		return -1;

	if (mem_is_null(pDest) &&
	    (uint32_t) p2  >= pC->field_ref.field_count &&
	    default_val_mem != NULL) {
		mem_copy_as_ephemeral(pDest, default_val_mem);
	}
	if (pDest->type == MEM_TYPE_NULL)
		goto op_column_out;
	enum field_type field_type = field_type_MAX;
	/* Currently PSEUDO cursor does not have info about field types. */
	if (pC->eCurType == CURTYPE_TARANTOOL)
		field_type = pC->uc.pCursor->space->def->fields[p2].type;
	if (field_type == FIELD_TYPE_ANY)
		pDest->flags |= MEM_Any;
	else if (field_type == FIELD_TYPE_SCALAR)
		pDest->flags |= MEM_Scalar;
	else if (field_type == FIELD_TYPE_NUMBER)
		pDest->flags |= MEM_Number;
op_column_out:
	REGISTER_TRACE(p, pOp->p3, pDest);
	return 0;
}

/* Opcode: RowData P1 P2 * * P5
 * Synopsis: r[P2]=data
 *
 * Write into register P2 the complete row content for the row at
 * which cursor P1 is currently pointing.
 * There is no interpretation of the data.
 * It is just copied onto the P2 register exactly as
 * it is found in the database file.
 * P5 can be used in debug mode to check if xferOptimization has
 * actually started processing.
 *
 * If cursor P1 is an index, then the content is the key of the row.
 * If cursor P2 is a table, then the content extracted is the data.
 *
 * If the P1 cursor must be pointing to a valid row (not a NULL row)
 * of a real table, not a pseudo-table.
 */
int SQL_PRESERVE_NONE vdbe_op_rowdata(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	VdbeCursor *pC;
	BtCursor *pCrsr;
	u32 n;

/*
 * Flag P5 is cleared after the first insertion using xfer
 * optimization.
 */
#ifdef SQL_TEST
	if ((pOp->p5 & OPFLAG_XFER_OPT) != 0) {
		pOp->p5 &= ~OPFLAG_XFER_OPT;
		sql_xfer_count++;
	}
#endif

	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);

	assert(pOp->p1>=0 && pOp->p1<p->nCursor);
	pC = p->apCsr[pOp->p1];
	assert(pC != 0);
	assert(pC->eCurType == CURTYPE_TARANTOOL);
	assert(pC->eCurType != CURTYPE_SORTER);
	assert(pC->nullRow == 0);
	assert(pC->uc.pCursor != 0);
	pCrsr = pC->uc.pCursor;

	/* The OP_RowData opcodes always follow
	 * OP_Rewind/Op_Next with no intervening instructions
	 * that might invalidate the cursor.
	 * If this where not the case, on of the following assert()s
	 * would fail.
	 */
	assert(sqlCursorIsValid(pCrsr));
	assert(pCrsr->eState == CURSOR_VALID);
	assert(pCrsr->curFlags & BTCF_TaCursor ||
	       pCrsr->curFlags & BTCF_TEphemCursor);
	tarantoolsqlPayloadFetch(pCrsr, &n);
	if (n > SQL_MAX_LENGTH) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob too big");
		return -1;
	}

	char *buf = xregion_alloc(&fiber()->gc, n);
	sqlCursorPayload(pCrsr, 0, n, buf);
	mem_set_bin_ephemeral(pOut, buf, n);
	assert(sqlVdbeCheckMemInvariants(pOut));
	UPDATE_MAX_BLOBSIZE(pOut);
	REGISTER_TRACE(p, pOp->p2, pOut);
	return 0;
}
