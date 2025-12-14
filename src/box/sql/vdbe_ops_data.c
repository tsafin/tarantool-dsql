/* Data and constant opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"

/* Opcode: Integer P1 P2 * * *
 * Synopsis: r[P2]=P1
 *
 * The 32-bit integer value P1 is written into register P2.
 */
int vdbe_op_integer(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	mem_set_int(pOut, pOp->p1, pOp->p1 < 0);
	return 0;
}

/* Opcode: Bool P1 P2 * * *
 * Synopsis: r[P2]=P1
 *
 * The boolean value P1 is written into register P2.
 */
int vdbe_op_bool(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	assert(pOp->p1 == 0 || pOp->p1 == 1);
	mem_set_bool(pOut, pOp->p1);
	return 0;
}

/* Opcode: Int64 * P2 * P4 *
 * Synopsis: r[P2]=P4
 *
 * P4 is a pointer to a 64-bit integer value.
 * Write that value into register P2.
 */
int vdbe_op_int64(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	assert(pOp->p4.pI64 != NULL);
	mem_set_int(pOut, *pOp->p4.pI64, pOp->p4type == P4_INT64);
	return 0;
}

/* Opcode: Real * P2 * P4 *
 * Synopsis: r[P2]=P4
 *
 * P4 is a pointer to a 64-bit floating point value.
 * Write that value into register P2.
 */
int vdbe_op_real(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	assert(!sqlIsNaN(*pOp->p4.pReal));
	mem_set_double(pOut, *pOp->p4.pReal);
	return 0;
}

/* Opcode: String P1 P2 P3 P4 P5
 * Synopsis: r[P2]='P4' (len=P1)
 *
 * The string value P4 of length P1 (bytes) is stored in register P2.
 *
 * If P3 is not zero and the content of register P3 is equal to P5, then
 * the datatype of the register P2 is converted to BLOB.  The content is
 * the same sequence of bytes, it is merely interpreted as a BLOB instead
 * of a string, as if it had been CAST.  In other words:
 *
 * if (P3!=0 and reg[P3]==P5) reg[P2] := CAST(reg[P2] as BLOB)
 */
int vdbe_op_string(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(pOp->p4.z != NULL);
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	assert(strlen(pOp->p4.z) == (size_t)pOp->p1);
	mem_set_str0_static(pOut, pOp->p4.z);
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}

/* Opcode: Null P1 P2 P3 * *
 * Synopsis: r[P2..P3]=NULL
 *
 * Write a NULL into registers P2.  If P3 greater than P2, then also write
 * NULL into register P3 and every register in between P2 and P3.  If P3
 * is less than P2 (typically P3 is zero) then only register P2 is
 * set to NULL.
 *
 * If the P1 value is non-zero, then also set the MEM_Cleared flag so that
 * NULL values will not compare equal even if SQL_NULLEQ is set on
 * OP_Ne or OP_Eq.
 */
int vdbe_op_null(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	int cnt;
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	cnt = pOp->p3 - pOp->p2;
	assert(pOp->p3 <= (p->nMem + 1 - p->nCursor));
	if (pOp->p1 != 0)
		mem_set_null_clear(pOut);
	while (cnt > 0) {
		pOut++;
		memAboutToChange(p, pOut);
		if (pOp->p1 != 0)
			mem_set_null_clear(pOut);
		else
			mem_set_null(pOut);
		cnt--;
	}
	return 0;
}

/* Opcode: Blob P1 P2 P3 P4 *
 * Synopsis: r[P2]=P4 (len=P1, subtype=P3)
 *
 * P4 points to a blob of data P1 bytes long.  Store this
 * blob in register P2.  Set subtype to P3.
 */
int vdbe_op_blob(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	assert(pOp->p1 <= SQL_MAX_LENGTH);
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	if (pOp->p3 == 0) {
		mem_set_bin_static(pOut, pOp->p4.z, pOp->p1);
	} else {
		assert(pOp->p3 == SQL_SUBTYPE_MSGPACK);
		if (mp_typeof(*pOp->p4.z) == MP_MAP)
			mem_set_map_static(pOut, pOp->p4.z, pOp->p1);
		else
			mem_set_array_static(pOut, pOp->p4.z, pOp->p1);
	}
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}

/* Opcode: Variable P1 P2 * P4 *
 * Synopsis: r[P2]=parameter(P1,P4)
 *
 * Transfer the values of bound parameter P1 into register P2
 *
 * If the parameter is named, then its name appears in P4.
 * The P4 value is used by sql_bind_parameter_name().
 */
int vdbe_op_variable(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)aMem;
	Mem *pVar;
	assert(pOp->p1 > 0 && pOp->p1 <= p->nVar);
	assert(pOp->p4.z == 0 ||
	       pOp->p4.z == sqlVListNumToName(p->pVList, pOp->p1));
	pVar = &p->aVar[pOp->p1 - 1];
	if (sqlVdbeMemTooBig(pVar)) {
		return -1;
	}
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
	mem_copy_as_ephemeral(pOut, pVar);
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}

/* Opcode: Move P1 P2 P3 * *
 * Synopsis: r[P2@P3]=r[P1@P3]
 *
 * Move the P3 values in register P1..P1+P3-1 over into
 * registers P2..P2+P3-1.  Registers P1..P1+P3-1 are
 * left holding a NULL.  It is an error for register ranges
 * P1..P1+P3-1 and P2..P2+P3-1 to overlap.  It is an error
 * for P3 to be less than 1.
 */
int vdbe_op_move(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	int n = pOp->p3;
	int p1 = pOp->p1;
	int p2 = pOp->p2;

	assert(n > 0 && p1 > 0 && p2 > 0);
	assert(p1 + n <= p2 || p2 + n <= p1);

	Mem *pIn1 = &aMem[p1];
	Mem *pOut = &aMem[p2];
	do {
		assert(pOut <= &aMem[(p->nMem + 1 - p->nCursor)]);
		assert(pIn1 <= &aMem[(p->nMem + 1 - p->nCursor)]);
		assert(memIsValid(pIn1));
		memAboutToChange(p, pOut);
		mem_move(pOut, pIn1);
		REGISTER_TRACE(p, p2, pOut);
		p2++;
		pIn1++;
		pOut++;
	} while (--n);
	return 0;
}

/* Opcode: Copy P1 P2 P3 * *
 * Synopsis: r[P2@P3+1]=r[P1@P3+1]
 *
 * Make a copy of registers P1..P1+P3 into registers P2..P2+P3.
 *
 * This instruction makes a deep copy of the value.  A duplicate
 * is made of any string or blob constant.  See also OP_SCopy.
 */
int vdbe_op_copy(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	int n = pOp->p3;
	int p2 = pOp->p2;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pOut = &aMem[pOp->p2];
	assert(pOut != pIn1);
	while (true) {
		if (mem_copy(pOut, pIn1) != 0)
			return -1;
		REGISTER_TRACE(p, p2, pOut);
		if ((n--) == 0)
			break;
		pOut++;
		pIn1++;
		(void)p2++;
	}
	return 0;
}

/* Opcode: SCopy P1 P2 * * *
 * Synopsis: r[P2]=r[P1]
 *
 * Make a shallow copy of register P1 into register P2.
 *
 * This instruction makes a shallow copy of the value.  If the value
 * is a string or blob, then the copy is only a pointer to the
 * original and hence if the original changes so will the copy.
 * Worse, if the original is deallocated, the copy becomes invalid.
 * Thus the program must guarantee that the original will not change
 * during the lifetime of the copy.  Use OP_Copy to make a complete
 * copy.
 */
int vdbe_op_scopy(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	Mem *pOut = &aMem[pOp->p2];
	assert(pOut != pIn1);
	mem_copy_as_ephemeral(pOut, pIn1);
#ifdef SQL_DEBUG
	if (pOut->pScopyFrom == 0)
		pOut->pScopyFrom = pIn1;
#endif
	return 0;
}
