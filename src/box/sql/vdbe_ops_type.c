/* Type conversion opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_ops_cnp_impl.h"
#include "vdbe_debug.h"

/* Opcode: MustBeInt P1 P2 * * *
 *
 * Force the value in register P1 to be an integer.  If the value
 * in P1 is not an integer and cannot be converted into an integer
 * without data loss, then jump immediately to P2, or if P2==0
 * raise an ER_SQL_TYPE_MISMATCH error.
 */
int SQL_PRESERVE_NONE vdbe_op_mustbeint(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_mustbeint_impl(p, pOp, aMem);
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
int SQL_PRESERVE_NONE vdbe_op_cast(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_cast_impl(p, pOp, aMem);
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
int SQL_PRESERVE_NONE vdbe_op_applytype(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_applytype_impl(p, pOp, aMem);
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
int SQL_PRESERVE_NONE vdbe_op_makerecord(Vdbe *p, Op *pOp, Mem *aMem)
{
	return vdbe_op_makerecord_impl(p, pOp, aMem);
}
