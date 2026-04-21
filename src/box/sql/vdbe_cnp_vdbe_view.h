/*
 * vdbe_cnp_vdbe_view.h — Minimal Vdbe view for CnP stencil stubs.
 *
 * Stencil stubs are compiled with minimal includes (no sqlInt.h chain).
 * They need access to a few Vdbe fields by offset.  This header defines
 * a "proxy" struct that mirrors exactly the Vdbe layout for those fields.
 *
 * Correctness is enforced at full-build time by _Static_assert in vdbe_cnp.c.
 *
 * DO NOT modify without also updating the _Static_assert checks.
 */

#ifndef VDBE_CNP_VDBE_VIEW_H
#define VDBE_CNP_VDBE_VIEW_H

#include <stddef.h> /* size_t */

/*
 * Mirrors struct Vdbe up through iCompare (vdbeInt.h lines 228-237).
 *
 *   offset  0: pPrev  (void *)
 *   offset  8: pNext  (void *)
 *   offset 16: pParse (void *)
 *   offset 24: nVar   (int)       -- ynVar = typedef int
 *   offset 28: magic  (unsigned)  -- u32
 *   offset 32: nMem   (int)
 *   offset 36: nCursor(int)
 *   offset 40: cacheCtr (unsigned) -- u32
 *   offset 44: pc     (int)
 *   offset 48: iCompare (int)
 */
struct CnpVdbeView {
	void *pPrev; /* offset  0 */
	void *pNext; /* offset  8 */
	void *pParse; /* offset 16 */
	int nVar; /* offset 24 */
	unsigned int magic; /* offset 28 */
	int nMem; /* offset 32 */
	int nCursor; /* offset 36 */
	unsigned int cacheCtr; /* offset 40 */
	int pc; /* offset 44 */
	int iCompare; /* offset 48 */
};

/* Accessors used by stencil stubs */
#define cnp_vdbe_pc(p) (((struct CnpVdbeView *)(p))->pc)
#define cnp_vdbe_icompare(p) (((struct CnpVdbeView *)(p))->iCompare)

#endif /* VDBE_CNP_VDBE_VIEW_H */
