#ifndef SRC_BOX_SQL_VDBE_DEBUG_H
#define SRC_BOX_SQL_VDBE_DEBUG_H

#include "vdbeInt.h"

/* Prepare a register for writing (set it to NULL and mark about-to-change).
 * Available to external opcode handlers. */
struct Mem *vdbe_prepare_null_out(Vdbe *v, int n);

/* Debug helper to mark a Mem about to change (only active with SQL_DEBUG).
 * Exposed so handlers compiled in other translation units can call it. */
void sqlVdbeMemAboutToChange(Vdbe *pVdbe, Mem *pMem);

/* memAboutToChange macro for compatibility. */
#ifdef SQL_DEBUG
# define memAboutToChange(P,M) sqlVdbeMemAboutToChange(P,M)
# define REGISTER_TRACE(P,R,M) \
    do { if ((P)->sql_flags & SQL_VdbeTrace) \
        printf("REG[%d] = %s\n", (R), mem_str(M)); } while(0)
#else
# define memAboutToChange(P,M)
# define REGISTER_TRACE(P,R,M)
#endif

#endif /* SRC_BOX_SQL_VDBE_DEBUG_H */
