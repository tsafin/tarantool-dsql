# Physical Plan Descriptor

## Expression normalization prerequisite

sql_expr_canonicalize() is an isolated M3 prerequisite prototype. It returns
an owned structural encoding for resolved columns, NULL/integer/finite-float/
string constants, and a fixed scalar operator set. It rejects function calls,
reduced/token-only nodes, flags outside its allowlist, and unknown operators.
Expr exposes function source tokens but does not by itself prove a stable
function identity or absence of side effects. The helper is not wired into
descriptor expression references, resolver routing, or lowering. Column
encoding requires a caller-supplied cursor-to-logical-relation ordinal map
and emits that ordinal, not Expr.iTable. Stability therefore holds only
under the same relation binding; this is not yet a replay or cross-statement
fingerprint.

## Status

`PROTOTYPE` — an internal immutable C descriptor API now validates and
deep-copies the single-table v1 shape. It is not connected to planner
resolution, VDBE lowering, MsgPack/YAML serialization, or fingerprinting;
those remain later M3 subtasks. Joins, aggregates, and subqueries extend the
schema in later versions.

M3.3 adds a separately testable physical selector: given a supported logical
chain and access candidates supplied by fixed/current estimates, it chooses
the least estimated total cost (stable ties by access kind then index ID) and
builds this descriptor. It handles primary/secondary point lookup, range,
index full scan, and table full scan. This remains an explicit candidate
interface, not SQL expression analysis or `where.c` routing; the current
logical IR does not consume the isolated `sql_expr_canonicalize()` helper.
Descriptor expression references therefore still lack stable normalized
identities, and this selector is not a detached normalized-input model for
M1 replay.

M3.4 adds `sql_plan_lower()`, an ordered callback contract over a descriptor:
scan, each residual filter, projection, each finalize operator (sort/limit),
then result. The unit test fixes ordering and callback error propagation. This
is not an executable bytecode builder and does not call Tarantool's VDBE APIs.
In particular, expression compilation, cursor allocation/opening, engine
specific seek loops, sorter setup/comparison, limit registers, and SQL result
delivery are not implemented. It is a boundary prototype only; parity and
production `where.c` routing remain mandatory before M3 can be considered
integrated.

#### M3.4 executable-lowering feasibility audit (2026-09-27)

No safe executable-lowering slice can currently be added as an isolated
consumer of the descriptor. The code-path boundary is concrete:

- `sql_select_record_fallback()` in `select.c` builds a logical plan only for
  classification and immediately deletes it; it does not build or retain a
  physical descriptor. `sql_plan_producer_result_init()` is only a result
  wrapper and has unit-test callers, not a production SELECT caller.
- `sql_physical_plan_from_logical()` requires its caller to supply a candidate
  array and candidate-owned expressions. Its repository callers are tests;
  there is no production access-candidate provider or mapping from resolved
  `Expr` trees to descriptor expression references.
- The actual SELECT bytecode path in `select.c` calls `sqlWhereBegin()`
  (ordinary SELECT loop), `selectInnerLoop()` (row/filter/projection/result
  work), and `sqlWhereEnd()`. `sqlWhereBegin()` in `where.c` owns legacy
  cursor/loop selection and emits code while preparing that path. The current
  callback lowerer has no VDBE, `Parse`, cursor, or result-destination
  context, and a callback failure after emission cannot be rolled back into
  `where.c` safely.

Therefore even a table-full-scan-only route needs a production producer that
constructs and validates a descriptor before any bytecode is emitted, a
statement-lifetime expression/register mapping, explicit engine cursor/open/
iteration semantics, and a transactional dispatch boundary that guarantees
fallback only before emission. Without these, claiming all-or-nothing
lowering or parity would be false. Keep the contract prototype, but keep
executable M3.4, successful new-planner routing, and the M3.7 feature flag
open. The next independently testable step is a VDBE builder contract with
preflight validation and explicit emission-failure semantics; live routing
must wait until the producer and expression bindings exist.

#### Smallest candidate and blocking VDBE primitive

The smallest plausible single-table candidate is `SELECT c FROM t` with no
predicate, ordering, or limit and an ordinary output destination. It would
need to produce one complete VDBE loop: open the base-space primary index,
rewind, load column `c`, return a row, advance, and halt. Existing equivalents
are split across three owners: `sqlWhereBegin()` in `where.c` opens the table
cursor via `vdbe_emit_open_cursor()` (`OP_OpenSpace` plus `OP_IteratorOpen`);
`wherecode.c` emits the full-scan `OP_Rewind`/`OP_Next` loop; and
`selectInnerLoop()` in `select.c` compiles the projection and
`OP_ResultRow`. The cursor number, result register range, loop labels, and
destination semantics come from `Parse`/`SrcList`/`WhereInfo`, not from the
current descriptor. The descriptor also has no binding from canonical
expression refs back to resolved `Expr` nodes, so even column `c` cannot yet
be compiled from descriptor contents.

The failure-safety primitive now exists as `vdbe_codegen_checkpoint`: it
captures the opcode boundary and the relevant `Parse` register/cursor,
label, expression-cache, and temporary-register state. Rollback frees owned
P4/comment payloads, clears the speculative opcode suffix, and restores the
captured parse state; a focused unit test exercises rollback and commit.
This checkpoint intentionally does not cover arbitrary parser/AST mutations,
schema side effects, or VDBE metadata, so it is not yet sufficient to wrap
the complete SELECT integration path. The producer/expression/cursor/result
bindings and an auditable boundary around every other mutable state remain
the narrow blockers. This is not a claim that existing VDBE opcodes cannot
express the scan.

M3.5 now has a producer-contract prototype in `sql_plan_fallback.{h,c}`.
It maps the existing logical and physical reject enums to append-only numeric
`sql_plan_fallback_reason` values and stable names, and returns an observable
`sql_plan_producer_result`: either a borrowed new-planner descriptor with
`path_class=new_planner`, or no descriptor with
`path_class=fallback` and a reason. Logical-shape rejection takes precedence
over physical candidate rejection. Unit tests cover every current mapping,
the external name, and success/fallback result construction.

M3.5's SQL producer path also walks the resolved expression tree before
flattening. Ordinary function expressions without the resolver's
`EP_ConstFunc` marker (from `func_def.is_deterministic`) are classified as
`UNSUPPORTED_NONDETERMINISTIC`; tests cover built-in `random()`, a
non-deterministic SQL UDF, deterministic `abs()`, and the reason counter.
This metadata has no separate side-effect bit and can miss argument-dependent
volatility, so it is not yet proof that every effectful expression is gated.
Explicit `INDEXED BY` and `NOT INDEXED` clauses are also rejected as
`UNSUPPORTED_ACCESS_HINT` until access constraints are represented in the
logical/physical IR.

This remains a producer prototype, not new-planner execution routing: no new
resolver caller consumes a descriptor and statements are not dispatched to a
new lowering path. Current `where.c` execution records supported scalar
volatility/structural fallback metadata and reason counters; M0 snapshot
capture and broader route coverage are tracked separately under M3.6. The
missing new-planner success path and complete fallback coverage keep M3.5 open.

The physical-reject mapping is not runtime fallback accounting. A repository
caller audit shows `sql_physical_plan_from_logical()` is called only by its
unit tests. `sql_plan_fallback_from_physical()` is called by the producer
contract helper, but that helper itself has only unit-test callers. Production
SQL does not currently construct the candidate array or call the selector.
Consequently there is no observed
physical reject to attach to the existing VDBE fallback counters, and adding
`NO_ACCESS_PATH` (or another physical reason) at the current legacy route
would mislabel a route that never attempted the new physical selector.
Close this only with the future SQL producer integration: build the logical
input and candidates, invoke the selector, and, on a rejected result, record
the mapped reason at the code path that actually dispatches to `where.c`.
Until then, unit coverage proves mapping semantics only; it does not prove
SQL routing or runtime counter coverage.

### M3.7 feature-flag readiness

`sql_new_planner_single_table=on/off` is not implementable as a meaningful
switch yet. `sql_physical_plan_from_logical()` selects only from a candidate
array supplied by its caller; SQL planning has no provider that constructs
that array from resolved indexes and estimates. `sql_plan_lower()` then emits
an abstract callback sequence, not VDBE bytecode, and neither API is called by
`sqlWhereBegin()`. An on/off setting added before those connections would be a
no-op or would claim a planner route that did not produce the executable plan.

The eventual contract is: off (the default) preserves current planning and
path classification. On may select `new_planner` only after candidate
generation, complete descriptor validation, and executable lowering succeed.
Unsupported shapes, absent candidates, and failures detected before bytecode
emission use the current planner with their stable fallback classification;
the new route must not emit partial VDBE and then fall back. Config scope
(session or instance) is intentionally unresolved. Implement M3.7 only after
an end-to-end success path exists, with tests proving default behavior is
unchanged and that `on` selects an executable, parity-tested plan only for the
supported query class.

## Purpose

The physical plan descriptor is the **stable contract between the planner
and VDBE lowering**. The planner produces a descriptor; lowering consumes
it and emits VDBE bytecode. Neither side reaches across this boundary.

Why this matters beyond "good layering":

- **Baseline format that survives executor change.** The roadmap's M0
  parity corpus defers L4 (access summary) and L5 (algorithm choice) until
  this descriptor exists. Once it does, those layers can be captured in a
  format that survives any future executor swap. VDBE bytecode is
  disposable; the descriptor is not.
- **Per-query path-class tracking.** Each descriptor carries an explicit
  `path_class` field naming which planner produced it. That is how the
  roadmap measures migration progress per query class.
- **JIT-agnostic.** The descriptor mentions no JIT backend. CnP and LLVM
  consume the bytecode that lowering emits; they never see the descriptor.

## Non-goals

- Not a runtime intermediate representation. Operators in the descriptor
  do not execute; they are lowered to VDBE.
- Not a relational algebra IR with rewrites. Rewrites belong to a
  logical-plan layer above this; the descriptor is the *output* of the
  physical-plan phase, not its working format.
- Not a serialization format for cross-version compatibility. Schema
  evolution is allowed; old snapshots may need re-baselining when the
  version field changes.

## Format

Two surfaces, one schema:

- **Runtime form** — MsgPack object built in the statement-lifetime arena.
  Consumed by VDBE lowering; never persisted.
- **Baseline form** — canonical YAML written to
  `test/sql-baselines/snapshots/.../q<N>.<engine>.yaml` under the
  optional `plan:` key once M3 emits a descriptor. Persisted, diffed,
  reviewed. M0-A/M0-B snapshots do not require this key.

A round-trip is required: MsgPack → canonical YAML → MsgPack must produce
byte-identical MsgPack. The canonical YAML is sort-keyed at every map
level so diffs are stable.

## Schema versioning

The descriptor carries a top-level `descriptor_version: <int>` field.

- **v1** is the schema described below. It supports the roadmap M3
  query class (single-table SELECT).
- **v2** adds joins (descriptor M3 follow-up).
- **v3** adds aggregates and DISTINCT.
- Bumping `descriptor_version` invalidates stored `plan:` comparisons and
  requires recapture of snapshots containing that key. L1/L2 snapshots
  without `plan:` remain valid if their own schema and query identity are
  unchanged. Each bump requires a written changelog entry.

M3 starts only after M1 owns statement-level `path_class` and replay
identity. Descriptor authors can build and test the v1 in-memory form with
fixed statistics while S1/S2 progress. A single integration owner then
wires the descriptor to VDBE lowering and the snapshot emitter. This keeps
the planner, lowerer, and statistics implementation independently testable
without giving them competing ownership of `where.c` or the snapshot schema.

## v1 schema

```yaml
descriptor_version: 1

path_class:
  taken: new_planner            # or current_where_c, fallback_<reason>
  reason: null                  # stable code from a fixed enum (see below)
  fallback_to: null             # "current_where_c" when taken != new_planner

relations:
  - rel_id: r0                  # stable within this descriptor
    space_id: 512
    space_name: t1              # for human-readable diffs only
    access:
      kind: IndexRangeScan      # see "operator vocabulary v1" below
      index_id: 0               # primary key in this example
      index_name: t1_pk
      bounds:
        - { side: lower, op: GE, expr_ref: e0 }
        - { side: upper, op: LT, expr_ref: e1 }
      direction: ASC            # ASC | DESC
      projected_columns: [c0, c1, c2]
      produced_order:
        - { column: c0, direction: ASC, nulls: FIRST }
      est_rows: 1200
      est_rows_confidence: 0.7

filters:
  - target_rel: r0
    expr_ref: e2
    est_selectivity: 0.5
    est_selectivity_confidence: 0.5

projections:
  - source: r0
    columns: [c0, c2]

finalize:
  - kind: Sort
    keys:
      - { column: c0, direction: ASC, nulls: FIRST }
    est_rows: 600
  - kind: Limit
    n: 10

expressions:
  - id: e0
    canonical: "param(:lo)"
  - id: e1
    canonical: "param(:hi)"
  - id: e2
    canonical: "eq(col(r0, c1), const_int(42))"

cost:
  startup: 0.0
  total: 1200.0
  rows: 10
  row_width: 24
  confidence: 0.6

planning:
  elapsed_us: 480
  budget_used_pct: 8
  candidates_generated: 12
  candidates_dominated: 7
  candidates_truncated: 0
  candidates_retained: 5
```

## Operator vocabulary v1

The narrow set required by roadmap M3:

| Kind | Where | Notes |
|------|-------|-------|
| `PkPointLookup` | `relations[].access` | Single-row equality on primary key. |
| `IndexPointLookup` | `relations[].access` | Single-row equality on secondary index. |
| `IndexRangeScan` | `relations[].access` | Open or closed range; direction explicit. |
| `IndexFullScan` | `relations[].access` | Full traversal in index order. |
| `TableFullScan` | `relations[].access` | Full traversal in physical order (memtx) or LSM order (Vinyl). |
| `Filter` | `filters[]` | Residual predicate applied after access. |
| `Project` | `projections[]` | Column subset selection. |
| `Sort` | `finalize[]` | Materializing sort. Property-produced order. |
| `Limit` | `finalize[]` | LIMIT/OFFSET. |
| `Result` | implicit terminal | Not represented; every descriptor implies one. |

## Properties

v1 captures only two properties per access path:

- `produced_order` — list of `(column, direction, nulls)` triples
  describing the natural output order. Required for `Sort` elimination
  later (when E1's property-aware dominance lands).
- `est_rows` + `est_rows_confidence` — used by cost and by future
  property dominance.

Required-properties (what downstream operators demand) are implicit in v1
because lowering walks the descriptor in fixed order. v2 will lift them
to explicit fields once join order matters.

## Cost shape

The `cost:` block uses the linear abstract units from
[`next_gen_sql_planner.md`](next_gen_sql_planner.md):

- `startup` — work before the first row;
- `total` — startup plus expected work to consume all rows;
- `rows` — expected delivered row count;
- `row_width` — expected average bytes per delivered row;
- `confidence` — `[0.0, 1.0]`, controls retention of close alternatives
  and fallback visibility.

v1 does not include separate `cpu` / `memory` / `engine_access` fields
because single-table lowering has nowhere to spend them. v2 expands cost
when join algorithms can differ in those dimensions.

## path_class and fallback reasons

`path_class.taken` is one of three stable strings:

- `current_where_c` — current planner produced this plan.
- `new_planner` — new planner produced this plan.
- `fallback_<reason>` — new planner rejected the query; current planner
  ran. `reason` is a stable enum:

| Reason code | Meaning |
|-------------|---------|
| `UNSUPPORTED_JOIN` | Query contains JOIN; v1 single-table-only. |
| `UNSUPPORTED_SUBQUERY` | Scalar / EXISTS / IN subquery. |
| `UNSUPPORTED_AGGREGATE` | GROUP BY / aggregate / DISTINCT. |
| `UNSUPPORTED_CTE` | WITH / WITH RECURSIVE. |
| `UNSUPPORTED_COMPOUND` | UNION / INTERSECT / EXCEPT. |
| `UNSUPPORTED_DML` | INSERT / UPDATE / DELETE. |
| `UNSUPPORTED_TRIGGER` | Statement involves trigger subprogram. |
| `UNSUPPORTED_NONDETERMINISTIC` | Function is not declared deterministic. This does not detect deterministic UDF side effects. |
| `UNSUPPORTED_ACCESS_HINT` | Explicit `INDEXED BY` / `NOT INDEXED` requirement is not modeled. |
| `UNSUPPORTED_FUNCTION` | Deterministic function call is outside the canonical expression contract. |
| `UNSUPPORTED_COLLATION` | Explicit collation semantics are not represented by the expression contract. |
| `UNSUPPORTED_EXPRESSION` | Resolved scalar expression is outside the canonical expression contract. |
| `BUDGET_EXCEEDED` | Planner search budget exhausted. |
| `LOW_CONFIDENCE_STATS` | Stats confidence below threshold (configurable). |
| `LOWERING_FAILED` | Internal bug in lowering; record and fall back. |
| `UNRESOLVED_INPUT` | Resolver did not provide a resolved SELECT shape. |
| `UNSUPPORTED_RELATION_COUNT` | Relation count is outside the v1 single-relation shape. |
| `UNSUPPORTED_DISTINCT` | DISTINCT is not supported by v1. |
| `INVALID_LOGICAL_PLAN` | Logical IR is missing or structurally invalid. |
| `NO_ACCESS_PATH` | No physical access candidate was supplied. |
| `INVALID_CANDIDATE` | All physical candidates were invalid. |

Reason codes are append-only. Adding one is a v1 schema change but does
not require a `descriptor_version` bump because old codes still parse.

## Canonical fingerprint

The descriptor's canonical YAML form has a SHA-256 fingerprint. Two
descriptors with identical fingerprints are guaranteed to produce
identical VDBE bytecode (assuming lowering is deterministic, which it
must be).

The fingerprint excludes:

- `cost:` block (estimates may shift with stats updates without
  changing the plan);
- `planning:` block (measurement, not plan content);
- `est_rows*` / `est_selectivity*` fields inside operators;
- `space_name` / `index_name` (debug labels only; `space_id` and
  `index_id` are authoritative).

The fingerprint is the L4/L5 baseline diff key. A fingerprint change is
a plan-shape change.

## Extension sketch (v2 and later)

Not required for M3, but worth flagging so v1 leaves room:

- **Joins (v2):** add `joins:` block with operator kinds `NestedLoop`,
  `HashJoin` (when hash join lands), `MergeJoin`. Each join names outer
  and inner sub-trees recursively; the schema becomes tree-shaped rather
  than the flat list it is in v1.
- **Required properties (v2):** lift to explicit fields per node so the
  enumerator E1 can reason about them.
- **Aggregates (v3):** add `Aggregate` operator with `kind: Streaming |
  Hash`, group keys, and aggregate-expression refs.
- **Subqueries (v3 or v4):** scalar / EXISTS / IN as nested
  descriptors with correlation-edge metadata.
- **DML (v4):** add `mutation:` block; descriptor terminates in an
  `Insert`, `Update`, or `Delete` sink rather than `Result`.

## Open questions

These remain deferred beyond the in-memory M3.1 contract. Document final
decisions in this file as they are made:

- **Q1.** Should `path_class.taken` carry a version of the new planner
  that produced it, so baselines distinguish "planner v1 chose X" from
  "planner v1.1 chose Y"? Provisional answer: yes, add
  `planner_version: <int>` alongside.
- **Q2.** What is the canonical-YAML serializer? A toy hand-written
  emitter is enough for M0; for production CI we need a library that
  guarantees stable map ordering. Likely candidates: PyYAML with
  explicit `sort_keys=True`, or a vendored canonical-YAML implementation.
- **Q3.** Should the fingerprint cover the `descriptor_version`? Yes —
  otherwise two semantically distinct schemas could collide.
- **Q4.** How does v1 represent `OFFSET`? Treated as a parameter to
  `Limit` (`{kind: Limit, n: 10, offset: 5}`), or as its own operator?
  Provisional answer: parameter to `Limit`; simpler for lowering.
- **Q5.** What happens when stats are missing entirely (S1 not yet run)?
  Provisional answer: `est_rows_confidence: 0.0` and the descriptor
  carries `LOW_CONFIDENCE_STATS` in `path_class` if the new planner
  would otherwise emit it; current planner just runs without confidence
  metadata.

## Cross-references

- [`roadmap.md`](roadmap.md) M3.1 — the milestone that promotes this
  document from `SPEC-DRAFTED` to `PROTOTYPE`.
- [`planner_vm_migration.md`](planner_vm_migration.md) — the conceptual
  companion that describes how each operator lowers to VDBE bytecode.
- [`statistics_implementation_plan.md`](statistics_implementation_plan.md)
  — defines the snapshot API the descriptor's `est_rows*` fields read
  from.
- [`next_gen_sql_planner.md`](next_gen_sql_planner.md) — the cost-units
  and property-precedence reasoning that the descriptor's `cost:` and
  property blocks instantiate.
