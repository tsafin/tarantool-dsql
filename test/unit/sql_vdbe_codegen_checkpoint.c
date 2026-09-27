#include "unit.h"

#include "box/box.h"
#include "box/sql.h"
#include "main.h"
#include "box/sql/sqlInt.h"
#include "box/sql/vdbeInt.h"

#include "coll/coll.h"

#include "core/diag.h"
#include "core/event.h"
#include "core/fiber.h"
#include "core/memory.h"

/* box.cc references these server entry-point globals; this unit target links
 * libbox without the server executable. */
char tarantool_path[PATH_MAX];
long tarantool_start_time;

sigint_cb_t
set_sigint_cb(sigint_cb_t new_sigint_cb)
{
	static sigint_cb_t sigint_cb;
	sigint_cb_t old_sigint_cb = sigint_cb;
	sigint_cb = new_sigint_cb;
	return old_sigint_cb;
}

int
main(void)
{
	memory_init();
	fiber_init(fiber_c_invoke);
	coll_init();
	event_init();
	box_init();
	sql_init();

	plan(10);
	header();
	struct Parse parse = {};
	struct Vdbe vdbe = {};
	vdbe.magic = VDBE_MAGIC_INIT;
	vdbe.pParse = &parse;
	int old_op = sqlVdbeAddOp0(&vdbe, OP_Noop);
	int old_label = sqlVdbeMakeLabel(&vdbe);
	assert(old_op == 0);
	assert(old_label < 0);
	struct vdbe_codegen_checkpoint checkpoint;
	ok(vdbe_codegen_checkpoint_init(&checkpoint, &vdbe) == 0,
	   "checkpoint initializes at VDBE construction time");

	parse.nMem = 7;
	parse.nTab = 3;
	parse.nRangeReg = 2;
	parse.iRangeReg = 5;
	parse.nTempReg = 1;
	parse.aTempReg[0] = 11;
	parse.nColCache = 1;
	parse.iCacheLevel = 9;
	parse.iCacheCnt = 13;
	parse.aColCache[0].iReg = 17;
	parse.nQueryLoop = 23;
	sqlVdbeResolveLabel(&vdbe, old_label);
	int new_label = sqlVdbeMakeLabel(&vdbe);
	sqlVdbeResolveLabel(&vdbe, new_label);
	char *owned_p4 = sql_xmalloc(sizeof("owned P4"));
	memcpy(owned_p4, "owned P4", sizeof("owned P4"));
	int dynamic_op = sqlVdbeAddOp4(&vdbe, OP_String8, 0, 1, 0,
				       owned_p4, P4_DYNAMIC);
	assert(dynamic_op == 1);
#ifdef SQL_ENABLE_EXPLAIN_COMMENTS
	sqlVdbeComment(&vdbe, "owned comment");
#endif
	parse.is_aborted = true;

	vdbe_codegen_checkpoint_rollback(&checkpoint);
	ok(vdbe.nOp == 1 && vdbe.aOp[0].opcode == OP_Noop,
	   "rollback drops only instructions emitted after the mark");
	ok(parse.nMem == 0 && parse.nTab == 0 && parse.nRangeReg == 0 &&
	   parse.iRangeReg == 0 && parse.nTempReg == 0 &&
	   parse.nColCache == 0 && parse.iCacheLevel == 0 &&
	   parse.iCacheCnt == 0 && parse.nQueryLoop == 0 &&
	   !parse.is_aborted && parse.nErr == 0 &&
	   diag_is_empty(diag_get()),
	   "rollback restores codegen counters and speculative abort state");
	ok(parse.nLabel == 1 && parse.aLabel[0] == -1,
	   "rollback restores prior label values and count");
	ok(vdbe.aOp[1].p4type == P4_NOTUSED &&
	   vdbe.aOp[1].p4.p == NULL,
	   "rollback clears the freed opcode slot and owned P4");

	ok(vdbe_codegen_checkpoint_init(&checkpoint, &vdbe) == 0,
	   "checkpoint can be reused after rollback");
	char *committed_p4 = sql_xmalloc(sizeof("committed P4"));
	memcpy(committed_p4, "committed P4", sizeof("committed P4"));
	sqlVdbeAddOp4(&vdbe, OP_String8, 0, 1, 0, committed_p4, P4_DYNAMIC);
	vdbe_codegen_checkpoint_commit(&checkpoint);
	ok(vdbe.nOp == 2 && vdbe.aOp[1].p4type == P4_DYNAMIC,
	   "commit keeps emitted instructions and their owned P4");

	parse.is_aborted = true;
	vdbe_codegen_checkpoint_init(&checkpoint, &vdbe);
	parse.is_aborted = false;
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	ok(parse.is_aborted && diag_is_empty(diag_get()),
	   "rollback preserves abort state that predates speculation");
	parse.is_aborted = false;
	vdbe_codegen_checkpoint_init(&checkpoint, &vdbe);
	parse.is_aborted = true;
	parse.nErr++;
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	ok(parse.is_aborted && parse.nErr == 1 && diag_is_empty(diag_get()),
	   "rollback preserves parse errors instead of enabling fallback");
	parse.is_aborted = false;
	parse.nErr = 0;

	diag_set(ClientError, ER_SQL_EXECUTE, "pre-existing diagnostic");
	struct error *baseline_error = diag_last_error(diag_get());
	vdbe_codegen_checkpoint_init(&checkpoint, &vdbe);
	parse.is_aborted = true;
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	ok(!parse.is_aborted && diag_last_error(diag_get()) == baseline_error,
	   "unchanged pre-existing diagnostic is not mistaken for a new failure");
	diag_clear(diag_get());

	vdbe_codegen_checkpoint_init(&checkpoint, &vdbe);
	parse.is_aborted = true;
	diag_set(ClientError, ER_SQL_EXECUTE, "speculative codegen failure");
	vdbe_codegen_checkpoint_rollback(&checkpoint);
	ok(parse.is_aborted && !diag_is_empty(diag_get()),
	   "rollback preserves a speculative diagnostic as a hard failure");
	diag_clear(diag_get());
	parse.is_aborted = false;
	/* The test owns this synthetic VDBE; release its retained opcode payloads. */
	for (int i = 0; i < vdbe.nOp; ++i) {
		if (vdbe.aOp[i].p4type == P4_DYNAMIC)
			sql_xfree(vdbe.aOp[i].p4.p);
	}
	sql_xfree(vdbe.aOp);
	sql_xfree(parse.aLabel);
	footer();
	int rc = check_plan();
	box_free();
	event_free();
	coll_free();
	fiber_free();
	memory_free();
	return rc;
}
