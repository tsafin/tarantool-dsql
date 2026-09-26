# Parity Corpus Snapshot Schema (v1)

## Status

`ACCEPTED LOCALLY` — manifest v1 and snapshot schema v1 are the full-corpus
capture contract. The first hosted full-corpus CI result is pending branch
publication. Reviewed policy v2 names baseline anchor
`04b63d19ab7deaa233ec2549d467b79d0cf4f5f2`. Semantic changes to
required fields use the versioning policy below.

## Purpose

This document is the one-way-door contract for the roadmap M0 milestone
([issue #15](https://github.com/tsafin/tarantool-dsql/issues/15)) under epic
[#14](https://github.com/tsafin/tarantool-dsql/issues/14). It defines:

- the per-`(test × engine)` YAML snapshot format consumed by the diff tool;
- the file path layout under `test/sql-baselines/snapshots/`;
- the canonical serialization rules that make snapshot diffs stable;
- the perf-trail CSV format (separate from snapshots, not gated).

M0.1–M0.8 tooling was reconciled against this contract and accepted with
the full local generated/CnP/LLVM matrix and repeat captures. Future
planner-path changes still need a reviewed L3 exception.

## Scope (B-light)

Captured per `(test, engine)`:

| Layer | Captured? | Gated? |
|-------|-----------|--------|
| L1 — result rows | yes | hard gate (zero diffs) |
| L2 — diagnostic / error | yes | hard gate (zero diffs) |
| L3 — path_class | yes | policy gate (planner switch/fallback reviewed) |
| L4 — access summary | **deferred to M3** (descriptor exists) | — |
| L5 — algorithm choice | **deferred to M3** | — |
| L6 — VDBE program listing | yes (forensic only, on request) | not gated |
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
├── forensics/                         L6 VDBE listings (M0.3)
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
`box.execute()`. Query identity consists of the snapshot path **and**
`test.{suite,file,query_index,query_sql}` and `engine`; a mismatch in any of
these is a hard gate even when the result happens to match. The present
classifier emits **file-level** feature tags; `test.feature_tags` retains
that meaning in v1 and is never query-level coverage or an M3 eligibility
oracle. A future query-level field must have a distinct name and derive from
captured SQL, not from the containing file.
`query_sql` is ordinary YAML text for valid UTF-8 and a `!!binary` scalar
for SQL containing invalid UTF-8 or control bytes. Both forms identify the
original SQL bytes; the validator and diff compare binary content bytewise.

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
  against_commit: fe3d181199           # short hash of the source repository under test
  tarantool_version: 3.x-dev           # from box.info.version
  primary_dispatcher: generated        # the dispatcher whose result was stored

l1_result:
  ok: true                             # false when L2 has error
  column_names: [c0, c1]
  column_types: [integer, string]
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
  taken: current_where_c               # current_where_c | new_planner | fallback | fallback_<reason>
  reason: null                         # required stable code for fallback; nil otherwise
  fallback_to: null                    # current_where_c for fallback; nil otherwise

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

**Required fields:** `schema_version`, `test.{suite,file,query_index,query_sql}`,
`engine`, `captured.*`, `l1_result.{ok,rows_sorted,rows}`,
`l2_diagnostic.status`, `l3_path_class.taken`. When the result has a SQL
header, `l1_result.{column_names,column_types}` must also be emitted. The
comparison hard-gates all present L1 fields, including header metadata;
absence on only one side is a hard gate.

**Optional fields:** `dispatcher_parity` (filled at CI time, not at capture).
`l1_result.{column_names,column_types}` may be omitted for statements that
have no result header, including errors and DDL/DML.

## Result canonicalization

Snapshot diffs must be stable across engine ordering, JIT execution
variance, and floating-point formatting. The harness MUST:

1. **Sort `l1_result.rows`** lexicographically by stringified row form
   *unless* the original SQL contains an `ORDER BY`. With `ORDER BY`,
   preserve the original order; record `rows_sorted: false` in that case.
2. **Normalize floating-point representation.** Finite Lua numbers use 17
   significant digits so adjacent doubles remain distinct. Safe exact
   integers serialize as YAML integers; larger numbers serialize as YAML
   floats. Non-finite numbers reject the capture until v1 has a typed
   representation for them; a quoted string would conflate SQL FLOAT with
   SQL TEXT.
3. **Encode binary blobs as Base64** with explicit `!!binary` tag.
4. **Encode NULL as YAML `null`** (not `~`, not empty).
5. **Encode booleans as YAML `true` / `false`** (not `yes` / `no`).
6. **UTF-8 text, no BOM.** ASCII controls, C1 Unicode controls
   (U+0080–U+009F), and invalid UTF-8 are emitted as Base64 binary, not as
   pointer-like or lossy text.
7. **Preserve SQL MAP and ARRAY values recursively.** SQL container values
   must never be converted to Lua pointer strings. Keep numeric and text map
   keys distinct and keep an empty MAP (`{}`) distinct from an empty ARRAY
   (`[]`). Sort map keys deterministically at every nesting depth.
8. **Normalize EXPLAIN's volatile `OpenTEphemeral` P4 only.** When this
   opcode has a nonempty P4, its `sql_space_info` pointer bytes are replaced
   with `<sql_space_info>`; every other opcode/P4 value remains gated.
9. **Normalize the observed generated subquery metadata form.** The engine
   formats temporary derived-table names with a process pointer. Only a
   column name matching `sql_sq_<UPPERCASE-HEX>.COLUMN_<digits>` has its
   pointer segment replaced with `<generated>`; bare names, other suffixes,
   and user names remain gated.
10. **LF line endings**, single trailing newline at EOF.

Extended SQL scalars and containers use typed cell wrappers inside
`l1_result.rows`, because Tarantool's YAML decoder otherwise turns DECIMAL
into a Lua number and DATETIME into text. Examples:

```yaml
- {sql_type: decimal, value: '1.20'}
- {sql_type: datetime, value: '2020-01-01T00:00:00Z'}
- {sql_type: array, items: [11, 22]}
- {sql_type: map, entries: [{key: 1, value: two}]}
```

The wrapper is reserved for captured SQL values; native SQL MAP/ARRAY cells
are always wrapped, so their keys cannot collide with `sql_type` or `value`.
Int64, uint64, UUID, INTERVAL, and VARBINARY also use `sql_type`/`value`.
Unrecognized cdata fails the capture instead of silently losing type.

A round-trip `parse(emit(parse(file))) == parse(file)` must hold. Semantic
comparison uses typed decoded values; provenance and YAML formatting are not
parity keys. Distinguish the SQL string `'1'` from the number `1`, and a
missing cell from SQL NULL.

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

- `null` — the statement did not enter the WHERE planner; no planner path
  classification applies.
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

The current M3 producer also emits `fallback` as `taken`, with the stable
reason in the separate `reason` field. Its append-only reason codes are
`UNRESOLVED_INPUT`, `UNSUPPORTED_RELATION_COUNT`, `UNSUPPORTED_SUBQUERY`,
`UNSUPPORTED_AGGREGATE`, `UNSUPPORTED_COMPOUND`, `UNSUPPORTED_CTE`,
`UNSUPPORTED_DISTINCT`, `INVALID_LOGICAL_PLAN`, `NO_ACCESS_PATH`, and
`INVALID_CANDIDATE`. For both fallback encodings, `fallback_to` must be
`current_where_c`; non-fallback paths must leave both `reason` and
`fallback_to` null. The capture validator enforces these combinations.

Adding a reason code is append-only and does NOT bump `schema_version`.

M0 capture obtains the path from `EXPLAIN (planner = 'snapshot')`: statements
that use the legacy planner report `current_where_c`, statements that do not
enter the WHERE planner report `null`, while supported rejects report
`fallback` with a stable reason and `fallback_to: current_where_c`.
The diff hard-gates a path change by default. A deliberate switch to
`new_planner` needs an explicit, reviewed path-change policy;
`--ignore-path-class` is reserved for same-build dispatcher parity and must
not be used for PR baseline comparison. Until M1 supplies runtime path
evidence, M0 CI proves L1/L2 parity but not planner routing.

## L6 forensic VDBE listing format

L6 listings are NOT in the snapshot YAML. They live under
`test/sql-baselines/forensics/<suite>/<test>/q<N>.<engine>.<dispatcher>.trace-query`
and are written only when the diff tool detects an L1 or L2 mismatch.

The `.trace-query` extension distinguishes per-query VDBE listings from other
Tarantool trace artifacts that might land in the same directory tree.

Format: one VDBE opcode per line, comma-separated:

```
<pc>,<opcode_name>,<P1>,<P2>,<P3>,<P4_kind>:<P4_value>,<P5>
```

`P4_value` is a stable rendering (string-quoted, integers as decimal,
pointers stripped). `P4_kind` is one of: `INT32`, `INT64`, `STRING`,
`COLLATE`, `KEYINFO`, `FUNCDEF`, `VTAB`, `NONE`, `EXPLAIN_TEXT`.
`EXPLAIN_TEXT` means the capture came from SQL's public `EXPLAIN` result;
that interface does not expose the original internal P4 union tag. L6 output
is a static VDBE program listing: it includes untaken branches and is not a
record of the dynamic opcode dispatch sequence. SQL statements unsupported by
`EXPLAIN` produce a comment-only capture file with a reason.

The forensic format is used for human inspection only; the diff tool does
not gate on it. It captures a static VDBE program listing, not the dynamic
opcode sequence taken at execution time. Format changes do NOT bump
`schema_version`.

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
`skipped_queries`, `snapshot_errors`, and `accepted`. The current v1 capture
also records sorted query-index arrays: `executed_query_indices`,
`native_compile_attempt_query_indices`, `native_compile_success_query_indices`,
`native_participation_query_indices`, `eligible_query_indices`, and
`mode_miss_queries`, plus counts `eligible_queries` and
`native_participation_queries`. A manifest describes one test execution,
not an entire suite or an exclusion. Engine-specific inclusion/exclusion is
defined by `corpus.json`.

The manifest may also include `planner_metrics_version: 2` and a
`planner_metrics` array copied from `EXPLAIN (planner = 'snapshot')`. Each
entry identifies the query and its path/reason plus per-statement candidate,
elapsed-time, and fallback counts, as well as generated, dominated,
truncated, and retained bounded-path counts. These measurements are diagnostic
only; they are excluded from baseline equality and corpus acceptance gates.
When present, the capture validator checks each entry's unique increasing
query index, valid path/reason pair, and agreement with the corresponding
snapshot's L3 path metadata. Metrics remain optional for compatibility, so
this check does not establish complete per-SELECT metric coverage.

The per-query native proof distinguishes execution from structural native
eligibility. An executed query has a positive interpreter-step or selected
native-execution counter delta. A query is eligible only when it executed
and either compiled successfully for the selected native mode or entered
native code (including a cache hit). A mode miss is an eligible query with
no native entry and rejects the capture. Compile-only `EXPLAIN` queries and
deliberate short-program LLVM fallbacks are therefore recorded but not
misreported as native mode misses. Generated mode records executed indices
and empty native lists. The counters are process-global, so tests with
concurrent, unattributable SQL execution need an explicit corpus exclusion.

The harness requires `--work-dir=<absolute empty directory>` for database
isolation. It exits nonzero and writes no snapshots when the test load fails,
TAP exits nonzero or never exits, `box.cfg` fails, no query was captured, or
the runtime SQL default engine differs from `--engine` at any captured query
or at test end. For CnP and LLVM modes, the appropriate execution counter
must rise during the test; a build with LLVM disabled therefore cannot
produce an accepted LLVM capture. Any mode miss, skipped query, or
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
4. `captured.at`, `captured.against_commit`, and runtime version are
   provenance, not semantic parity keys. Compare query identity, L1/L2, and
   the applicable L3 policy. A planned switch to `new_planner` requires a
   reviewed exception; it must not be silently ignored. `corpus.py` passes the
   tested repository's full Git SHA to each capture process, including the
   normal-runner child, so a baseline captured with the head's harness still
   records the baseline commit here.
5. Run generated, CnP, and LLVM modes only where the build supports them.
   Record unsupported modes explicitly; do not report a skipped mode as
   parity success. Both memtx and Vinyl need declared coverage.

The uncommitted `sql-tap`/memtx snapshots observed on 2026-09-24 remain an
investigation artifact: they cover only one suite and engine and are not the
named reproducible baseline.

## M0 implementation worktree history

The completed M0 work was split across independent worktrees after the
manifest and query-identity contract was locked:

| Worktree branch | Delivered work |
|-----------------|----------------|
| `m0/capture` | run outcomes, manifests, and capture validation |
| `m0/corpus` | suite/engine inventory, reviews, and isolated capture shards |
| `m0/ci` | manifest equality and snapshot/dispatcher workflows |
| `m0/classification` | file-level classification contract |

The integration owner assembled the policy and baseline serially; all tracks
are complete. S0 is complete as well.

## Resolved decisions

- **Language: Lua for capture, schema and parity semantics; Python for the
  outer batch runner.** Tarantool is a Lua shop with embedded LuaJIT. Modern
  Tarantool tests use `luatest`. The harness, classifier, validator, and diff
  tool run via the Tarantool binary. `corpus.py` uses only the Python standard
  library to enumerate suite files, launch isolated Tarantool processes, and
  check manifest identity and query counts. This matches the existing Python
  test-run.py infrastructure while keeping result interpretation in Lua.
  YAML serialization uses a
  vendored canonical-YAML implementation (or `lua-yaml` with explicit
  sort-keys pass) — NOT PyYAML.
- **Feature tags: denormalized per snapshot** (full tag set copied from
  `classification.yaml` into each snapshot's `test.feature_tags`). Adds
  ~5-10 short strings per file; gives self-contained replay so a snapshot
  can be inspected without the classification index.
- **Primary dispatcher: `generated`.** The reference L1+L2 are captured under
  the generated interpreter; CnP and LLVM are parity-checked against it.

## Resolved implementation questions

These decisions were verified during M0 acceptance.

- **Q1.** A setup failure produces a rejected run manifest and **no accepted
  snapshots**. A synthetic query-zero error would misrepresent an unexecuted
  SQL statement.
- **Q2.** A nondeterministic test is excluded only with a per-test reason and
  repeat-capture evidence; a file-level classifier tag alone is not proof.
- **Q3.** Each test gets a fresh, empty database work directory. Neither
  `box.snapshot()` nor deleting files in a shared directory is required.

## Cross-references

- [`docs/vdbe/roadmap.md`](../../docs/vdbe/roadmap.md) — overall plan;
  this schema is M0's one-way door.
- [`docs/vdbe/current_sql_feature_matrix.md`](../../docs/vdbe/current_sql_feature_matrix.md) —
  L1/L2/L3 layer definitions; this file is the wire format.
- [`docs/vdbe/physical_plan_descriptor.md`](../../docs/vdbe/physical_plan_descriptor.md) —
  L4/L5 descriptor format that will extend this schema when M3 lands.
