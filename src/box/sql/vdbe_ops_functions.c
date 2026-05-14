/* Function and Session opcode handlers extracted from vdbe.c
 *
 * Implements:
 * - OP_BuiltinFunction: Call built-in SQL function
 * - OP_FunctionByName: Call user-defined function by name
 * - OP_SetSession: Set session variable/setting
 */

#include "sqlInt.h"
#include "vdbeInt.h"
#include "mem.h"
#include "vdbe_ops.h"
#include "vdbe_debug.h"
#include "box/session.h"
#include "box/port.h"
#include "box/session_settings.h"
#include "box/func.h"
#include "box/func_cache.h"

static inline bool
vdbe_is_ascii_prefix(const char *str, uint64_t limit)
{
	for (uint64_t i = 0; i < limit; ++i) {
		if (((unsigned char)str[i] & 0x80) != 0)
			return false;
	}
	return true;
}

/* Opcode: BuiltinFunction P1 P2 P3 P4 *
 * Synopsis: r[P3]=builtin_func(r[P2@P1])
 *
 * Invoke a built-in SQL function (P4 is a pointer to a function context)
 * with P1 arguments taken from register P2 and successors.
 * The result of the function is stored in register P3.
 */
int
vdbe_op_builtinfunction(Vdbe *p, Op *pOp, Mem *aMem)
{
	int argc = pOp->p1;
	int P2 = pOp->p2;
	int P3 = pOp->p3;
	sql_context *pCtx;

	assert(pOp->p4type == P4_FUNCCTX);
	pCtx = pOp->p4.pCtx;

	Mem *pOut = vdbe_prepare_null_out(p, P3);
	if (pCtx->pOut != pOut)
		pCtx->pOut = pOut;

#ifdef SQL_DEBUG
	for (int i = 0; i < argc; i++) {
		assert(memIsValid(&aMem[P2 + i]));
		REGISTER_TRACE(p, P2 + i, &aMem[P2 + i]);
	}
#endif
	pCtx->is_aborted = false;
	assert(pCtx->func->def->language == FUNC_LANGUAGE_SQL_BUILTIN);
	struct func_sql_builtin *func = (struct func_sql_builtin *)pCtx->func;
	func->call(pCtx, argc, &aMem[P2]);

	/* If the function returned an error, signal failure */
	if (pCtx->is_aborted)
		return -1;

	/* Copy the result of the function into register P3 */
	if (mem_is_bytes(pOut)) {
		if (sqlVdbeMemTooBig(pCtx->pOut))
			return -1;
	}

	REGISTER_TRACE(p, P3, pCtx->pOut);
	UPDATE_MAX_BLOBSIZE(pCtx->pOut);
	return 0;
}

int
vdbe_op_builtinfunction_substr3_string_fast(Vdbe *p, Op *pOp, Mem *aMem)
{
	assert(pOp->p4type == P4_FUNCCTX);
	if (pOp->p1 != 3)
		return vdbe_op_builtinfunction(p, pOp, aMem);

	sql_context *pCtx = pOp->p4.pCtx;
	Mem *args = &aMem[pOp->p2];
	Mem *pOut = vdbe_prepare_null_out(p, pOp->p3);
	if (pCtx->pOut != pOut)
		pCtx->pOut = pOut;

#ifdef SQL_DEBUG
	for (int i = 0; i < pOp->p1; i++) {
		assert(memIsValid(&aMem[pOp->p2 + i]));
		REGISTER_TRACE(p, pOp->p2 + i, &aMem[pOp->p2 + i]);
	}
#endif

	pCtx->is_aborted = false;
	if (mem_is_any_null(&args[0], &args[1]) || mem_is_null(&args[2]))
		goto done;
	if (!mem_is_str(&args[0]) || !mem_is_uint(&args[1]) ||
	    !mem_is_uint(&args[2]) || args[1].u.u == 0 ||
	    pOut == &args[0] || args[0].n > SQL_MAX_LENGTH)
		return vdbe_op_builtinfunction(p, pOp, aMem);

	uint64_t start = args[1].u.u - 1;
	uint64_t length = args[2].u.u;
	if (length == 0 || start >= (uint64_t)args[0].n) {
		mem_set_str_static(pOut, "", 0);
		goto done;
	}
	uint64_t inspect = start + length;
	if (inspect > (uint64_t)args[0].n)
		inspect = args[0].n;
	if (!vdbe_is_ascii_prefix(args[0].z, inspect))
		return vdbe_op_builtinfunction(p, pOp, aMem);

	uint64_t len = (uint64_t)args[0].n - start;
	if (len > length)
		len = length;
	mem_set_str_ephemeral(pOut, args[0].z + start, len);

done:
	if (mem_is_bytes(pOut) && sqlVdbeMemTooBig(pOut))
		return -1;
	REGISTER_TRACE(p, pOp->p3, pOut);
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}

/* Opcode: FunctionByName P1 P2 P3 P4 *
 * Synopsis: r[P3]=func(r[P2@P1])
 *
 * Invoke a user function (P4 is a string name of the function)
 * with P1 arguments taken from register P2 and successors.
 * The result of the function is stored in register P3.
 */
int
vdbe_op_functionbyname(Vdbe *p, Op *pOp, Mem *aMem)
{
	int argc = pOp->p1;
	int P2 = pOp->p2;
	int P3 = pOp->p3;

	assert(pOp->p4type == P4_DYNAMIC);
	struct func *func = func_by_name(pOp->p4.z, strlen(pOp->p4.z));
	if (unlikely(func == NULL)) {
		diag_set(ClientError, ER_NO_SUCH_FUNCTION, pOp->p4.z);
		return -1;
	}

	/* Function call may yield so pointer to the function may
	 * turn out to be invalid after call.
	 */
	enum field_type returns = func->def->returns;
	struct Mem *argv = &aMem[P2];
	struct port args, ret;

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	port_vdbemem_create(&args, argv, argc);
	if (func_call(func, &args, &ret) != 0) {
		region_truncate(region, region_svp);
		return -1;
	}

	Mem *pOut = vdbe_prepare_null_out(p, P3);
	uint32_t size;
	struct Mem *mem = (struct Mem *)port_get_vdbemem(&ret, &size);
	port_destroy(&ret);
	if (mem == NULL) {
		region_truncate(region, region_svp);
		return -1;
	}
	assert(size == 1);
	mem_move(pOut, &mem[0]);
	assert(mem_is_null(&mem[0]) && mem_is_trivial(&mem[0]));
	region_truncate(region, region_svp);
	if (!mem_is_field_compatible(pOut, returns)) {
		diag_set(ClientError, ER_FUNC_INVALID_RETURN_TYPE, pOp->p4.z,
			 field_type_strs[returns],
			 mp_type_strs[mem_mp_type(pOut)]);
		return -1;
	}

	/* Copy the result of the function invocation into register P3. */
	if (mem_is_bytes(pOut))
		if (sqlVdbeMemTooBig(pOut))
			return -1;

	REGISTER_TRACE(p, P3, pOut);
	UPDATE_MAX_BLOBSIZE(pOut);
	return 0;
}

/* Opcode: SetSession P1 * * P4 *
 * Synopsis: Set session variable/setting
 *
 * Set a session setting. P1 is the register containing the value to set.
 * P4 is the setting name string. The setting is configured through the
 * session_settings array based on the type.
 */
int
vdbe_op_setsession(Vdbe *p, Op *pOp, Mem *aMem)
{
	(void)p;

	assert(pOp->p4type == P4_DYNAMIC);
	const char *setting_name = pOp->p4.z;
	int sid = session_setting_find(setting_name);
	if (sid < 0) {
		diag_set(ClientError, ER_NO_SUCH_SESSION_SETTING, setting_name);
		return -1;
	}

	Mem *pIn1 = &aMem[pOp->p1];
	struct session_setting *setting = &session_settings[sid];
	switch (setting->field_type) {
	case FIELD_TYPE_BOOLEAN: {
		if (!mem_is_bool(pIn1)) {
			diag_set(ClientError, ER_SESSION_SETTING_INVALID_VALUE,
				 session_setting_strs[sid],
				 field_type_strs[setting->field_type]);
			return -1;
		}
		bool value = pIn1->u.b;
		size_t size = mp_sizeof_bool(value);
		char *mp_value = (char *)static_alloc(size);
		if (mp_value == NULL) {
			diag_set(OutOfMemory, size, "static_alloc", "mp_value");
			return -1;
		}
		mp_encode_bool(mp_value, value);
		if (setting->set(sid, mp_value) != 0)
			return -1;
		break;
	}
	case FIELD_TYPE_STRING: {
		if (!mem_is_str(pIn1)) {
			diag_set(ClientError, ER_SESSION_SETTING_INVALID_VALUE,
				 session_setting_strs[sid],
				 field_type_strs[setting->field_type]);
			return -1;
		}
		const char *str = pIn1->z;
		uint32_t size = mp_sizeof_str(pIn1->n);
		char *mp_value = (char *)static_alloc(size);
		if (mp_value == NULL) {
			diag_set(OutOfMemory, size, "static_alloc", "mp_value");
			return -1;
		}
		mp_encode_str(mp_value, str, pIn1->n);
		if (setting->set(sid, mp_value) != 0)
			return -1;
		break;
	}
	default:
		diag_set(ClientError, ER_SESSION_SETTING_INVALID_VALUE,
			 session_setting_strs[sid],
			 field_type_strs[setting->field_type]);
		return -1;
	}
	p->nChange++;
	return 0;
}
