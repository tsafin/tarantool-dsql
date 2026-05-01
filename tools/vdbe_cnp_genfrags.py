#!/usr/bin/env python3
"""
Generate a dedicated threaded-fragment production source for CnP research.

This is intentionally separate from the shipping wrapper-stencil pipeline.
The generated source compiles a small pilot subset of the threaded dispatcher
into one function with addressable begin/dispatch/end labels so that a future
extractor can recover stitchable opcode fragments without function wrappers.
"""

import argparse
import os


PILOT_FRAGMENTS = [
    {
        "name": "OP_Integer",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_integer_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Bool",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_bool_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Int64",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_int64_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Init",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": "",
        "dispatch_prep": "pOp = &aOp[pOp->p2];",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Add",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_add_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Subtract",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_sub_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Multiply",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_multiply_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Divide",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_divide_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Remainder",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_remainder_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_BitAnd",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_bitand_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_BitOr",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_bitor_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_BitNot",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_bitnot_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_ShiftLeft",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_shiftleft_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_ShiftRight",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_shiftright_impl(p, pOp, aMem))
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Goto",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": "",
        "dispatch_prep": "pOp = &aOp[pOp->p2];",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_IfNot",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_ifnot_inline(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Once",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_once_inline(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_IsNull",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_isnull_inline(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Halt",
        "kind": "CNP_FRAG_TERMINAL",
        "tail_fallthrough": False,
        "body": "",
        "dispatch_prep": "",
        "dispatch_transfer": 'GOTO_DONE();',
    },
    {
        "name": "OP_ResultRow",
        "kind": "CNP_FRAG_ROW",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_resultrow(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        GOTO_ROW();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Null",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_null(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Variable",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_variable(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Copy",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_copy(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SCopy",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_scopy(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_AddImm",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_addimm_inline(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_TTransaction",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_ttransaction_inline(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_IteratorOpen",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_iteratoropen(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_NullRow",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_nullrow_inline(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Column",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_column(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_ApplyType",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_applytype_impl(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_OpenSpace",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_openspace_inline(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SkipLoad",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_skipload_inline(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_AggStep",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_aggstep_impl(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_AggFinal",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_aggfinal_impl(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_BuiltinFunction",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_builtinfunction(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_DecrJumpZero",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": """\
{
    int handler_rc = vdbe_op_decrjumpzero_inline(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SetDiag",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": """\
{
    int handler_rc = vdbe_cnp_setdiag_handler(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Noop",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
if (vdbe_op_noop_inline(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Eq",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": """\
{
    int handler_rc = vdbe_op_eq_impl(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Ge",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": """\
{
    int handler_rc = vdbe_op_ge_impl(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_MustBeInt",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": """\
{
    int handler_rc = vdbe_op_mustbeint_sysv_bridge(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_MakeRecord",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
if (vdbe_op_makerecord_sysv_bridge(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_InitCoroutine",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
{
    p->pc = (int)(pOp - aOp);
    int target_pc = vdbe_op_initcoroutine_jit(p, pOp, aMem);
    if (target_pc < 0)
        GOTO_ERROR();
    JUMP_PC(target_pc);
}""",
        "dispatch_prep": "",
        "dispatch_transfer": '',
    },
    {
        "name": "OP_Yield",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
{
    p->pc = (int)(pOp - aOp);
    int target_pc = vdbe_op_yield_jit(p, pOp, aMem);
    if (target_pc < 0)
        GOTO_ERROR();
    JUMP_PC(target_pc);
}""",
        "dispatch_prep": "",
        "dispatch_transfer": '',
    },
    {
        "name": "OP_EndCoroutine",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
{
    int target_pc = vdbe_op_endcoroutine_jit(p, pOp, aMem);
    if (target_pc < 0)
        GOTO_ERROR();
    JUMP_PC(target_pc);
}""",
        "dispatch_prep": "",
        "dispatch_transfer": '',
    },
    {
        "name": "OP_SorterOpen",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
if (vdbe_op_sorteropen(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_OpenPseudo",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
if (vdbe_op_openpseudo_inline(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SorterInsert",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
if (vdbe_op_sorterinsert(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SorterData",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": False,
        "body": """\
if (vdbe_op_sorterdata(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SorterSort",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": """\
{
    int handler_rc = vdbe_op_sortersort(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SorterNext",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": """\
{
    int handler_rc = vdbe_op_sorternext_jit(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Rewind",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_rewind(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SeekGE",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_seekge(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SeekLE",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_seekle(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SeekLT",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_seeklt(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_SeekGT",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_seekgt(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_IdxLE",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_idx_compare(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_IdxGT",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_idx_compare(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_IdxGE",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_idx_compare(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_IdxLT",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    int handler_rc = vdbe_op_idx_compare(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_Next",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": True,
        "body": """\
{
    /* vdbe_op_next_jit inverts the raw handler: rc=1 = more rows (jump),
     * rc=0 = exhausted (fall through). Matches JUMP_P2 convention. */
    int handler_rc = vdbe_op_next_jit(p, pOp, aMem);
    if (handler_rc < 0)
        GOTO_ERROR();
    if (handler_rc == 1)
        JUMP_P2();
}""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
]


HEADER = """\
/*
 * Auto-generated by tools/vdbe_cnp_genfrags.py — DO NOT EDIT
 *
 * Dedicated threaded fragment source for Copy-and-Patch extraction research.
 * Each opcode is compiled as a standalone preserve_none function so the
 * runtime can chain fragments with musttail tail calls.
 */

#include <stddef.h>
#define VDBE_CNP_FRAGMENT_BUILD 1
#include "sqlInt.h"
#include "vdbeInt.h"
#include "vdbe.h"
#include "vdbe_debug.h"
#include "vdbe_ops.h"
#include "vdbe_ops_cnp_impl.h"

typedef int64_t (*cnp_frag_exec_func_t)(struct Vdbe *, VdbeOp *, VdbeOp *, Mem *)
    __attribute__((preserve_none));

extern void **cnp_frag_dispatch_table;

#define JMP_FALLTHROUGH() do {                                 \\
    __attribute__((musttail)) return ((cnp_frag_exec_func_t)   \\
        cnp_frag_dispatch_table[(size_t)(pOp - aOp)])          \\
        (p, aOp, pOp, aMem);                                   \\
} while (0)

#define JUMP_P2() do {                                         \\
    pOp = &aOp[pOp->p2];                                       \\
    __attribute__((musttail)) return ((cnp_frag_exec_func_t)   \\
        cnp_frag_dispatch_table[(size_t)(pOp - aOp)])          \\
        (p, aOp, pOp, aMem);                                   \\
} while (0)

#define JUMP_PC(target_pc) do {                                \\
    pOp = &aOp[(target_pc)];                                   \\
    __attribute__((musttail)) return ((cnp_frag_exec_func_t)   \\
        cnp_frag_dispatch_table[(size_t)(pOp - aOp)])          \\
        (p, aOp, pOp, aMem);                                   \\
} while (0)

#define GOTO_ERROR() return -1
#define GOTO_ROW() return SQL_ROW
#define GOTO_DONE() return SQL_DONE

"""


def generate(output_path: str) -> None:
    func_blocks = []
    for frag in PILOT_FRAGMENTS:
        body = frag["body"]
        body_block = ""
        if body:
            body_block = "    " + body.replace("\n", "\n    ") + "\n"
        dispatch_prep = frag["dispatch_prep"]
        dispatch_prep_block = ""
        if dispatch_prep:
            dispatch_prep_block = (
                "    " + dispatch_prep.replace("\n", "\n    ") + "\n"
            )
        func_blocks.append(
            f"""__attribute__((preserve_none, noinline, used))
int64_t
cnp_frag_sym_{frag["name"]}(struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp, Mem *aMem)
{{
{body_block}{dispatch_prep_block}    {frag["dispatch_transfer"]}
}}
"""
        )

    text = HEADER + "\n".join(func_blocks)

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(text)

    print(f"Generated {output_path} with {len(PILOT_FRAGMENTS)} fragment entries")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate a dedicated threaded fragment source for CnP"
    )
    parser.add_argument("--output", required=True, help="Output C file path")
    args = parser.parse_args()
    generate(args.output)


if __name__ == "__main__":
    main()
