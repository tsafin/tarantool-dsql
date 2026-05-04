# `EXPLAIN (...)` Bytecode And Disassembly

This note documents the SQL surface added for VDBE introspection.

## Syntax

The extended form uses PostgreSQL-style modifiers:

```sql
EXPLAIN (BYTECODE = YES) SELECT 1 + 2 + 3;
EXPLAIN (BYTECODE = YES, DISASSEMBLE = YES) SELECT 1 + 2 + 3;
```

Accepted boolean spellings:

- `TRUE` / `FALSE`
- `YES` / `NO`
- `ON` / `OFF`
- `1` / `0`

Bare `EXPLAIN ...` and `EXPLAIN QUERY PLAN ...` keep their existing behavior.

## Runtime Example

For the tiny statement:

```sql
EXPLAIN (BYTECODE = YES, DISASSEMBLE = YES) SELECT 1 + 2 + 3;
```

the result set uses three columns:

```text
metadata:
- name: section
  type: text
- name: addr
  type: integer
- name: detail
  type: text
```

The first rows contain the VDBE bytecode section:

```text
- ['bytecode', 0, 'Return           0 4 0  00']
- ['bytecode', 1, 'ElseNotEq        3 2 1  00']
- ['bytecode', 2, 'ResultRow        1 1 0  00 ; output=r[1]']
- ['bytecode', 3, 'Halt             0 0 0  00']
- ['bytecode', 4, 'Integer          1 4 0  00 ; r[4]=1']
- ['bytecode', 5, 'Integer          2 5 0  00 ; r[5]=2']
- ['bytecode', 6, 'ElseNotEq        5 4 2  00']
- ['bytecode', 7, 'Integer          3 3 0  00 ; r[3]=3']
- ['bytecode', 8, 'Gosub            0 1 0  00']
```

The following rows contain the native CnP disassembly. A short excerpt from
the current LLVM-backed in-process decoder:

```text
- ['disassembly', 0, 'movsxd   rax, dword ptr [r14 + 8]']
- ['disassembly', 4, 'lea      rcx, [rax + 2*rax]']
- ['disassembly', 8, 'lea      r14, [8*rcx]']
- ['disassembly', 16, 'add      r14, r13']
- ['disassembly', 19, 'movabs   rcx, 102694535504400']
- ['disassembly', 29, 'mov      rcx, qword ptr [rcx]']
- ['disassembly', 32, 'mov      rax, qword ptr [rcx + 8*rax]']
- ['disassembly', 36, 'jmp      rax']
- ['disassembly', 38, 'push     rax']
- ['disassembly', 39, 'mov      esi, dword ptr [r14 + 8]']
- ['disassembly', 43, 'movabs   rax, 102694508699024']
- ['disassembly', 207, 'L00cf:']
- ['disassembly', 207, 'jne      L00cf']
```

The direct repro script prints the same data as indented JSON, including
measurement counters:

```bash
cd /path/to/build
rm -f *.snap *.xlog
./src/tarantool ../tools/sql_explain_disassemble_repro.lua
```

On the current debug build, the combined example produced:

- `sql_cnp_compiled_bytes` delta: `1214`
- total row count: `290`
- disassembly text bytes: `8715`

The disassembly `addr` column is relative to the beginning of the generated
native code buffer, not an absolute runtime address. That keeps the listing
stable and readable while preserving instruction order and code span.

The offsets also stay within the generated code span:

- first disassembly offset: `0`
- last disassembly offset: `1212`
- code span: `1212`

That discrepancy is expected with the current LLVM-backed textual output: the
native program is a bit over one kilobyte, while the formatted disassembly
expands it into many more lines.

## Notes

- Use this form when debugging opcode lowering, control-flow stitching, and
  CnP/JIT bring-up.
- The native listing is a debugging aid, not a stable API.
- If you need to inspect the exact output mechanically, use
  `tools/sql_explain_disassemble_repro.lua`.
