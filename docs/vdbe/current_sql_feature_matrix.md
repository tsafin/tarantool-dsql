# Current SQL And VDBE Feature Matrix

## Purpose

This document records the SQL semantic baseline that a new planner, physical
plan IR, or VDBE lowering path must preserve.

For **scheduling and milestone tracking**, see [`roadmap.md`](roadmap.md).
The roadmap's M0 milestone turns this matrix into an automated parity corpus
that gates every later phase.

The migration goal is not to add every feature named in the architecture
documents. It is initially to match the current implementation for the query
class being migrated. Unsupported current syntax does not become an MVP
requirement unless it is accepted as a separate product feature.

This inventory is based on:

- parser and planner/executor source under `src/box/sql`;
- existing `test/sql`, `test/sql-tap`, and `test/sql-luatest` coverage;
- focused probes against `build-jit-relwithdebinfo/src/tarantool` on
  June 15, 2026.

Status meanings:

- **Yes**: implemented and covered by source/tests or a focused probe;
- **Partial**: useful forms exist, but important forms or syntax are absent;
- **No**: parser rejection, explicit unsupported error, or no implementation;
- **Internal only**: VDBE mechanism exists but is not a public SQL operator.

## Important Distinction: SQL Semantics Versus Planner Operators

The current implementation supports `EXISTS`, `NOT EXISTS`, `IN`, and
`NOT IN`, including correlated forms. These provide semi-join and anti-join
semantics, but the planner does not expose explicit `SemiJoin` or `AntiJoin`
nodes and the SQL grammar does not accept `SEMI JOIN` or `ANTI JOIN`.

Likewise, recursive CTEs and compound queries are supported through specialized
VDBE generation, queues, ephemeral tables, sorters, and coroutines rather than
through general runtime operator nodes.

The new IR may represent these semantics more explicitly, but it is not
required to expose new SQL syntax.

## Query Features

| Feature | Current SQL support | Current implementation / limitation | Migration requirement |
| --- | --- | --- | --- |
| Basic `SELECT`, filter, projection | Yes | Lowered directly to VDBE expressions and loops | Preserve |
| `DISTINCT` | Yes | Uses ordering or ephemeral structures depending on plan | Preserve |
| `ORDER BY`, `LIMIT`, `OFFSET` | Yes | Specialized order-satisfaction planning and sorter/top-N paths | Preserve before replacing join search |
| Aggregate functions | Yes | Current aggregate VDBE paths | Preserve |
| `GROUP BY`, `HAVING` | Yes | Aggregate/sorter-based execution | Preserve |
| Aggregate `FILTER (WHERE ...)` | No | Focused probe is rejected by parser | Not an MVP requirement |
| Window functions and `OVER` | No | `row_number() OVER (...)` is rejected by parser; no window planner/executor subsystem found | Not an MVP requirement |
| Scalar subquery | Yes | `SRT_Mem`; correlated forms are supported | Preserve |
| Derived table / subquery in `FROM` | Yes | May flatten, materialize, or execute as coroutine | Preserve for migrated query classes |
| Correlated subquery | Yes | Outer references and repeated subquery evaluation are supported | Preserve; explicit correlation metadata required in new IR |
| `EXISTS` / `NOT EXISTS` | Yes | `SRT_Exists`; provides semi/anti semantics without explicit join node | Preserve semantics |
| `IN (SELECT ...)` / `NOT IN (SELECT ...)` | Yes | `SRT_Set`/ephemeral structures; includes SQL NULL semantics | Preserve semantics exactly |
| Explicit `SEMI JOIN` / `ANTI JOIN` syntax | No | No `JT_SEMI`/`JT_ANTI`; focused probes rejected/misparsed | Not an SQL MVP requirement; optional internal IR nodes |
| Non-recursive CTE | Yes | Parsed through `WITH`; CTE expansion/materialization uses current SELECT machinery | Preserve |
| Recursive CTE | Partial | Queue/FIFO VDBE execution supports `UNION [ALL]`, ordering, limit/offset, and substantial test coverage | Preserve supported subset |
| Recursive aggregate CTE | No | Explicit error: `Tarantool does not support recursive aggregate queries` | Preserve rejection |
| Multiple/indirect recursive references | Partial | Explicit restrictions and errors for multiple references and recursive references in subqueries | Preserve current acceptance/rejection behavior |
| `UNION` / `UNION ALL` | Yes | Compound SELECT paths, ephemeral tables, and coroutines | Preserve |
| `INTERSECT` / `EXCEPT` | Yes | Dedicated compound SELECT paths and ephemeral structures | Preserve |
| `VALUES` and multi-row `VALUES` | Yes | Used directly and as SELECT/recursive-CTE input | Preserve |

## Join Features

| Feature | Current SQL support | Current implementation / limitation | Migration requirement |
| --- | --- | --- | --- |
| Inner join | Yes | Nested-loop physical execution | Preserve |
| Cross/comma join | Yes | Nested-loop execution with ordering constraints | Preserve |
| `LEFT [OUTER] JOIN` | Yes | Specialized legality, ON/WHERE handling, and NULL extension | Preserve before enabling new join lowering |
| `NATURAL JOIN` and `USING` | Yes | Resolved into join predicates/column behavior before execution | Preserve |
| `RIGHT JOIN` | No | Explicit unsupported error | Not an MVP requirement |
| `FULL OUTER JOIN` | No | Explicit unsupported/error path | Not an MVP requirement |
| `LATERAL` syntax | No | Focused probe rejected; correlated subqueries remain available in other forms | Not an MVP requirement |
| Hash join | No | No current physical hash-join family | New optional feature, not parity |
| Merge join | No | No current physical merge-join family | New optional feature, not parity |
| Explicit semi/anti physical join | No | Semantics implemented through subqueries and ephemeral sets | Optional optimizer representation |

## DML, Schema, And Procedural Features

| Feature | Current SQL support | Current implementation / limitation | Migration requirement |
| --- | --- | --- | --- |
| `INSERT`, multi-row insert | Yes | Current VDBE DML path | Preserve |
| `INSERT ... SELECT` | Yes | SELECT destination and transfer optimization paths | Preserve |
| `UPDATE`, `DELETE` | Yes | Current VDBE DML paths | Preserve when DML enters migration scope |
| CTE-prefixed `INSERT` / `UPDATE` / `DELETE` | Yes | Grammar accepts `with(...)` before all three DML forms | Preserve when DML enters scope |
| DML `RETURNING` | No | Focused insert/update/delete probes rejected by parser | Not an MVP requirement |
| `UPDATE ... FROM` | No | Grammar has no `FROM` clause after update set-list | Not an MVP requirement |
| `DELETE ... USING` | No | Focused probe rejected by parser | Not an MVP requirement |
| Conflict algorithms / `REPLACE` | Yes | `OR ABORT/FAIL/IGNORE/REPLACE/ROLLBACK` and `REPLACE` paths | Preserve |
| PostgreSQL/SQLite UPSERT `ON CONFLICT ... DO UPDATE` | No | Current grammar supports constraint/conflict policies, not a general UPSERT clause | Not an MVP requirement |
| Row triggers | Yes | VDBE subprograms via `OP_Program`; recursive triggers are configurable | Preserve when DML enters scope |
| Statement triggers | No | Parser explicitly reports `FOR EACH STATEMENT triggers are not implemented` | Not an MVP requirement |
| Views | Yes | Stored SELECT and normal query expansion | Preserve |
| Primary/secondary/expression indexes | Yes | Access paths are central to current planner | Preserve |
| `CHECK` and foreign-key schema objects | Yes, with branch/runtime caveats | Syntax/catalog/test coverage exists; enforcement behavior must be baselined separately for each branch/build | Do not infer enforcement from syntax; add focused parity baseline |
| `memtx` and `vinyl` SQL tables | Yes | Both engines are selectable and tested | Preserve engine-specific behavior |

## Planner And Execution Capabilities

| Capability | Current status | Notes for migration |
| --- | --- | --- |
| Base table/index path enumeration | Yes | `WhereLoop` candidates include point/range/covering and related paths |
| Multi-table join ordering | Yes | Bounded `WherePath` dynamic-programming/beam-style solver |
| Join algorithm selection | No | Nested loops are the physical join family |
| ORDER BY-aware join search | Yes | Specialized `nOBSat`/`revMask`/order-satisfaction logic; easy to regress |
| Subquery flattening | Yes, rule-limited | Many semantic restrictions are encoded in `select.c` |
| Subquery coroutine execution | Yes | `SRT_Coroutine` and VDBE coroutine opcodes |
| Subquery/compound materialization | Yes | Ephemeral tables, sets, sorters, FIFO/queue destinations |
| Recursive CTE queue execution | Yes | `SRT_Fifo`, `SRT_DistFifo`, `SRT_Queue`, and `SRT_DistQueue` |
| Trigger subprogram execution | Yes | VDBE frames and `OP_Program` |
| Sort, distinct, and set-operation temporary storage | Yes | Sorters and ephemeral structures |
| `ANALYZE`, `_sql_stat1`, `_sql_stat4` | No, historical scaffolding only | `ANALYZE` is rejected, neither stats space exists in a fresh instance, all analyze TAP tests are disabled, `OP_LoadAnalysis` is a no-op, and prefix estimates use `default_tuple_est[]` |
| Explicit logical/physical operator IR | No | Planning remains coupled to SELECT/VDBE generation |
| Explicit property memo | No | Ordering is specialized rather than represented by a general memo/property system |
| Native distributed SQL planning | No | vshard is not a native SQL distributed planner |
| Native columnar execution | No | Row/MsgPack-oriented engines and VDBE |

## Baseline Decisions Before MVP Work

The roadmap's **M0 milestone** turns this matrix into a machine-readable
parity corpus. The captured layers per `(test × engine)`:

- **L1** result rows (sorted for comparability);
- **L2** diagnostic / error message;
- **L3** path_class (`current_where_c`, `new_planner`, or
  `fallback_<reason>`);
- **L6** VDBE opcode trace (forensic, captured on diff only, not gated);
- **L7** execution latency bands (perf trail, not gated).

Dispatcher (generated / CnP / LLVM) is a runtime parity check, not a stored
snapshot dimension — all three must produce identical L1+L2 for any
supported query, or the dispatcher-parity CI job fails.

L4 (access summary) and L5 (algorithm choice) are deferred until the
physical-plan descriptor exists naturally with the new planner (roadmap M3).
This is the B-light scope agreed in the roadmap.

The new planner/lowering path is required to match current behavior only for
the query classes it declares supported. For current unsupported features,
matching the existing rejection or visibly falling back is sufficient.

The semantic baseline is the authoritative VDBE behavior. Generated VDBE,
VDBE CnP, and VDBE LLVM are execution backends, not separate SQL dialects.
When a JIT backend cannot compile a supported opcode/shape, it must fall back
without changing SQL results, diagnostics, side effects, or ordering.

## Immediate Scope Consequences

- Windows, right/full joins, `LATERAL`, DML `RETURNING`, explicit semi/anti SQL
  syntax, hash join, and merge join should be removed from parity requirements.
- Recursive CTEs, correlated subqueries, `EXISTS`/`IN`, compound SELECT,
  left-join semantics, sorter/ephemeral execution, and VDBE coroutines are
  existing behavior and must not be treated as hypothetical future features.
- Explicit `SemiJoin`/`AntiJoin` nodes may still be useful in the new logical
  IR, but they lower existing SQL semantics rather than add new syntax.
- The historical `ANALYZE`/stat1/stat4 scaffolding must be audited for useful
  pieces before implementing normalized statistics infrastructure.

## Primary Evidence Pointers

| Area | Source / test evidence |
| --- | --- |
| Join syntax and right/full rejection | `src/box/sql/select.c` (`sqlJoinType()`), `test/sql-tap/join.test.lua` |
| Correlated scalar/`EXISTS`/`IN` subqueries | `src/box/sql/expr.c`, `src/box/sql/select.c` (`SRT_Mem`, `SRT_Exists`, `SRT_Set`), `test/sql-tap/tkt1473.test.lua`, `test/sql-tap/eqp.test.lua` |
| Recursive CTE execution and restrictions | `src/box/sql/select.c` (`generateWithRecursiveQuery()`), `src/box/sql/parse.y`, `test/sql-tap/with1.test.lua`, `test/sql-tap/with2.test.lua` |
| Compound queries | `src/box/sql/select.c` (`multiSelect*` paths), `test/sql-tap/selectB.test.lua` |
| CTE-prefixed DML and absence of `RETURNING`/`UPDATE FROM`/`DELETE USING` grammar | `src/box/sql/parse.y` |
| Trigger subprograms and row-only trigger restriction | `src/box/sql/trigger.c`, `src/box/sql/vdbe.c` (`OP_Program`), `test/sql-tap/trigger1.test.lua`, `test/sql-tap/triggerC.test.lua` |
| Statistics scaffolding/non-operation | `src/box/sql/vdbe.c`, `src/box/sql/vdbe_ops_inline_medium_8.c`, `src/box/sql.c` (`index_field_tuple_est()`), disabled `test/sql-tap/analyze*.test.lua`, `test/sql-tap/suite.ini` |
| Window/aggregate-filter absence | No corresponding parser/executor subsystem; focused `row_number() OVER (...)` and aggregate `FILTER` probes are rejected |
