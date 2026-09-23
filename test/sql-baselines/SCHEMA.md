# Parity Corpus Snapshot Schema (v1)

## Status

`PROTOTYPE` — v1 has consumers and emitters, but no accepted corpus baseline.
The 2026-09-24 M0-A/M0-B review in
[`roadmap.md`](../../docs/vdbe/roadmap.md) must reconcile implementation and
contract before accepting snapshots. After acceptance, semantic changes to
required fields use the versioning policy below.

## Purpose

This document is the one-way-door contract for the roadmap M0 milestone
([issue #15](https://github.com/tsafin/tarantool-dsql/issues/15)) under epic
[#14](https://github.com/tsafin/tarantool-dsql/issues/14). It defines:

- the per-`(test × engine)` YAML snapshot format consumed by the diff tool;
- the file path layout under `test/sql-baselines/snapshots/`;
- the canonical serialization rules that make snapshot diffs stable;
- the perf-trail CSV format (separate from snapshots, not gated).

M0.1–M0.7 tooling was committed against this draft. The next parallel
implementation wave follows the M0.8b manifest and comparison contract
below.

## Scope (B-light)

Captured per `(test, engine)`:

| Layer | Captured? | Gated? |
|-------|-----------|--------|
| L1 — result rows | yes | hard gate (zero diffs) |
| L2 — diagnostic / error | yes | hard gate (zero diffs) |
| L3 — path_class | yes | policy gate (planner switch/fallback reviewed) |
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
├── manifests/                         M0-A per-test run outcomes
│   └── <suite>/<test>.<engine>.json
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
`box.execute()`. The present classifier emits **file-level** feature tags;
it does not assign query indices. `test.feature_tags` in current snapshots
therefore describes the containing file, not necessarily that SQL statement.
Do not use these tags as query-level coverage or an M3 eligibility oracle.
M0.8b must decide whether to add a separately named query-level tag field or
replace this field with a version bump, then validate that choice against
captured SQL rather than file text.

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

The current harness writes `current_where_c` unconditionally. It is a
placeholder until M1 emits the actual statement path. A candidate using the
new planner will intentionally change L3 from `current_where_c` to
`new_planner`; a byte-for-byte L3 gate would reject the migration itself.
M0.8b must define a path policy: verify L3 is sourced from execution,
require explicit eligibility and fallback reasons, and review any increase
in fallback count or unexpected path switch. L1/L2 remain hard parity gates.
Until that policy is implemented, no CI result may claim to gate planner
selection.

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

## Baseline acceptance contract (M0-A and M0-B)

The snapshot schema alone cannot prove a complete run. The standalone harness
now writes one JSON outcome at
`manifests/<suite>/<test>.<engine>.json` for every attempted test. Manifest
v1 contains `manifest_version: 1`, `suite`, `test_file`, `engine`,
`runtime_engine`, `engine_mismatch`, `dispatcher_requested`, `sql_jit_enable`,
`execution_mode`, `mode_executed`, `cnp_exec_delta`, `llvm_exec_delta`,
`test_exit_code` (integer or `"missing"`), `test_load_ok`,
`test_load_error`, `cfg_errors`, `captured_queries`, `written_snapshots`,
`skipped_queries`, `snapshot_errors`, and `accepted`. A manifest describes
one test execution, not an entire suite or an exclusion. Suite inventory,
explicit exclusion reasons, and per-query dispatcher proof are M0-B work.

The harness requires `--work-dir=<absolute empty directory>` for database
isolation. It exits nonzero and writes no snapshots when the test load fails,
TAP exits nonzero or never exits, `box.cfg` fails, no query was captured, or
the runtime SQL default engine differs from `--engine` at any captured query
or at test end. For CnP and LLVM modes, the appropriate execution counter
must rise during the test; a build with LLVM disabled therefore cannot
produce an accepted LLVM capture. Any skipped query or
snapshot write failure also rejects the run. An unsuccessful run's manifest
remains for diagnosis. The output tree is not accepted until
`tarantool test/sql-baselines/validate.lua <capture-root>` succeeds; this
validator checks nonempty manifests, all run outcomes, contiguous snapshot
IDs, required v1 fields, and orphan snapshots.

Acceptance requires:

1. The normal test-runner setup is honored, or the standalone runner has a
   demonstrated equivalent setup for that test. A load error, failed TAP
   assertion, suppressed `box.cfg` error, write error, or early exit fails
   the run; captured queries from that run are quarantined.
2. The suite/engine coverage set and query IDs match the manifests on repeat
   capture. Empty trees, missing tests, truncated runs, and unknown engines
   fail the gate. A deliberate result or diagnostic mutation must fail CI.
3. Required v1 fields are present. In particular, verify `rows_sorted` is
   actually emitted, NULLs survive array serialization, diagnostics carry a
   stable code when available, and order-sensitive queries keep row order.
   These are known reconciliation points in the current prototype.
4. `captured.at`, `captured.against_commit`, and runtime version are
   provenance, not semantic parity keys. Compare L1/L2 and the applicable
   L3 policy; report provenance separately. Compare the same query identity
   and SQL text before comparing rows. A planned switch to `new_planner`
   must not fail solely because the path string changed.
5. Run generated, CnP, and LLVM modes only where the build supports them.
   Record unsupported modes explicitly; do not report a skipped mode as
   parity success. Both memtx and Vinyl need declared coverage.

The uncommitted `sql-tap`/memtx snapshots observed on 2026-09-24 are an
investigation artifact. They must not become the baseline until the above
checks pass and their size/storage choice is reviewed.

## What this enables for parallel worktrees

After M0.8b locks the manifest and query-identity contract, the following
can fan out without merge collisions:

| Worktree branch | Subtasks | Depends on this schema for |
|-----------------|----------|----------------------------|
| `m0/capture` | M0.8a | run outcome and manifest emission |
| `m0/corpus` | M0.8c | suite/engine inventory and isolated capture shards |
| `m0/ci` | M0.8d | manifest equality and snapshot/dispatcher comparison |
| `m0/classification` | M0.8b follow-up | file/query tag distinction |

The manifest/schema owner integrates these tracks serially; S0 is complete.

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
