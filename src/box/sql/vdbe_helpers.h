/*
 * This file is part of tarantool.
 *
 * Copyright (C) 2010-2024 Tarantool AUTHORS
 * Please see the AUTHORS file for the list of copyright holders.
 *
 * Tarantool is free software; you can redistribute it and/or modify
 * it under the terms of version 2.1 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * Tarantool is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Tarantool; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#ifndef VDBE_HELPERS_H
#define VDBE_HELPERS_H

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Forward declarations
 */
struct Vdbe;
struct Mem;
struct VdbeCursor;

/**
 * Helper function: sqlVdbeMemAboutToChange
 *
 * Marks that a register is about to be modified. This is used for
 * trace logging and register change tracking within the VDBE execution.
 *
 * When a register that was created by OP_SCopy is modified, the copies
 * that depend on it need to be marked as invalid.
 *
 * Arguments:
 *   pVdbe - The VDBE instance
 *   pMem  - The memory register about to be modified
 */
#ifdef SQL_DEBUG
void sqlVdbeMemAboutToChange(struct Vdbe *pVdbe, struct Mem *pMem);
#else
static inline void
sqlVdbeMemAboutToChange(struct Vdbe *pVdbe, struct Mem *pMem)
{
	(void)pVdbe;
	(void)pMem;
}
#endif

/**
 * Helper function: vdbe_prepare_null_out
 *
 * Pre-initialize an output register as NULL with the MEM_Cleared flag.
 * This is commonly used by opcodes that produce output but need to ensure
 * the register starts in a known state (cleared NULL).
 *
 * Used by:
 *   - OP_Sequence: Gets next sequence value
 *   - OP_Decimal: Decimal constant
 *   - And other opcodes that need cleared NULL output
 *
 * Arguments:
 *   v - The VDBE instance
 *   n - Register number (1-based, part of aMem array)
 *
 * Returns:
 *   Pointer to the initialized Mem register
 */
struct Mem *vdbe_prepare_null_out(struct Vdbe *v, int n);

/**
 * Helper function: allocateCursor
 *
 * Allocate a new VdbeCursor structure for the VDBE.
 * Each cursor maintains position information for table/index access.
 *
 * Arguments:
 *   p - The VDBE instance
 *   iCur - Cursor index to allocate
 *   nField - Number of fields in the table/index
 *   eCurType - Type of cursor (CURTYPE_TARANTOOL, CURTYPE_SORTER, etc.)
 *
 * Returns:
 *   Pointer to the allocated VdbeCursor, or NULL if allocation failed
 */
struct VdbeCursor * allocateCursor(struct Vdbe *p, int iCur, int nField,
	unsigned char eCurType);

#if defined(__cplusplus)
}
#endif

#endif /* VDBE_HELPERS_H */
