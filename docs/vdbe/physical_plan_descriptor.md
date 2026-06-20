# Physical Plan Descriptor

## Status

`SPEC-DRAFTED` — initial v1 covers only the single-table query class.
Joins, aggregates, and subqueries extend the schema in later versions.

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
  `plan:` key. Persisted, diffed, reviewed.

A round-trip is required: MsgPack → canonical YAML → MsgPack must produce
byte-identical MsgPack. The canonical YAML is sort-keyed at every map
level so diffs are stable.

## Schema versioning

The descriptor carries a top-level `descriptor_version: <int>` field.

- **v1** is the schema described below. It supports the roadmap M3
  query class (single-table SELECT).
- **v2** adds joins (descriptor M3 follow-up).
- **v3** adds aggregates and DISTINCT.
- Bumping `descriptor_version` invalidates all stored baselines. The
  M0 harness re-captures baselines on a bump. This is a one-way door per
  version and requires a written changelog entry.

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
| `UNSUPPORTED_NONDETERMINISTIC` | Non-deterministic or side-effecting function. |
| `BUDGET_EXCEEDED` | Planner search budget exhausted. |
| `LOW_CONFIDENCE_STATS` | Stats confidence below threshold (configurable). |
| `LOWERING_FAILED` | Internal bug in lowering; record and fall back. |

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

These are deferred to M3.1 implementation. Document final decisions in
this file as they are made:

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
