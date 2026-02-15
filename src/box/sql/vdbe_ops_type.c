/* Type conversion opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"

/* Opcode: MustBeInt P1 P2 * * *
 *
 * Force the value in register P1 to be an integer.  If the value
 * in P1 is not an integer and cannot be converted into an integer
 * without data loss, then jump immediately to P2, or if P2==0
 * raise an ER_SQL_TYPE_MISMATCH error.
 */
int vdbe_op_mustbeint(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	if (mem_to_int_precise(pIn1) != 0) {
		if (pOp->p2 != 0)
			return 1;  /* Jump to P2 */
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
			 mem_str(pIn1), "integer");
		return -1;  /* Error */
	}
	return 0;  /* Continue */
}

/* Opcode: Cast P1 P2 * * *
 * Synopsis: type(r[P1])
 *
 * Force the value in register P1 to be the type defined by P2.
 *
 * <ul>
 * <li value="97"> TEXT
 * <li value="98"> BLOB
 * <li value="99"> NUMERIC
 * <li value="100"> INTEGER
 * <li value="101"> REAL
 * </ul>
 *
 * A NULL value is not changed by this routine.  It remains NULL.
 */
int vdbe_op_cast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pIn1 = &aMem[pOp->p1];
	int rc = mem_cast_explicit(pIn1, pOp->p2);
	UPDATE_MAX_BLOBSIZE(pIn1);
	if (rc == 0)
		return 0;
	diag_set(ClientError, ER_SQL_TYPE_MISMATCH, mem_str(pIn1),
		 field_type_strs[pOp->p2]);
	return -1;
}

/* Opcode: ApplyType P1 P2 * P4 *
 * Synopsis: type(r[P1@P2])
 *
 * Check that types of P2 registers starting from register P1 are
 * compatible with given field types in P4. If the MEM_type of the
 * value and the given type are incompatible according to
 * field_mp_plain_type_is_compatible(), but both are numeric,
 * this opcode attempts to convert the value to the type.
 */
int vdbe_op_applytype(Vdbe *p, Op *pOp, Mem *aMem)
{
	enum field_type *types = pOp->p4.types;
	assert(types != NULL);
	Mem *pIn1 = &aMem[pOp->p1];
	for (int i = 0; i < pOp->p2; ++i, ++pIn1) {
		enum field_type type = types[i];
		assert(pIn1 <= &p->aMem[(p->nMem + 1 - p->nCursor)]);
		assert(memIsValid(pIn1));
		if (mem_cast_implicit(pIn1, type) != 0) {
			diag_set(ClientError, ER_SQL_TYPE_MISMATCH,
				 mem_str(pIn1), field_type_strs[type]);
			return -1;
		}
	}
	return 0;
}

/* Opcode: MakeRecord P1 P2 P3 * P5
 * Synopsis: r[P3]=mkrec(r[P1@P2])
 *
 * Convert P2 registers beginning with P1 into the [record format]
 * use as a data record in a database table or as a key
 * in an index.  The OP_Column opcode can decode the record later.
 *
 * If P5 is not NULL then record under construction is intended to be inserted
 * into ephemeral space. Thus, sort of memory optimization can be performed.
 */
int vdbe_op_makerecord(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pData0;           /* First field to be combined into the record */
	int nField;            /* Number of fields in the record */
	u8 bIsEphemeral;

	/* Assuming the record contains N fields, the record format looks
	 * like this:
	 *
	 * ------------------------------------------------------------------------
	 * | hdr-size | type 0 | type 1 | ... | type N-1 | data0 | ... | data N-1 |
	 * ------------------------------------------------------------------------
	 *
	 * Data(0) is taken from register P1.  Data(1) comes from register P1+1
	 * and so forth.
	 *
	 * Each type field is a varint representing the serial type of the
	 * corresponding data element. The hdr-size field is also a varint which
	 * is the offset from the beginning of the record to data0.
	 */
	nField = pOp->p1;
	bIsEphemeral = pOp->p5;
	assert(nField > 0 && pOp->p2 > 0 &&
	       pOp->p2 + nField <= (p->nMem + 1 - p->nCursor) + 1);
	pData0 = &aMem[nField];
	nField = pOp->p2;

	/* Identify the output register */
	assert(pOp->p3 < pOp->p1 || pOp->p3 >= pOp->p1 + pOp->p2);
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p3);

	/* Initialize any uninitialized registers to NULL before encoding.
	 * This handles bytecode patterns (e.g., CREATE TABLE) where not all
	 * fields are explicitly set but need to be encoded into a record. */
	for (int i = 0; i < nField; i++) {
		if (pData0[i].type == MEM_TYPE_INVALID) {
			mem_set_null(&pData0[i]);
		}
	}

	struct region *region = &fiber()->gc;
	size_t used = region_used(region);
	uint32_t tuple_size;
	char *tuple = mem_encode_array(pData0, nField, &tuple_size, region);
	if (tuple == NULL)
		return -1;
	if (tuple_size > SQL_MAX_LENGTH) {
		diag_set(ClientError, ER_SQL_EXECUTE, "string or blob too big");
		return -1;
	}

	/* In case of ephemeral space, it is possible to save some memory
	 * allocating one by ordinary malloc: instead of cutting pieces
	 * from region and waiting while they will be freed after
	 * statement commitment, it is better to reuse the same chunk.
	 * Such optimization is prohibited for ordinary spaces, since
	 * memory shouldn't be reused until it is written into WAL.
	 *
	 * However, if memory for ephemeral space is allocated
	 * on region, it will be freed only in sql_stmt_finalize()
	 * routine.
	 */
	if (bIsEphemeral) {
		if (mem_copy_bin(pOut, tuple, tuple_size) != 0)
			return -1;
		region_truncate(region, used);
	} else {
		/* Allocate memory on the region for the tuple
		 * to be passed to Tarantool. Before that, make
		 * sure previously allocated memory has gone.
		 */
		mem_destroy(pOut);
		mem_set_bin_ephemeral(pOut, tuple, tuple_size);
	}
	assert(sqlVdbeCheckMemInvariants(pOut));
	assert(pOp->p3 > 0 && pOp->p3 <= (p->nMem + 1 - p->nCursor));
	REGISTER_TRACE(p, pOp->p3, pOut);
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}
