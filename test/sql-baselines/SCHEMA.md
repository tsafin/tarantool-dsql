# Parity Corpus Snapshot Schema (v1)

## Status

`SPEC-DRAFTED` — initial v1. Once any code in `m0/harness`, `m0/classifier`,
`m0/diff-and-ci`, or `m0/perf-trail` consumes or emits this schema, the spec
is **locked**. Bumping `schema_version` invalidates all stored snapshots.

## Purpose

This document is the one-way-door contract for the roadmap M0 milestone
([issue #15](https://github.com/tsafin/tarantool-dsql/issues/15)) under epic
[#14](https://github.com/tsafin/tarantool-dsql/issues/14). It defines:

- the per-`(test × engine)` YAML snapshot format consumed by the diff tool;
- the file path layout under `test/sql-baselines/snapshots/`;
- the canonical serialization rules that make snapshot diffs stable;
- the perf-trail CSV format (separate from snapshots, not gated).

Until this spec is committed, parallel worktrees implementing M0.2 / M0.4 /
M0.5 / M0.6 must wait. After it is committed, they fan out cleanly because
each touches different files.

## Scope (B-light)

Captured per `(test, engine)`:

| Layer | Captured? | Gated? |
|-------|-----------|--------|
| L1 — result rows | yes | hard gate (zero diffs) |
| L2 — diagnostic / error | yes | hard gate (zero diffs) |
| L3 — path_class | yes | hard gate (fallback must classify) |
| L4 — access summary | **deferred to M3** (descriptor exists) | — |
| L5 — algorithm choice | **deferred to M3** | — |
| L6 — VDBE opcode trace | yes (forensic only, on diff) | not gated |
| L7 — latency bands | perf-trail CSV (separate file) | not gated |

Dispatcher (generated / CnP / LLVM) is **not** a snapshot dimension. All
three execute every query under CI; their L1+L2 must match the single
stored snapshot for that `(test, engine)`. Divergence is a JIT correctness
bug, not a baseline variant.

## File path layout

```
test/sql-baselines/
├── SCHEMA.md                          this file
├── classification.yaml                M0.1 output, single file
├── snapshots/
│   ├── sql-tap/
│   │   ├── select1/
│   │   │   ├── q01.memtx.yaml
│   │   │   ├── q01.vinyl.yaml
│   │   │   ├── q02.memtx.yaml
│   │   │   └── ...
│   │   ├── join1/
│   │   │   └── ...
│   │   └── ...
│   ├── sql/
│   │   └── ...
│   └── sql-luatest/
│       └── ...
├── forensics/                         L6 traces (M0.3)
│   └── <suite>/<test>/q<N>.<engine>.<dispatcher>.trace-query
└── perf/                              L7 CSVs (M0.7)
    └── <YYYY-MM-DD>-<commit_sha>.csv
```

**Naming rules:**

- `<suite>` is `sql`, `sql-tap`, or `sql-luatest`.
- `<test>` is the test file basename without `.test.lua` / `.lua`.
- `<N>` is a zero-padded query index within the test file. Format is
  `%02d` by default (`q01`, `q02`, ... `q99`). Tooling MUST handle 3+ digit
  forms gracefully (`q100`, `q1000`) — the corpus has long-tailed test files
  and the limit is not artificial.
- `<engine>` is `memtx` or `vinyl`.
- `<dispatcher>` (forensics only) is `generated`, `cnp`, or `llvm`.

Queries are numbered in the order the test file emits them to
`box.execute()`. The classifier (M0.1) is responsible for emitting per-query
indices into `classification.yaml`.

## v1 snapshot schema

```yaml
schema_version: 1

test:
  suite: sql-tap
  file: select1.test.lua
  query_index: 1
  query_sql: |
    SELECT c0, c1 FROM t WHERE c0 = 42 ORDER BY c1
  feature_tags:                       # from classification.yaml, copied for self-containment
    - single_table_select
    - filter
    - order_by

engine: memtx                          # memtx | vinyl

captured:
  at: 2026-06-21T22:00:00Z             # UTC, RFC3339
  against_commit: fe3d181199           # short git hash
  tarantool_version: 3.x-dev           # from box.info.version
  primary_dispatcher: generated        # the dispatcher whose result was stored

l1_result:
  ok: true                             # false when L2 has error
  column_names: [c0, c1]
  column_types: [INTEGER, TEXT]
  rows_sorted: true                    # always true; see "Result canonicalization"
  rows:
    - [42, "alice"]
    - [42, "bob"]
    - [42, "charlie"]

l2_diagnostic:
  status: success                      # success | error
  error_code: null                     # stable error code string when status == error
  error_message_canonical: null        # canonicalized message text (parameter/path-stripped)

l3_path_class:
  taken: current_where_c               # current_where_c | new_planner | fallback_<reason>
  reason: null                         # stable enum code when taken starts with fallback_
  fallback_to: null                    # set when taken != new_planner and != current_where_c

metadata:
  planner_version: 0                   # bumped when new planner ships
  classification_version: 1            # bumped on classifier semantic change

dispatcher_parity:
  generated:
    l1_match: true                     # set by parity job at CI time
    l2_match: true
  cnp:
    l1_match: true
    l2_match: true
  llvm:
    l1_match: true
    l2_match: true
```

**Required fields:** `schema_version`, `test.*`, `engine`, `captured.*`,
`l1_result.{ok, rows_sorted, rows}`, `l2_diagnostic.status`, `l3_path_class.taken`.

**Optional fields:** `dispatcher_parity` (filled at CI time, not at capture).
`l1_result.{column_names, column_types}` are recommended but may be omitted
for queries that error before producing a header (in which case `l1_result.ok`
is `false`).

## Result canonicalization

Snapshot diffs must be stable across engine ordering, JIT execution
variance, and floating-point formatting. The harness MUST:

1. **Sort `l1_result.rows`** lexicographically by stringified row form
   *unless* the original SQL contains an `ORDER BY`. With `ORDER BY`,
   preserve the original order; record `rows_sorted: false` in that case.
2. **Normalize floating-point representation.** Each float is serialized as
   a YAML string with the format `!!str "1.234560e+02"` (uppercase `E` or
   lowercase consistent per file; pick lowercase). Floats that are exact
   integers serialize as integers.
3. **Encode binary blobs as Base64** with explicit `!!binary` tag.
4. **Encode NULL as YAML `null`** (not `~`, not empty).
5. **Encode booleans as YAML `true` / `false`** (not `yes` / `no`).
6. **UTF-8 strings, no BOM.** Embedded NULs in strings forbidden.
7. **Sort all map keys** alphabetically at every nesting depth.
8. **LF line endings**, single trailing newline at EOF.

A round-trip `parse(emit(parse(file))) == parse(file)` must hold. The diff
tool relies on byte-equality after canonicalization.

## L2 diagnostic canonicalization

Error messages embed parameters, paths, and addresses that churn between
runs. The harness MUST strip:

- absolute file paths (replace with `<path>`);
- numeric line/column positions (replace with `<N>`);
- pointer addresses (replace with `<addr>`);
- timestamps (replace with `<ts>`);
- parameter values inside diagnostic substrings like `"value 42 is out of range"`
  (replace with `<value>`, but **preserve column/table names** because those
  are semantic).

The stable `error_code` field is the SQL error code (e.g.
`ER_SQL_PARSER_GENERIC`, `ER_SQL_TYPE_MISMATCH`), not the message text.
The diff tool gates on `error_code`; `error_message_canonical` is diffable
but not gating.

## L3 path_class enum

Stable string values for `l3_path_class.taken`:

- `current_where_c` — current planner produced this plan.
- `new_planner` — new planner (M3+) produced this plan.
- `fallback_<reason>` — new planner rejected the query; current planner ran.

Stable values for `l3_path_class.reason` when `taken` starts with `fallback_`:

| Reason code | Meaning |
|-------------|---------|
| `UNSUPPORTED_JOIN` | JOIN; new planner is single-table-only in M3. |
| `UNSUPPORTED_SUBQUERY` | Scalar / EXISTS / IN subquery. |
| `UNSUPPORTED_AGGREGATE` | GROUP BY / aggregate / DISTINCT. |
| `UNSUPPORTED_CTE` | WITH / WITH RECURSIVE. |
| `UNSUPPORTED_COMPOUND` | UNION / INTERSECT / EXCEPT. |
| `UNSUPPORTED_DML` | INSERT / UPDATE / DELETE. |
| `UNSUPPORTED_TRIGGER` | Statement invokes trigger subprogram. |
| `UNSUPPORTED_NONDETERMINISTIC` | Non-deterministic or side-effecting function. |
| `BUDGET_EXCEEDED` | New planner search budget exhausted. |
| `LOW_CONFIDENCE_STATS` | Stats confidence below threshold (post-S1). |
| `LOWERING_FAILED` | Internal bug; falls back rather than crashing. |

Adding a reason code is append-only and does NOT bump `schema_version`.

## L6 forensic trace format

L6 traces are NOT in the snapshot YAML. They live under
`test/sql-baselines/forensics/<suite>/<test>/q<N>.<engine>.<dispatcher>.trace-query`
and are written only when the diff tool detects an L1 or L2 mismatch.

The `.trace-query` extension distinguishes per-query VDBE traces from any
other Tarantool trace artifacts that might land in the same directory tree.

Format: one VDBE opcode per line, comma-separated:

```
<pc>,<opcode_name>,<P1>,<P2>,<P3>,<P4_kind>:<P4_value>,<P5>
```

`P4_value` is a stable rendering (string-quoted, integers as decimal,
pointers stripped). `P4_kind` is one of: `INT32`, `INT64`, `STRING`,
`COLLATE`, `KEYINFO`, `FUNCDEF`, `VTAB`, `NONE`.

The forensic format is used for human inspection only; the diff tool does
not gate on it. Format changes do NOT bump `schema_version`.

## Perf-trail CSV (L7)

One CSV per CI run at `test/sql-baselines/perf/<YYYY-MM-DD>-<commit_sha>.csv`:

```csv
test_id,engine,dispatcher,time_p50_us,time_p95_us,rows_returned,memory_peak_bytes
sql-tap/select1/q01,memtx,generated,124,189,3,12288
sql-tap/select1/q01,memtx,cnp,87,141,3,12288
sql-tap/select1/q01,memtx,llvm,92,158,3,12288
sql-tap/select1/q01,vinyl,generated,398,612,3,16384
...
```

CSV format is NOT versioned in the schema. It is a metrics-store artifact,
not a parity gate. Schema changes to the CSV are tracked separately.

## Versioning policy

- `schema_version: 1` is this document.
- Bumping `schema_version` invalidates all stored snapshots. The harness
  must re-capture against `master` and replace the entire `snapshots/` tree.
- Adding a new top-level field that is *optional* (with backward-compatible
  default behavior) does NOT bump `schema_version`.
- Adding a new required field, removing a field, renaming a field, or
  changing semantics of an existing field DOES bump `schema_version`.
- L3 reason codes and L6 forensic format are append-only / non-versioned.

## What this enables for parallel worktrees

After this file is committed, the following can fan out without merge
collisions:

| Worktree branch | Subtasks | Depends on this schema for |
|-----------------|----------|----------------------------|
| `m0/classifier` | M0.1 | `classification.yaml` shape |
| `m0/harness` | M0.2, M0.3 | snapshot emission, forensic format |
| `m0/diff-and-ci` | M0.4, M0.5, M0.6 | snapshot reading, parity check format |
| `m0/perf-trail` | M0.7 | perf-trail CSV columns |

`s0/audit` does NOT depend on this schema and can start in parallel
without waiting.

## Resolved decisions

- **Language: Lua, not Python.** Tarantool is a Lua shop with embedded LuaJIT.
  Modern Tarantool tests use `luatest`. The harness, classifier, diff tool,
  and CI integration are all written in Lua. The in-Tarantool capture helper
  runs natively; the diff/classifier tools run via standalone `tarantool`
  binary if needed outside the test harness. YAML serialization uses a
  vendored canonical-YAML implementation (or `lua-yaml` with explicit
  sort-keys pass) — NOT PyYAML.
- **Feature tags: denormalized per snapshot** (full tag set copied from
  `classification.yaml` into each snapshot's `test.feature_tags`). Adds
  ~5-10 short strings per file; gives self-contained replay so a snapshot
  can be inspected without the classification index.
- **Primary dispatcher: `generated`.** The reference L1+L2 are captured under
  the generated interpreter; CnP and LLVM are parity-checked against it.

## Open questions

Deferred to M0 implementation. Document final decisions here as they are
made.

- **Q1.** When a test errors during setup (before any query runs), what
  snapshot is produced? Provisional: emit one snapshot file with
  `query_index: 0`, `l1_result.ok: false`, error in `l2_diagnostic`.
- **Q2.** How do we handle tests that produce *random* output (e.g. tests
  using `random()` without seeding)? Provisional: exclude from corpus via
  classifier tag `nondeterministic`, list in `classification.yaml` with
  reason.
- **Q3.** Does the harness need to invoke `box.snapshot()` between tests to
  ensure clean state, or is `rm -f *.snap *.xlog` between runs sufficient?
  Provisional: `rm -f` between runs, matches CLAUDE.md guidance.

## Cross-references

- [`docs/vdbe/roadmap.md`](../../docs/vdbe/roadmap.md) — overall plan;
  this schema is M0's one-way door.
- [`docs/vdbe/current_sql_feature_matrix.md`](../../docs/vdbe/current_sql_feature_matrix.md) —
  L1/L2/L3 layer definitions; this file is the wire format.
- [`docs/vdbe/physical_plan_descriptor.md`](../../docs/vdbe/physical_plan_descriptor.md) —
  L4/L5 descriptor format that will extend this schema when M3 lands.
