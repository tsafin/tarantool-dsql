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
        "dispatch_prep": "LOAD_AOP();\n    pOp = &aOp[pOp->p2];",
        "dispatch_transfer": 'JMP_P2();',
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
        "name": "OP_Goto",
        "kind": "CNP_FRAG_JUMP_P2",
        "tail_fallthrough": False,
        "body": "",
        "dispatch_prep": "LOAD_AOP();\n    pOp = &aOp[pOp->p2];",
        "dispatch_transfer": 'JMP_P2();',
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
if (vdbe_op_applytype(p, pOp, aMem) < 0)
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
if (vdbe_op_aggstep(p, pOp, aMem) < 0)
    GOTO_ERROR();""",
        "dispatch_prep": "pOp += 1;",
        "dispatch_transfer": 'JMP_FALLTHROUGH();',
    },
    {
        "name": "OP_AggFinal",
        "kind": "CNP_FRAG_FALLTHROUGH",
        "tail_fallthrough": True,
        "body": """\
if (vdbe_op_aggfinal(p, pOp, aMem) < 0)
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
 * This is not a production dispatcher. Its job is to compile stitchable
 * opcode fragments with explicit begin/dispatch/end labels.
 */

#include <stddef.h>
#define VDBE_CNP_FRAGMENT_BUILD 1
#include "sqlInt.h"
#include "vdbeInt.h"
#include "vdbe.h"
#include "vdbe_debug.h"
#include "vdbe_ops.h"
#include "vdbe_ops_cnp_impl.h"

extern void **cnp_frag_get_dispatch_table(void);
extern char cnp_frag_base_label[];

__attribute__((preserve_none, noinline, used))
int64_t
cnp_fragment_entry(struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp, Mem *aMem);

typedef int64_t cnp_frag_exec_func_t(struct Vdbe *, VdbeOp *, VdbeOp *, Mem *)
    __attribute__((preserve_none));

__attribute__((preserve_none, naked, used))
int64_t
cnp_fragment_wrapper(struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp, Mem *aMem)
{
    /*
     * Bridge from the normal SysV call ABI used by vdbe_cnp_exec() to the
     * preserve_none live-in register convention used by the stitched
     * fragment entry and all extracted fragment bodies.
     */
    __asm__ volatile(
        "mov %rdi, %r12\\n\\t"
        "mov %rsi, %r13\\n\\t"
        "mov %rdx, %r14\\n\\t"
        "mov %rcx, %r15\\n\\t"
        "jmp cnp_fragment_entry\\n\\t");
}

__attribute__((preserve_none, noinline, used))
int64_t
cnp_fragment_entry(struct Vdbe *p, VdbeOp *aOp, VdbeOp *pOp, Mem *aMem)
{
"""

FOOTER = """\
cnp_fragment_base:
    __asm__ __volatile__(".globl cnp_frag_base_label\\n\\t"
                         "cnp_frag_base_label:");

    __asm__ volatile ("subq $16, %%rsp\\n\\t"
                      "mov %0, -16(%%rbp)\\n\\t"
                      "mov %1, -8(%%rbp)"
                      :: "r"(pOp), "r"(aOp) : "memory");

    void **dispatch_table = cnp_frag_get_dispatch_table();
    __asm__ volatile ("jmpq *%0"
                      :: "r"(dispatch_table[(size_t)(pOp - aOp)]));

#define JMP_FALLTHROUGH() do {                                 \\
    __asm__ volatile ("jmp cnp_frag_fallthrough_label");       \\
} while (0)

#define JMP_P2() do {                                          \\
    __asm__ volatile ("jmp cnp_frag_jump_p2_label");           \\
} while (0)

#define LOAD_POP() do {                                        \\
    __asm__ volatile ("mov -16(%%rbp), %0"                     \\
                      : "=r"(pOp) :: "memory");                \\
} while (0)

#define LOAD_AOP() do {                                        \\
    __asm__ volatile ("mov -8(%%rbp), %0"                      \\
                      : "=r"(aOp) :: "memory");                \\
} while (0)

#define STORE_POP() do {                                       \\
    __asm__ volatile ("mov %0, -16(%%rbp)"                     \\
                      :: "r"(pOp) : "memory");                 \\
} while (0)

#define JUMP_P2() do {                                         \\
    LOAD_AOP();                                                \\
    pOp = &aOp[pOp->p2];                                       \\
    STORE_POP();                                               \\
    JMP_P2();                                                  \\
} while (0)

#define GOTO_ERROR() do {                                      \\
    __asm__ volatile ("jmp cnp_frag_error_label");             \\
} while (0)

#define GOTO_ROW() do {                                        \\
    __asm__ volatile ("jmp cnp_frag_row_label");               \\
} while (0)

#define GOTO_DONE() do {                                       \\
    __asm__ volatile ("jmp cnp_frag_done_label");              \\
} while (0)

__LABELS__

    __asm__ __volatile__(".globl cnp_frag_fallthrough_label\\n\\t"
                         "cnp_frag_fallthrough_label:");
    __asm__ __volatile__(".globl cnp_frag_jump_p2_label\\n\\t"
                         "cnp_frag_jump_p2_label:");
    __asm__ __volatile__(".globl cnp_frag_error_label\\n\\t"
                         "cnp_frag_error_label:");
    __asm__ __volatile__(".globl cnp_frag_row_label\\n\\t"
                         "cnp_frag_row_label:");
    __asm__ __volatile__(".globl cnp_frag_done_label\\n\\t"
                         "cnp_frag_done_label:");

#undef GOTO_DONE
#undef GOTO_ROW
 #undef GOTO_ERROR
 #undef JMP_P2
 #undef JMP_FALLTHROUGH
 #undef JUMP_P2
 #undef STORE_POP
 #undef LOAD_POP
 #undef LOAD_AOP
    return 0;
}

"""


def generate(output_path: str) -> None:
    label_blocks = []
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
                '    __asm__ __volatile__("" : "+r"(pOp));\n'
            )
        label_blocks.append(
            f"""{frag["name"]}_begin:
    __asm__ __volatile__(".globl cnp_frag_sym_{frag["name"]}_begin\\n\\t"
                         "cnp_frag_sym_{frag["name"]}_begin:");
    LOAD_POP();
{body_block}{frag["name"]}_dispatch:
    __asm__ __volatile__(".globl cnp_frag_sym_{frag["name"]}_dispatch\\n\\t"
                         "cnp_frag_sym_{frag["name"]}_dispatch:");
{dispatch_prep_block}    STORE_POP();
{frag["name"]}_transfer:
    __asm__ __volatile__(".globl cnp_frag_sym_{frag["name"]}_transfer\\n\\t"
                         "cnp_frag_sym_{frag["name"]}_transfer:");
    {frag["dispatch_transfer"]}
{frag["name"]}_end:
    __asm__ __volatile__(".globl cnp_frag_sym_{frag["name"]}_end\\n\\t"
                         "cnp_frag_sym_{frag["name"]}_end:");
    __asm__ volatile("" ::: "memory");
"""
        )

    extern_lines = []
    for frag in PILOT_FRAGMENTS:
        for suffix in ("begin", "dispatch", "transfer", "end"):
            extern_lines.append(
                f"extern char cnp_frag_sym_{frag['name']}_{suffix}[];"
            )

    text = HEADER + "\n".join(extern_lines) + "\n"
    text += FOOTER.replace("__LABELS__", "\n".join(label_blocks))

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
