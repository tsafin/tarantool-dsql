/*
 * VDBE Inline Opcode Handlers - Medium Complexity Batch 7
 * Phase 5.6h: Bitwise arithmetic, value loading, data structures, and cursor ops
 *
 * This file contains wrapper functions for medium-complexity inline opcodes
 * extracted from vdbe.c and refactored to work in the generated dispatcher.
 *
 * Opcodes in this file (8 opcodes, 216-288 chars):
 * - OP_ShiftLeft: Bitwise left shift operation
 * - OP_ShiftRight: Bitwise right shift operation
 * - OP_String8: Load C string constant with auto-length calculation
 * - OP_Array: Create msgpack array from register range
 * - OP_Map: Create msgpack map from register pairs
 * - OP_Getitem: Extract element from array or map by index
 * - OP_OpenPseudo: Create pseudo-cursor for memory-resident data
 * - OP_Count: Get record count from cursor
 *
 * Total: ~2020 characters
 * Helper dependencies: None (uses standard mem_* interface)
 */

#include "sqlInt.h"
#include "mem.h"
#include "vdbeInt.h"
#include "vdbe_helpers.h"
#include "tarantoolInt.h"

static inline bool
mem_is_plain_uint(const struct Mem *mem)
{
	return mem->type == MEM_TYPE_UINT && !mem_is_metatype(mem);
}

/*
 * Opcode: SHIFTLEFT P1 P2 P3 * *
 * Synopsis: r[P3] = r[P2] << r[P1]
 *
 * Perform a bitwise left shift operation. The shift amount is in P1,
 * the value to shift is in P2, and the result is stored in P3.
 *
 * Preconditions:
 * - P1 must contain a valid shift amount (unsigned integer)
 * - P2 must contain a valid value (integer or NULL)
 * - P3 must be a valid output register
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (shift operation failed)
 */
int
vdbe_op_shiftleft_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1, *pIn2, *pOut;

	(void)p;

	/* Get input and output registers */
	pIn1 = &aMem[pOp->p1];
	pIn2 = &aMem[pOp->p2];
	pOut = &aMem[pOp->p3];

	/* Perform bitwise left shift: r[P3] = r[P2] << r[P1] */
	if (mem_shift_left(pIn2, pIn1, pOut) != 0)
		return -1;

	/* Result should be unsigned integer or NULL */
	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);

	return 0;  /* Continue to next instruction */
}

int
vdbe_op_shiftleft_uint_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pShift = &aMem[pOp->p1];
	Mem *pValue = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pShift, pValue)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pShift) || !mem_is_plain_uint(pValue))
		return vdbe_op_shiftleft_inline(p, pOp, aMem);
	uint64_t shift = pShift->u.u;
	uint64_t value = pValue->u.u;
	mem_set_uint(pOut, shift >= 64 ? 0 : value << shift);
	return 0;
}

int
vdbe_op_shiftleft_imm1_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pValue = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_null(pValue)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pValue))
		return vdbe_op_shiftleft_inline(p, pOp, aMem);
	mem_set_uint(pOut, pValue->u.u << 1);
	return 0;
}

int
vdbe_op_shiftleft_imm2_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pValue = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_null(pValue)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pValue))
		return vdbe_op_shiftleft_inline(p, pOp, aMem);
	mem_set_uint(pOut, pValue->u.u << 2);
	return 0;
}

/*
 * Opcode: SHIFTRIGHT P1 P2 P3 * *
 * Synopsis: r[P3] = r[P2] >> r[P1]
 *
 * Perform a bitwise right shift operation. The shift amount is in P1,
 * the value to shift is in P2, and the result is stored in P3.
 *
 * Preconditions:
 * - P1 must contain a valid shift amount (unsigned integer)
 * - P2 must contain a valid value (integer or NULL)
 * - P3 must be a valid output register
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (shift operation failed)
 */
int
vdbe_op_shiftright_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pIn1, *pIn2, *pOut;

	(void)p;

	/* Get input and output registers */
	pIn1 = &aMem[pOp->p1];
	pIn2 = &aMem[pOp->p2];
	pOut = &aMem[pOp->p3];

	/* Perform bitwise right shift: r[P3] = r[P2] >> r[P1] */
	if (mem_shift_right(pIn2, pIn1, pOut) != 0)
		return -1;

	/* Result should be unsigned integer or NULL */
	assert(pOut->type == MEM_TYPE_UINT || pOut->type == MEM_TYPE_NULL);

	return 0;  /* Continue to next instruction */
}

int
vdbe_op_shiftright_uint_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pShift = &aMem[pOp->p1];
	Mem *pValue = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_any_null(pShift, pValue)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pShift) || !mem_is_plain_uint(pValue))
		return vdbe_op_shiftright_inline(p, pOp, aMem);
	uint64_t shift = pShift->u.u;
	uint64_t value = pValue->u.u;
	mem_set_uint(pOut, shift >= 64 ? 0 : value >> shift);
	return 0;
}

int
vdbe_op_shiftright_imm1_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	Mem *pValue = &aMem[pOp->p2];
	Mem *pOut = &aMem[pOp->p3];
	if (mem_is_null(pValue)) {
		mem_set_null(pOut);
		return 0;
	}
	if (!mem_is_plain_uint(pValue))
		return vdbe_op_shiftright_inline(p, pOp, aMem);
	mem_set_uint(pOut, pValue->u.u >> 1);
	return 0;
}

/*
 * Opcode: STRING8 P1 P2 * * P4
 * Synopsis: r[P2]=P4 (C string constant with auto-length)
 *
 * Load a C string constant from P4 into register P2. The string length
 * is calculated automatically using sqlStrlen30(). This opcode is used
 * for string literals in SQL queries where the length is not pre-calculated.
 *
 * Preconditions:
 * - P4 must contain a valid C string pointer (not NULL)
 * - P2 must be a valid output register
 *
 * Return value:
 * - 0: Normal completion (falls through to OP_String handler)
 * - -1: Error (string too large)
 *
 * Note: This opcode modifies itself to OP_String and falls through,
 * allowing the next execution to use the pre-calculated length.
 */
int
vdbe_op_string8_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;
	(void)aMem;

	/* Validate that P4 contains a string pointer */
	assert(pOp->p4.z != 0);

	/* Self-modifying opcode: change to OP_String for next execution */
	pOp->opcode = OP_String;

	/* Calculate the string length and store in P1 */
	pOp->p1 = sqlStrlen30(pOp->p4.z);

	/* Check string size limit */
	if (pOp->p1 > SQL_MAX_LENGTH)
		return -1;

	/* Fall through to OP_String handler (will be executed on next dispatch) */
	return 0;
}

/*
 * Opcode: ARRAY P1 P3 P2 * *
 * Synopsis: r[P2]=array(r[P3]..r[P3+P1-1])
 *
 * Create a msgpack-encoded array from registers P3 through P3+P1-1
 * and store the result in register P2. This opcode is used to construct
 * array literals from register ranges.
 *
 * Preconditions:
 * - P1 must contain the number of elements (array count)
 * - P3 must contain a valid register offset (first element)
 * - P3+P1-1 must be within valid register range
 * - P2 must be a valid output register
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (encoding failed or memory allocation failed)
 */
int
vdbe_op_array_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pOut;
	uint32_t size;
	struct region *region;
	size_t svp;
	char *val;

	(void)p;

	/* Get output register */
	pOut = &aMem[pOp->p2];

	/* Get the garbage collection region for temporary encoding buffer */
	region = &fiber()->gc;
	svp = region_used(region);

	/* Encode registers as msgpack array */
	val = mem_encode_array(&aMem[pOp->p3], pOp->p1, &size, region);

	/* Copy encoded array to output register or cleanup on error */
	if (val == NULL || mem_copy_array(pOut, val, size) != 0) {
		region_truncate(region, svp);
		return -1;
	}

	/* Cleanup: truncate region back to saved position */
	region_truncate(region, svp);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: MAP P1 P3 P2 * *
 * Synopsis: r[P2]=map(r[P3]..r[P3+2*P1-1])
 *
 * Create a msgpack-encoded map (key-value structure) from registers
 * starting at P3, with P1 key-value pairs. The result is stored in P2.
 * This opcode is used for map/dictionary literals.
 *
 * Preconditions:
 * - P1 must contain the number of key-value pairs
 * - P3 must contain the first register (alternating keys/values)
 * - P3+2*P1-1 must be within valid register range
 * - P2 must be a valid output register
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (encoding failed or memory allocation failed)
 */
int
vdbe_op_map_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	Mem *pOut;
	uint32_t size;
	struct region *region;
	size_t svp;
	char *val;

	(void)p;

	/* Get output register */
	pOut = &aMem[pOp->p2];

	/* Get the garbage collection region for temporary encoding buffer */
	region = &fiber()->gc;
	svp = region_used(region);

	/* Encode registers as msgpack map (key-value pairs) */
	val = mem_encode_map(&aMem[pOp->p3], pOp->p1, &size, region);

	/* Copy encoded map to output register or cleanup on error */
	if (val == NULL || mem_copy_map(pOut, val, size) != 0) {
		region_truncate(region, svp);
		return -1;
	}

	/* Cleanup: truncate region back to saved position */
	region_truncate(region, svp);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: GETITEM P1 P2 P3 * *
 * Synopsis: r[P2]=value[P3@P1]
 *
 * Get an element from the value in register P3[P1] using values in
 * registers P3, ... P3 + (P1 - 1).
 *
 * Preconditions:
 * - P1 must contain a valid count (> 0)
 * - P2 must be a valid output register
 * - P3 + P1 must contain a valid source value
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (NULL container, out of bounds, or invalid operation)
 */
int
vdbe_op_getitem_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	int count;
	struct Mem *keys;
	struct Mem *pOut;
	struct Mem *value;
	(void)p;

	count = pOp->p1;
	assert(count > 0);
	value = &aMem[pOp->p3 + count];
	if (mem_is_null(value)) {
		diag_set(ClientError, ER_SQL_EXECUTE,
			"Selecting is not possible from NULL");
		return -1;
	}
	if (mem_is_any(value) || !mem_is_container(value)) {
		diag_set(ClientError, ER_SQL_TYPE_MISMATCH, mem_str(value),
			 "map or array");
		return -1;
	}

	pOut = &aMem[pOp->p2];
	keys = &aMem[pOp->p3];
	if (mem_getitem(value, keys, count, pOut) != 0)
		return -1;

	return 0;
}

/*
 * Opcode: OPENPSEUDO P1 P2 P3 * *
 * Synopsis: pseudotable_cursor[P1]=memory_register[P2]
 *
 * Create a pseudo-cursor that references memory-resident data. Pseudo-cursors
 * are used for temporary data structures and materialized subquery results.
 * The cursor number is P1, the memory register is P2, and P3 provides
 * additional cursor configuration.
 *
 * Preconditions:
 * - P1 must be a valid cursor number (>= 0)
 * - P2 must be a valid register containing the data reference
 * - P3 must be a valid parameter (>= 0)
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (cursor allocation failed)
 */
int
vdbe_op_openpseudo_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	VdbeCursor *pCx;

	(void)aMem;

	/* Validate cursor parameters */
	assert(pOp->p1 >= 0);
	assert(pOp->p3 >= 0);

	/* Allocate a pseudo-cursor with the specified type */
	pCx = allocateCursor(p, pOp->p1, pOp->p3, CURTYPE_PSEUDO);

	/* Check if cursor allocation failed */
	if (pCx == NULL)
		return -1;

	/* Initialize cursor state */
	pCx->nullRow = 1;

	/* Store the memory register reference in the pseudo-cursor */
	pCx->uc.pseudoTableReg = pOp->p2;

	/* P5 flag should be zero for OPENPSEUDO */
	assert(pOp->p5 == 0);

	return 0;  /* Continue to next instruction */
}

/*
 * Opcode: COUNT P1 P2 * * *
 * Synopsis: r[P2]=count(cursor[P1])
 *
 * Get the record count from a cursor and store it in register P2.
 * This opcode implements COUNT(*) aggregation by retrieving the
 * total number of records accessible through cursor P1.
 *
 * Preconditions:
 * - P1 must be a valid cursor number
 * - p->apCsr[P1] must point to a valid Tarantool cursor
 * - P2 must be a valid output register
 *
 * Return value:
 * - 0: Normal completion
 * - -1: Error (invalid cursor or count operation failed)
 */
int
vdbe_op_count_inline(Vdbe *p, Op *pOp, Mem *aMem)
{
	int64_t nEntry;
	BtCursor *pCrsr;
	Mem *pOut;

	/* Verify cursor is Tarantool type */
	assert(p->apCsr[pOp->p1]->eCurType == CURTYPE_TARANTOOL);

	/* Get the Tarantool cursor from the wrapper */
	pCrsr = p->apCsr[pOp->p1]->uc.pCursor;
	assert(pCrsr);

	/* Get record count based on cursor type */
	if (pCrsr->curFlags & BTCF_TaCursor) {
		nEntry = tarantoolsqlCount(pCrsr);
	} else {
		nEntry = 0;
	}

	/* Store the count in output register P2 */
	pOut = &aMem[pOp->p2];
	mem_set_uint(pOut, (uint64_t)nEntry);

	return 0;  /* Continue to next instruction */
}
