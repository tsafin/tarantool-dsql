/* Data and constant opcode handlers extracted from vdbe.c */
#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"

/* OP_Integer: r[P2] = P1 */
int vdbe_op_integer(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)aMem;
    Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
    mem_set_int(pOut, pOp->p1, pOp->p1 < 0);
    return 0;
}

/* OP_Bool: r[P2] = P1 (boolean) */
int vdbe_op_bool(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)aMem;
    Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
    assert(pOp->p1 == 0 || pOp->p1 == 1);
    mem_set_bool(pOut, pOp->p1);
    return 0;
}

/* OP_Int64: r[P2] = *pOp->p4.pI64 */
int vdbe_op_int64(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)aMem;
    Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
    assert(pOp->p4.pI64 != NULL);
    mem_set_int(pOut, *pOp->p4.pI64, pOp->p4type == P4_INT64);
    return 0;
}

/* OP_Real: r[P2] = *pOp->p4.pReal */
int vdbe_op_real(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)aMem;
    Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
    assert(!sqlIsNaN(*pOp->p4.pReal));
    mem_set_double(pOut, *pOp->p4.pReal);
    return 0;
}

/* OP_String: r[P2] = pOp->p4.z (len = pOp->p1) */
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

/* OP_Null: write NULL into registers P2..P3 (or only P2 if P3 < P2) */
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

/* OP_Blob: r[P2] = P4 (blob of length P1, subtype P3) */
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

/* OP_Variable: r[P2] = parameter(P1,P4) */
int vdbe_op_variable(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)aMem;
    Mem *pVar;
    assert(pOp->p1 > 0 && pOp->p1 <= p->nVar);
    assert(pOp->p4.z==0 || pOp->p4.z==sqlVListNumToName(p->pVList,pOp->p1));
    pVar = &p->aVar[pOp->p1 - 1];
    if (sqlVdbeMemTooBig(pVar)) {
        return -1;
    }
    Mem *pOut = vdbe_prepare_null_out(p, pOp->p2);
    mem_copy_as_ephemeral(pOut, pVar);
    UPDATE_MAX_BLOBSIZE(pOut);
    return 0;
}

/* OP_Move: r[P2@P3]=r[P1@P3] */
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
        assert(pOut<=&aMem[(p->nMem + 1 - p->nCursor)]);
        assert(pIn1<=&aMem[(p->nMem + 1 - p->nCursor)]);
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

/* OP_Copy: deep copy registers P1..P1+P3 into P2..P2+P3 */
int vdbe_op_copy(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p;
    int n = pOp->p3;
    int p2 = pOp->p2;
    Mem *pIn1 = &aMem[pOp->p1];
    Mem *pOut = &aMem[pOp->p2];
    assert(pOut!=pIn1);
    while (true) {
        if (mem_copy(pOut, pIn1) != 0)
            return -1;
	REGISTER_TRACE(p, p2, pOut);
        if ((n--)==0) break;
        pOut++;
        pIn1++;
	(void)p2++;
    }
    return 0;
}

/* OP_SCopy: shallow copy r[P1] into r[P2] */
int vdbe_op_scopy(Vdbe *p, Op *pOp, Mem *aMem)
{
    (void)p;
    Mem *pIn1 = &aMem[pOp->p1];
    Mem *pOut = &aMem[pOp->p2];
    assert(pOut!=pIn1);
    mem_copy_as_ephemeral(pOut, pIn1);
#ifdef SQL_DEBUG
    if (pOut->pScopyFrom==0) pOut->pScopyFrom = pIn1;
#endif
    return 0;
}
