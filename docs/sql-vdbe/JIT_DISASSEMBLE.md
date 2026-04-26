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

The following rows contain the native CnP disassembly. A short excerpt:

```text
- ['disassembly', 130727651835904, 'push   rbp']
- ['disassembly', 130727651835905, 'mov    rbp,rsp']
- ['disassembly', 130727651835908, 'push   r14']
- ['disassembly', 130727651835910, 'push   rbx']
- ['disassembly', 130727651835911, 'sub    rsp,0x100']
- ['disassembly', 130727651835918, 'movabs rax,0x57c35d497b6b']
- ['disassembly', 130727651835928, 'call   rax']
- ['disassembly', 130727651835930, 'jmp    rax']
- ['disassembly', 130727651835932, 'jmp    76e56740001e <vdbe_cnp_stmt_6584fd07_ops_9+0x1e>']
- ['disassembly', 130727651835934, 'jmp    76e567400020 <vdbe_cnp_stmt_6584fd07_ops_9+0x20>']
- ['disassembly', 130727651835936, 'call   76e5674002de <vdbe_cnp_stmt_6584fd07_ops_9+0x2de>']
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

The disassembly addresses also stay within the generated code span:

- first disassembly address: `130727651835904`
- last disassembly address: `130727651837116`
- address span: `1212`

That discrepancy is expected with the current `objdump`-based output: the
native program is a bit over one kilobyte, while the textual disassembly
expands it into many more lines.

## Notes

- Use this form when debugging opcode lowering, control-flow stitching, and
  CnP/JIT bring-up.
- The native listing is a debugging aid, not a stable API.
- If you need to inspect the exact output mechanically, use
  `tools/sql_explain_disassemble_repro.lua`.
