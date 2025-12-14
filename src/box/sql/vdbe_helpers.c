/* Helpers used by vdbe and extracted opcode handlers */
#include <stdio.h>
#include <string.h>

#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_debug.h"

#ifdef SQL_DEBUG

void
sqlVdbeMemAboutToChange(Vdbe * pVdbe, Mem * pMem)
{
    int i;
    Mem *pX;
    for (i = 0, pX = pVdbe->aMem; i < pVdbe->nMem; i++, pX++) {
        if (mem_is_bytes(pX) && !mem_is_ephemeral(pX) &&
            !mem_is_static(pX)) {
            if (pX->pScopyFrom == pMem) {
                mem_set_invalid(pX);
                pX->pScopyFrom = 0;
            }
        }
    }
    pMem->pScopyFrom = 0;
}

#endif /* SQL_DEBUG */

struct Mem *
vdbe_prepare_null_out(Vdbe *v, int n)
{
    assert(n > 0);
    assert(n <= (v->nMem + 1 - v->nCursor));
    struct Mem *out = &v->aMem[n];
    memAboutToChange(v, out);
    mem_set_null(out);
    return out;
}
