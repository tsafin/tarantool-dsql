/* Move sql_max_blobsize and updateMaxBlobsize out of vdbe.c so they are
 * visible to other compilation units when SQL_TEST is enabled.
 */
#include "sqlInt.h"
#include "mem.h"

#ifdef SQL_TEST
size_t sql_max_blobsize = 0;

void
updateMaxBlobsize(Mem *p)
{
    if (mem_is_bytes(p) && p->n > sql_max_blobsize)
        sql_max_blobsize = p->n;
}
#endif
