/* Helpers used by vdbe and extracted opcode handlers */
#include <stdio.h>
#include <string.h>

#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_debug.h"

/* Note: sqlVdbeMemAboutToChange and vdbe_prepare_null_out are defined in vdbe.c
 * to avoid duplicate definitions across multiple compilation units */

/*
 * Allocate VdbeCursor number iCur.  Return a pointer to it.  Return NULL
 * if we run out of memory.
 */
VdbeCursor *
allocateCursor(
	Vdbe *p,              /* The virtual machine */
	int iCur,             /* Index of the new VdbeCursor */
	int nField,           /* Number of fields in the table or index */
	u8 eCurType           /* Type of the new cursor */
	)
{
	/* Find the memory cell that will be used to store the blob of memory
	 * required for this VdbeCursor structure. It is convenient to use a
	 * vdbe memory cell to manage the memory allocation required for a
	 * VdbeCursor structure for the following reasons:
	 *
	 *   * Sometimes cursor numbers are used for a couple of different
	 *     purposes in a vdbe program. The different uses might require
	 *     different sized allocations. Memory cells provide growable
	 *     allocations.
	 *
	 * The memory cell for cursor 0 is aMem[0]. The rest are allocated from
	 * the top of the register space.  Cursor 1 is at Mem[p->nMem-1].
	 * Cursor 2 is at Mem[p->nMem-2]. And so forth.
	 */
	Mem *pMem = iCur>0 ? &p->aMem[p->nMem-iCur] : p->aMem;

	VdbeCursor *pCx = 0;
	int bt_offset = ROUND8(sizeof(VdbeCursor) + sizeof(uint32_t) * nField);
	int nByte = bt_offset +
		(eCurType==CURTYPE_TARANTOOL ? ROUND8(sizeof(BtCursor)) : 0);

	assert(iCur>=0 && iCur<p->nCursor);
	if (p->apCsr[iCur]) { /*OPTIMIZATION-IF-FALSE*/
		sqlVdbeFreeCursor(p->apCsr[iCur]);
		p->apCsr[iCur] = 0;
	}
	if (sqlVdbeMemClearAndResize(pMem, nByte) == 0) {
		p->apCsr[iCur] = pCx = (VdbeCursor*)pMem->z;
		memset(pCx, 0, offsetof(VdbeCursor,uc));
		pCx->eCurType = eCurType;
		pCx->nField = nField;
		vdbe_field_ref_create(&pCx->field_ref, nField);
		if (eCurType==CURTYPE_TARANTOOL) {
			pCx->uc.pCursor = (BtCursor*)&pMem->z[bt_offset];
			sqlCursorZero(pCx->uc.pCursor);
		}
	}
	return pCx;
}
