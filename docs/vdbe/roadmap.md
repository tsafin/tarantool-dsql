# Tarantool SQL Analytics Engine — Roadmap

## Purpose

This document is the single source of truth for the analytics-focused SQL
engine work on `tsafin/nextgen_sql` and its descendants. Status below was
reconciled with the local tree on 2026-09-30; uncommitted files are evidence
of work in progress, not completed deliverables.

Scope is deliberately narrow:

- proper SQL statistics, persisted in new system spaces;
- a new planner / lowering path that consumes those statistics and produces
  better plans for analytic workloads;
- a parity corpus that proves the new path never regresses results,
  diagnostics, or supported-class coverage of the current SQL engine.

Out of scope here (tracked elsewhere or rejected):

- push-based execution engine (rejected — VDBE remains the only executor);
- arm64 CnP stencil work (different machine);
- broad SQL feature additions (RIGHT JOIN, LATERAL, windows, UPSERT,
  RETURNING — none are parity requirements).

This file is structured so that it can be lifted directly into a GitHub epic
with linked sub-issues. Top-level milestones (M0, M1, S0, S1, S2, M3, E1, GATE)
each map to a sub-issue; their checklist items map to individual tasks.

## Related documents

- [`current_sql_feature_matrix.md`](current_sql_feature_matrix.md) —
  authoritative parity baseline (frozen subset).
- [`statistics_implementation_plan.md`](statistics_implementation_plan.md) —
  statistics infrastructure design.
- [`sql_stats_sampling.md`](sql_stats_sampling.md) — S1 memtx/Vinyl sampling
  contracts and feasibility constraints.
- [`planner_vm_migration.md`](planner_vm_migration.md) — staged migration
  of `where.c` to planner-IR + VDBE lowering.
- [`next_gen_sql_planner.md`](next_gen_sql_planner.md) — architecture survey
  (PostgreSQL / DPhyp / ORCA comparisons; memtx and Vinyl applicability).

## Status legend

Each work item carries a richer state than NOT-STARTED / DONE so the
roadmap accurately reflects how far an item has progressed past spec.

| State | Meaning |
|-------|---------|
| `NOT-STARTED` | No work, no spec beyond a paragraph in this file. |
| `SPEC-DRAFTED` | A focused spec exists; implementation has not started. |
| `PROTOTYPE` | Working code or tooling exists, but acceptance/CI gates are incomplete. |
| `FEATURE-GATED` | Default off in production builds; CI runs it on the parity corpus. |
| `PRODUCTION` | Default on; old path removed or scheduled for removal. |
| `COMPLETE` | A finite audit or decision deliverable is finished and recorded. |

The implementation maturity path is:
`NOT-STARTED → SPEC-DRAFTED → PROTOTYPE → FEATURE-GATED → PRODUCTION`.
Audits and decisions can instead end at `COMPLETE`.

## Execution model

Single human operator. Multiple LLM sessions allowed in parallel via git
worktrees. Implications:

- The human is the bottleneck for architectural decisions, reviews, and merges.
- LLM sessions are good at: implementing scoped sub-tasks, writing tests,
  mechanical refactors, drafting documentation.
- A sub-task marked `parallel: yes` is a candidate for an isolated worktree
  with its own LLM session. It must touch files that no concurrent task
  modifies, or its merge will fight.
- Sub-tasks marked `parallel: no` need human attention or touch a shared
  contract (descriptor, system-space schema) and should run serially.

## Dependency and integration plan

The dates in the earlier Gantt were prototype estimates and are now stale.
Use this dependency graph for scheduling. Arrows mean an accepted interface
or gate. The M0 contract and a small trusted corpus unblock development;
full-corpus coverage is required before production promotion.

```mermaid
flowchart TD
    A["M0-A: harness contract + seed corpus"] --> B["M0-B: trusted baseline + CI"]
    A --> C["M1: observability / replay contract"]
    D["S0: audit complete"] --> E["S1.4: in-memory snapshot API"]
    A --> E
    E --> F["S2.2-2.7: algorithm prototypes"]
    C --> G["M3.1-3.3: IR prototypes"]
    A --> G
    G --> G2["M3.4: executable producer + lowering"]
    G2 --> H["M3.5: production fallback gate"]
    C --> H
    H --> H2["M3.7: feature flag"]
    P["Human gate: approve persistent IDs / formats"] --> Q["S1.1 + S2.1: persistent spaces"]
    E --> R["S1.5: memtx sampling"]
    R --> R2["S1.6: Vinyl bounded-work strategy"]
    E --> S0["S1.3a: volatile collector + candidate snapshot"]
    R --> S0
    R2 --> S0
    S0 --> S1["S1.2: ANALYZE grammar + volatile execution"]
    S0 --> S2["S1.3b: persist collection generation"]
    Q --> S2
    S1 --> T["S1.7-1.9: adapter + validation"]
    S2 --> T
    Q --> U["S2 persistence / where.c integration"]
    F --> U
    U --> V["S2 integrated"]
    T --> W["S1 integrated"]
    H2 --> X["E1: bounded-DP evaluation"]
    V --> X
    X --> X1["E1.1: configurable widths"]
    X1 --> X2["E1.2: subset/property partitions"]
    X2 --> X3["E1.3: property-aware dominance"]
    X1 -. "independent instrumentation" .-> X4["E1.4: per-statement solver metrics"]
    X3 --> X5["E1.5: corpus A/B"]
    X4 --> X5
    B --> Y["promotion parity gate"]
    X --> Z["GATE: enumerator decision"]
    Y --> Z
```

The in-memory S1 snapshot, S2 algorithms, and M3 IR/lowering prototypes are
independent tracks after their stated contracts; they can use isolated
worktrees. Engine samplers and S1.3a's volatile collector are independent of
persistent IDs, but must honor the shared sampler/snapshot contracts.
S1.3b persistence and any persistent ANALYZE behavior stay behind the human
IDs/formats gate. M3 can use fixed/current estimates while statistics are
built; E1 acceptance waits for integrated M3 and S2.

## Parallel work and integration ownership

Parallelism means separate branches or worktrees with one owner per shared
interface. Each track must define a narrow contract before others consume it;
integration runs serially. No simultaneous test-run.py or performance runs
against shared build directories, ports, or databases.

| Track | Can start after | Independent work | Serial integration point |
|-------|-----------------|------------------|--------------------------|
| M0 harness validity | now | isolated test execution, capture manifest, failure propagation | SCHEMA.md semantics and harness API |
| M0 corpus/CI | M0 harness contract | representative fixtures, coverage report, workflow wiring | CI gate and accepted snapshots |
| M1 observability | M0-A contract | counters, replay serializer, EXPLAIN surface | one owner for `sql.c`, grammar and path-class API |
| S1 statistics | S0 + M0-A contract | sampling adapters, immutable snapshot API, volatile collector/candidate builder | approved system-space IDs/formats, persistent publication, `sql.c` / `where.c` adapter |
| S2 algorithms | S1 snapshot contract | HLL, MCV, histogram and synthetic data | persistence schema and selectivity adapter |
| M3 planner | M1 path-class and replay contract | logical IR, physical IR, lowering tests using fixed stats | resolver/`where.c` routing and feature flag |
| E1 evaluation | S2 + M3 integrated | benchmark workload design and measurement tooling | `wherePathSolver` and decision report |

Schema IDs and persistent formats need explicit review before implementation.
Keep the owner of `where.c` integration singular at each merge point. Merge
small, testable slices; do not hold a long-lived branch until an entire
milestone finishes. Each track's changes must state the exact interface
version and acceptance evidence in its review.

---

## M0 — Parity Corpus & Baseline Harness

**Goal:** establish a queryable parity baseline so every later phase can be
gated on "no result regression, no diagnostic regression, and reviewed
planner-path changes."

**State:** `COMPLETE` for the M0-A/M0-B parity gate in this branch. Manifest
v1 capture is fail-closed. Policy v2 reviews all 401 tests / 802 engine
pairs: 588 included, 214 excluded with evidence, none pending. The accepted
anchor is `d8fc1e339b0bb8579c8b08e39d062e20edf66666`; the CI jobs use
that named full-corpus policy. A clean native build passed the full local
generated/CnP/LLVM matrix (298 memtx tests / 49,535 queries and 290 Vinyl
tests / 39,535 queries per mode), with zero hard or soft parity drift,
identical manifests, and exact generated repeat captures. The post-provenance
capture also matched exactly. Local exit criteria are satisfied; the first
hosted full-corpus CI result is an informational post-publication check, not
a reason to hold subsequent implementation work. Local workflow provisioning,
including a detached baseline worktree and pinned test runner, passed. The older
untracked 132,413-snapshot memtx-only tree is not the baseline. The
classifier covers all 401 current file identities, but its tags are file-level,
not verified per-query feature coverage. A post-anchor policy refresh records
15 new planner/volatile-ANALYZE regression files as evidence-backed exclusions
from immutable M0 snapshots; their dedicated memtx/Vinyl normal-runner tests
pass, while their mode-changing/diagnostic scope remains covered by focused
M3/S1 tests. No historical baseline was recaptured or broadened.

**Scope (B-light):** capture L1 result rows, L2 diagnostic, L3 path_class
per (test × engine), plus an external run manifest proving coverage and
outcome. Dispatcher dimension is a runtime parity check, not a stored
dimension. L7 latency goes to a separate perf-trail CSV. L4/L5 (plan shape)
are deferred to M3 when the descriptor exists naturally.

**Exit criteria:**

- every in-scope SQL test inventoried, with file-level feature tags and
  explicit query-level coverage only where verified;
- a reviewed inclusion/exclusion manifest identifies every test in the three
  SQL suites and records each suite/engine run outcome;
- accepted snapshots cover every runnable in-corpus test for its supported
  engine(s), with stable query identity and a passing recapture comparison;
- test failures, partial capture, missing snapshots, and unavailable
  dispatchers fail closed rather than producing a success-shaped diff;
- CI runs the real snapshot and dispatcher parity jobs on the defined corpus.

**Subtasks:**

- [x] **M0.1** Test auto-classifier — `test/sql-baselines/classify.lua`.
  Scans `*.test.lua` across sql / sql-tap / sql-luatest, regex-detects
  feature markers per `docs/vdbe/current_sql_feature_matrix.md`, writes
  `test/sql-baselines/classification.yaml` (401 entries on current HEAD).
  Landed at nextgen_sql `f8ae5a0ddd`, expanded at `02fdfbe2d3`
  (canonical_yaml.lua scalar coverage).
- [x] **M0.2** Snapshot harness — `test/sql-baselines/harness/run.lua`
  monkey-patches `box.execute`, dofiles the test file, writes one snapshot
  per captured query at `test/sql-baselines/snapshots/<suite>/<test>/q%02d.<engine>.yaml`
  per SCHEMA.md v1. Landed at nextgen_sql `4f8dc74e0b`. Verified end-to-end
  on a 7-statement mini SQL test.
- [x] **M0.3** Forensic L6 capture — when `--forensic` is enabled, the harness
  runs `EXPLAIN` and records a static VDBE program listing under
  `test/sql-baselines/forensics/`. This captures opcodes and operands without
  claiming the dynamic dispatch path; statements unsupported by `EXPLAIN` get
  an explicit comment-only capture. L6 remains advisory, not a parity gate.
- [x] **M0.4** Diff tool — `test/sql-baselines/diff.lua`. Compares two
  snapshot trees, classifies drift as RESULT-REGRESSION / DIAGNOSTIC-CHANGE
  / PATH-CLASS-SHIFT (currently a hard gate) or SOFT-DRIFT (advisory). Emits text /
  yaml / json; exit 1 on any hard-gate. Landed at nextgen_sql `a3c6dbf191`.
  Verified: identical inputs → 0 hard, 0 soft, all-MATCH exit 0; mutated
  row → RESULT-REGRESSION exit 1. L3 remains a strict gate for baseline
  comparison; M3 planner changes require an explicit reviewed exception.
- [x] **M0.5** CI: snapshot diff job — `.github/workflows/parity-corpus.yml`
  builds PR head and the named full-policy anchor, runs isolated captures on
  both engines, checks manifest coverage, then diffs via `diff.lua`. The
  baseline is a detached worktree from verified PR history, including fork
  PRs. Local workflow provisioning passed; hosted execution awaits publication.
- [x] **M0.6** CI: dispatcher parity job —
  `.github/workflows/dispatcher-parity.yml`. Matrix
  `{memtx, vinyl} × {generated, cnp, llvm}`. Compares CnP and LLVM outputs
  against generated; fails with `JIT-CORRECTNESS-REGRESSION:<dispatcher>` on
  any L1 or L2 divergence. It reads the accepted full policy; the complete
  six-way local matrix passed before promotion.
- [x] **M0.7** Perf-trail emitter — `test/sql-baselines/perf/emit.lua`
  records timing via `fiber.clock64()`, writes one CSV per CI run at
  `test/sql-baselines/perf/<YYYY-MM-DD>-<sha>.csv`. Accompanying
  `perf/aggregate.lua` reads a directory of those CSVs and emits a markdown
  trend table. `sample.csv` aligned with SCHEMA.md §L7 exemplar. Landed at
  nextgen_sql `b33719055e` + `1a9c7933b6` (gitignore).
- [x] **M0.8a / M0-A** Validate capture semantics on a small, representative
  seed corpus: use the normal test runner or reproduce its setup faithfully,
  preserve test failure and exit status, exercise generated/CnP/LLVM
  selection, and reject incomplete captures. Record a manifest with expected
  test/query counts and exclusion reasons. Do not accept the current
  untracked snapshot tree as ground truth. The harness contract and manifest
  format are serial integration points; fixtures and diagnostic probes may
  be built independently. Manifest v1, validator, runner-equivalent shared-
  engine seeds, typed/error result classes, SQL/SQL-TAP/luatest adapters,
  and six-way dispatcher capture are committed and verified.
- [x] **M0.8b** Reconcile SCHEMA.md and the tools: file-level versus
  query-level tags, query identity, result order, diagnostic codes,
  metadata that should not affect parity, and schema versioning. Verify
  round-trip and repeat-capture stability on the seed corpus. The v1
  contract is accepted; future semantic changes require versioning.
- [x] **M0.8c / M0-B** Capture the declared corpus under memtx and Vinyl
  where supported, report coverage and failures, and review snapshot size
  and storage strategy before committing any bulk baseline. Baseline against
  a named integration commit rather than the moving local `master` branch.
  The 770 engine decisions, full local six-mode matrix, exact repeat, and
  measured snapshot storage support the named integration anchor.
- [x] **M0.8d** Expand the real isolated seed CI invocations to the accepted
  corpus. Compare each PR head against the selected baseline with explicit
  coverage equality; run dispatcher parity on both engines where supported.
  Keep CI failing on a missing or empty corpus. Both jobs now read the full
  policy; their first hosted full-corpus run is pending branch publication.

---

## S0 — Statistics Audit

**Goal:** decide reuse/delete per file for the historical `_sql_stat1` /
`_sql_stat4` scaffolding before designing the new system spaces.

**State:** `COMPLETE` — audit landed as `docs/vdbe/s0_audit_report.md`
on `tsafin/llvm_jit` at commit `de57e09edd` (2026-06-21). Report finds the
historical `_sql_stat1` / `_sql_stat4` scaffolding is almost entirely
vestigial text: no `analyze.c`, no ANALYZE grammar in `parse.y`, and
`OP_LoadAnalysis` is a triply-registered no-op. 3 DELETE / 2
KEEP-AS-REFERENCE / 1 EXTRACT-HELPER / 11 REWRITE dispositions recorded.
S1 can start against this baseline.

**Exit criteria:**

- written inventory of disabled tests, no-op opcodes, dead helper functions;
- explicit decision per item (delete / keep-as-reference / extract-helper).

**Subtasks:**

- [x] **S0.1** Inventory disabled analyze tests in `test/sql-tap/suite.ini`
  and the dead bodies of `OP_LoadAnalysis` and friends.
- [x] **S0.2** Inventory `_sql_stat1` / `_sql_stat4` references in
  `src/box/sql/*` and decide per-file reuse/delete.
- [x] **S0.3** Write `docs/vdbe/s0_audit_report.md` summarizing findings,
  with explicit per-item disposition.

---

## M1 — Planner Observability & Replay

**Goal:** make the *current* planner's decisions inspectable, replayable, and
counter-gated before changing the planner.

**State:** `COMPLETE` for the v1 observability and selection-replay contract.
M1.1–M1.5 provide planner counters, path classes and stable fallback reasons,
structured summary EXPLAIN, a versioned detached snapshot, and replay of the
captured ordered final-path selection. M3.5's later producer-ledger audit
closes the reviewed route/reason coverage referenced by M1.1/M1.2. Replay is
intentionally selection-only: it validates and selects from a complete
captured final-path list; live access-path enumeration, dominance, and beam
pruning are not replayed. Unsupported captures remain diagnostic-only. Local
gates are complete; hosted CI publication is not a prerequisite.

**Exit criteria:**

- a captured planning decision can be replayed without live storage;
- `box.stat.sql()` exposes planner counters (candidates considered, fallback
  reasons, planning elapsed time);
- `EXPLAIN (planner = 'summary')` returns structured rows.

**Subtasks:**

- [x] **M1.1** Add planner counters to `box.stat.sql()` —
  `sql_planner_candidates_total` and `sql_planner_elapsed_us` are hooked to
  WHERE-planner candidate insertion and elapsed-time paths.
  `sql_planner_fallback_total` now increments for the production multi-
  relation and aggregate fallback paths. They publish stable
  `UNSUPPORTED_RELATION_COUNT` and `UNSUPPORTED_AGGREGATE` reasons through
  summary EXPLAIN; snapshot serialization preserves the reason.
  Per-reason counters are exposed as stable
  `sql_planner_fallback_<REASON>_total` fields and tested for both routed
  reasons. Candidate, elapsed, total fallback, and per-reason counter growth
  are validated by the SQL suites. Other fallback shapes are tracked in M3.5;
  `sql_statement_compiles_total` remains preparatory only.
  *parallel: no* (the fallback call site shares `where.c` with M3).
- [x] **M1.2** Wire path_class emission in current `where.c` — statements
  invoking the WHERE planner store `current_where_c` on the per-statement
  VDBE; summary EXPLAIN reads that value, and statements that do not invoke
  the planner report NULL. M0 snapshot capture consumes the versioned
  EXPLAIN snapshot through the M3.6 harness prototype. Multi-relation SELECTs
  now report `fallback` with a stable reason for multi-relation and aggregate
  shapes; remaining new-planner/fallback classes still require M3 dispatch.
  *parallel: no* (touches the same
  `where.c` files M3 will modify; coordinate).
- [x] **M1.3** `EXPLAIN (planner = 'summary')` grammar + executor returning
  structured rows per the planner_vm_migration.md schema. *parallel: yes*.
- [x] **M1.4** `EXPLAIN (planner = 'snapshot')` returns a versioned MsgPack
  selection-replay object for a supported canonical single-relation subset.
  The v5 envelope carries component-ledger-derived `path_class`/reason,
  per-statement measurements, ordered final-path diagnostics, and per-SELECT
  route records. When normalized
  query/schema/statistics, actual beam width, selector identity, and final
  paths all validate, it embeds internal v5 input and sets `replayable=true`;
  otherwise it remains diagnostic-only with no `replay_inputs`. The migration
  spec defines the minimum canonical, self-contained input and validation
  contract; it intentionally does not choose persistence IDs or formats.
  Focused SQL
  checks assert non-replayable legacy/fallback captures have no partial
  `replay_inputs`, and the corpus capturer rejects malformed v5 objects or
  incomplete route ledgers. An
  audit of the narrow M3 single-
  relation IR found it is not yet a safe source for a new replay envelope:
  logical nodes borrow resolved `Expr` / `ExprList` trees from the live
  statement and carry the catalog `space_id`; the physical planner receives
  access candidates from a caller-supplied provider, while the IR does not
  capture relation/index definitions or the exact statistics/configuration
  used to form those candidates. Serializing that object would therefore
  retain live compiler/catalog dependencies and omit planner inputs. The
  detached canonical model and extraction API now provide the supported
  single-relation subset; unsupported shapes still fail closed. Replay
  execution stays in M1.5. The M1.4 owned-value prototype models a normalized
  single-relation SELECT subset (predicate, projections, ordering, limit and
  offset), logical columns and index parts, and relation/index statistics
  with population, width, NDV, confidence, and freshness semantics, including
  per-index tuple-count semantics and definition version. Validation
  rejects incomplete definitions, invalid index ordinals, inconsistent stats
  absence, and malformed limit/order metadata. A deterministic internal
  MsgPack input format v5 emits fixed lexicographic map-key order, sorts
  logical indexes by key, and preserves semantically ordered columns,
  projections, ordering, and key parts. Reordered equivalent index inputs
  serialize to byte-identical valid MsgPack. Predicate, projection, and
  ordering strings are checked against the prototype's narrow canonical
  scalar-expression grammar, including column bounds; SQL text, unsupported
  calls, and malformed expressions fail closed. Relation/index
  schema-definition strings remain opaque and are not syntax-validated or
  normalized. `sql_replay_input_extract_select()` now connects a resolved
  single-relation `Select` to detached input creation: it canonicalizes the
  predicate/projection/order trees, records sort direction/default NULL order,
  and accepts only nonnegative integer literal LIMIT/OFFSET. A companion
  `sql_replay_input_extract_select_from_catalog()` derives detached
  relation/column/index definitions from the source catalog space using
  logical relation ordinal `r0`; storage index IDs remain only in a separate
  in-memory association. It rejects views, functional indexes, and multikey
  indexes whose identity is not modeled, and preserves repeated key ordinals.
  The catalog-only API keeps statistics absent; a snapshot-backed companion
  copies current-schema relation and index summaries from an immutable
  `SqlStatsSnapshot`, including visible/physical/estimated population
  semantics, population/NDV provenance, width, confidence, and freshness
  fields. Missing or stale relation summaries stay explicitly
  absent; malformed or non-integral cardinalities that replay input v5 cannot
  represent exactly fail closed. Fractional average row widths are retained
  exactly as finite doubles. Planner configuration remains caller-supplied.
  All extractors reject unsupported cursor bindings,
  expressions/functions, limits, and SELECT structure without returning
  partial input; outputs retain no live AST/catalog pointers. The owned model
  also optionally carries a provider-supplied ordered access-
  candidate list distinguishing primary-key point, index point/range/full, and
  table-full access, with logical index keys, canonical constraints, scan
  direction, projected logical columns, produced order, separate access-
  estimated and cost rows, and finite confidence/cost metrics. Rank order is
  preserved because planner tie-breaking may consume it; stable candidate keys
  must be unique. Missing provider output differs from a known empty set; a
  snapshot-backed extraction regression now asserts it remains unavailable,
  not present-empty, when no active planner producer supplied candidates.
  Access-loop candidate lists remain caller/provider supplied. The active SQL
  planner captures final retained one-relation `WherePath` lists and combines
  them with detached normalized SQL/schema/stats input, selector identity, and
  effective beam width. Supported snapshots are v5 selection-replayable with
  embedded v5 input; unsupported shapes, joins, and malformed/stale stats
  remain diagnostic-only. The live `whereLoopInsert()`
  hook (`src/box/sql/where.c`) calls `sql_record_planner_candidate()`, whose
  VDBE-facing data is aggregate candidate/path counters, not normalized
  candidate identities, constraints, estimates, ordering, or costs. Therefore
  neither a complete access-loop candidate list nor a known-empty list can be
  asserted from that hook. This does not limit the separate selection-only
  final-path capture. A focused internal bridge now
  models provider outcomes explicitly as `UNAVAILABLE`, `COMPLETE`, or
  `INCOMPLETE`. It preserves the existing unavailable-vs-known-empty encoding,
  deep-copies only a complete provider list into the owned input, and returns
  `INCOMPLETE` with no result object when a provider reports a partial prefix.
  Unit tests cover all three outcomes and reject payload attached to an
  unavailable state. The configured `sql_replay_input.test` target rebuilds
  and passes, including all 19 candidate-provider checks. This validates
  transport/completeness-state handling
  only: the bridge cannot prove that a caller labeling its list `COMPLETE`
  actually enumerated every viable access candidate, and no active planner
  producer calls that access-loop bridge. A separate live final-path producer
  now captures ordinary one-relation post-beam `WherePath` alternatives, but
  does not claim the access-loop list is complete. Selection replayability is
  determined by the final-path/input capture, not that out-of-scope list. A
  bounded staging API remains available for a future enumeration experiment.
  fixed candidate storage, explicitly mark enumeration start/normal completion,
  append ordered values, or poison the capture on an unsupported shape.
  Capacity overflow and premature completion publish only `INCOMPLETE`; an
  unstarted producer remains `UNAVAILABLE`, distinct from a started,
  completed empty enumeration. Tests verify that no partial prefix escapes.
  This is transport-side all-or-nothing enforcement, not a `WhereLoop`
  producer: source audit finds `whereLoopInsert()` (where.c:1500) called before
  dominance replacement and for OR-subclause loops as well as ordinary
  candidates; it receives no relation-completion signal. Although the final
  `WhereInfo.pLoops` list is pruned, converting it requires per-loop
  `aLTerm`/`index_def` normalization, stable logical index mapping, projected
  columns and produced order, plus the exact statistics/configuration used by
  planning. The existing resolved-SELECT canonicalizer is not a `WhereLoop`
  constraint adapter, and preparation does not retain authoritative planner
  config or immutable statistics provenance alongside that list. Until those
  inputs and a post-enumeration completion boundary exist, the active planner
  cannot safely label a candidate list complete. The follow-up boundary audit
  in `planner_vm_migration.md` identifies successful `whereLoopAddAll()` return
  as the ordinary enumeration boundary (the retained `WhereInfo.pLoops`, before
  path selection); the one-table shortcut is a separate route and planner
  errors must discard capture. It also confirms that `wherePathSolver()` reads
  the actual, environment-configurable width at runtime, while replay
  extractors still accept algorithm/config versions as caller arguments. A
  future prepare-owned capture must record that actual width and a maintained
  algorithm identity alongside detached candidates and immutable stats
  provenance. Replay scope is now fixed to **selection only**, conditional on
  a complete, ordered final-path set captured by the live planner; M1.4 does
  not claim to rerun or validate enumeration. Access-loop enumeration remains
  outside scope. The v5 envelope embeds the canonical v5 selection input when
  its captured selector identity/configuration and path list validate.
  `planner_vm_migration.md` records the scope boundary and Mermaid flow.
  *parallel: yes*.
- [x] **M1.4/M1.5 final-path selector prototype.** A versioned detached
  selector now reproduces the final reduction in `wherePathSolver()` over an
  explicitly captured, post-beam final-path list: compare exact signed
  16-bit `LogEst` `rCost`, replace only on strict improvement, and therefore
  retain the first path on equal cost. It is deliberately not a min-cost
  selector over access loops and does not rerun enumeration, dominance, or
  beam pruning. Internal replay input now serializes ordered final-path
  fingerprints, exact path/unsorted/output LogEst values, `isOrdered`, and the
  one-relation reverse-scan mask, plus selector identity; the internal format
  advances to v5. Tests cover unique-min reorder stability, first-on-tie input
  order, captured ORDER BY/reverse metadata, unavailable versus known-empty
  final paths, and selector identity. Access-loop candidates alone no longer
  pass replay readiness. A live producer now captures the final retained
  `aFrom` paths after `wherePathSolver()` completes, including replacing the
  preliminary capture with the final ORDER BY cost pass. It preserves list
  order and emits all paths or none, with stable fingerprints, exact `LogEst`
  values, ordering, and reverse-scan metadata. The v5 envelope exposes
  `final_path_status`, `final_paths`, and the live selected fingerprint; joins and any route
  without a successful supported solver capture remain unavailable, while
  overflow and ambiguous fingerprints fail closed. Supported single-relation
  inputs now combine canonical SQL/schema/stats metadata with the exact ordered
  candidates, selector/config identity, and beam width. Runtime coverage
  verifies the embedded v5 input matches live candidates and the selected
  fingerprint equals strict-min selection. The M0 harness validates replayable
  and diagnostic-only v5 forms. The detached selector plus live capture slice
  is complete; the standalone selection replay consumer is tracked in M1.5.
  The live fingerprint now uses schema-scoped `(space_id, index_id, type,
  uniqueness)` identity without dereferencing `index_def->key_def`: transient
  legacy planner loops may carry an invalid nested key-definition pointer, and
  that field is not owned by the replay capture record. Focused
  `colname.test.lua` capture passes on both engines after this change.
  Final-path storage is now allocated only when snapshot EXPLAIN captures a
  candidate list, rather than reserving 64 records in every VDBE; cache-size
  estimation includes the dynamically allocated capacity. This restores the
  prepared-statement footprint for ordinary statements while retaining exact
  replay capture. Both local and remote `prepared.test.lua` variants pass.
  *parallel: yes; selector prototype is independent of live producer capture.*
- [x] **M1.5** Developer-only `sql_replay` API consumes a v5 snapshot artifact
  (and retains v4 read compatibility),
  validates its embedded v5 selector/version and final-path order against the
  external diagnostics, applies the strict-min / first-on-tie selector, and
  reports both the replayed fingerprint and whether it matches the captured
  winner. The API does not inspect live schema, data, catalog, or statistics;
  enumeration, dominance, and beam pruning remain explicitly out of scope per
  the selection-only replay decision. Runtime coverage captures a snapshot,
  drops the source table, replays from the saved artifact, and verifies the
  captured winner; an internally inconsistent candidate list is rejected.
  Internal selector tests cover input-cost changes and tie behavior.
  Diagnostic-only v5 envelopes remain non-replayable. This closes the M1.5
  selection-replay consumer slice, not full enumeration replay.
  *parallel: yes*.

---

## S1 — Relation / Index / Cardinality Stats

**Goal:** the current `where.c` stops using `default_tuple_est[]` for ordinary
tables, and instead consumes real per-relation cardinality and average row
width.

**State:** `PROTOTYPE`. S1.4 has an immutable, deep-copying, reference-counted
in-memory snapshot API with schema-staleness checks and a bounded allocation
budget; S1.5 has a bounded memtx sampling prototype; S1.6 now has a bounded,
fail-closed Vinyl sampler prototype. None is attached to prepare/ANALYZE or
populated by a collection job. `sql.c` accepts an optional immutable snapshot
and legacy index cardinality estimates consume it when present, falling back
on missing/stale data; `whereRangeScanEst()` still uses its heuristic
reduction over the adapted base estimate. Persistent collection,
prepared-statement snapshot ownership, ANALYZE, and complete adapter
validation remain open. The system-space schema remains DRAFT pending human
review of IDs and formats. On 2026-09-27 the user reconfirmed that the schema
stays DRAFT and that work should continue on independent tracks; no ID or
format approval is implied.

**Exit criteria:**

- `ANALYZE [table]` accepted by the parser, runs a collection job, persists
  results;
- `index_field_tuple_est()` reads from the new snapshot, not from defaults;
- selectivity/cardinality error on the agreed corpus improves;
- no regression under stale or missing stats (fallback to defaults).

**Subtasks:**

- [ ] **S1.1** Define new system spaces (`_sql_stats_relation`,
  `_sql_stats_index`) with versioned MsgPack payload format. Write a
  short `docs/vdbe/sql_stats_schema.md`. The spec remains **DRAFT** by explicit
  direction; no system-space IDs or payload-format choices are approved.
  *parallel: no* (system-space allocation is a one-way door — needs human
  sign-off).
- [x] **S1.2** Re-enable `ANALYZE` grammar and execute the volatile collection
  path without persistence. Remove the `unsupported ANALYZE` rejection only
  after S1.3a defines complete candidate-snapshot publication semantics.
  *parallel: yes, after S1.3a*.
  **Implementation status (2026-09):** the shared-view context now has a
  batch candidate assembler for complete relation/index spec sets. It checks
  exact flattened target coverage, duplicate relation/index ownership, and
  aggregate request, staging, temporary-memory, and scan/estimator work limits
  before sampling. Relation scans are sequential; summaries and reservoir
  memory are bounded in aggregate, with the largest reservoir charged because
  it is destroyed before the next relation. Detached relation parts remain
  private to the batch and are combined once; the existing publisher accepts
  only that final candidate. A later relation failure exposes no candidate and
  cannot replace the installed snapshot. The focused collection unit target
  passes, including work-budget rejection before extractor invocation and the
  prior repeated-assembly rejection/preservation case. A TEST_BUILD memtx-then-
  Vinyl runtime test now verifies a two-relation candidate, one publication,
  later-relation extractor failure, and installed pointer/content preservation;
  it passes locally. The shared-view batch can also opt into the native index
  hash adapter where no canonical extractor is supplied; unsupported key
  definitions remain fail-closed. Native output is labeled as 32-bit hash
  equivalence-class NDV (with collision-risk confidence discount), not exact
  SQL NDV. The TEST_BUILD memtx/Vinyl test checks this provenance; candidate
  validation separately requires index and relation population bases to match,
  while allowing the NDV basis to identify the hash domain. Both focused
  collection unit and SQL runtime targets pass locally. The shared
  view and candidate API are volatile only.
  **S1.2 status (2026-09):** SQL discovery, grammar, and the VDBE execution
  operation are now wired to the volatile batch builder. Bare `ANALYZE`
  discovers all eligible targets and publishes one complete candidate;
  named `ANALYZE table` builds one relation and replaces only that relation in
  a same-generation snapshot. Both forms retain the exact installed snapshot
  on failure. The focused TEST_BUILD runtime test covers bare memtx+Vinyl
  collection, named relation replacement, system-space no-op, missing/view
  errors, unsupported-index atomic failure, and row counts. The focused test
  and `sql_replay_input.test` pass locally. No persistence has been added.
  This does not authorize persistence choices;
  S1.1's schema remains DRAFT. The compatibility baseline from the historical
  `sqlAnalyze` implementation is now source-audited: bare form visits
  non-system, non-view spaces and all indexes; named missing-space and view
  targets error. The volatile contract now explicitly preserves named
  system-space no-collection behavior; excludes data-temporary spaces from
  bare enumeration and rejects them when named; and fails the complete batch
  for unsupported engine/index targets rather than silently omitting them.
  These choices and their limitations are recorded in
  `sql_stats_sampling.md`. This closes volatile SQL discovery/execution and
  rollback coverage only; persistence remains unimplemented. Bare `ANALYZE`
  with no eligible targets is a successful
  no-op with no publication. Per the user decision (2026-09-27), the first
  implementation uses fixed conservative compile-time sample/work/memory
  ceilings and fails atomically on exhaustion; no session-configurable limits
  are introduced. The exact ceilings are enumerated in
  `sql_stats_analyze_budget.h` and `sql_stats_sampling.md`; the production
  analyzer consumes these per-index and aggregate ceilings. The focused
  `analyze_volatile_test.lua` runtime suite and `sql_stats_collection.test`
  unit target pass against the current build. A TEST_BUILD regression now
  exceeds the 256-index-request ceiling through bare discovery after first
  installing a valid snapshot; ANALYZE fails before publication and the prior
  snapshot remains unchanged. The volatile SQL integration test injects an
  unsupported R-tree index into both named and bare collection requests and
  verifies each failure leaves the installed snapshot unchanged; the TEST_BUILD
  runtime suite passes locally.

  ```mermaid
  flowchart TD
    A[ANALYZE statement] --> B{Bare or named?}
    B -- Bare --> C[Enumerate persistent non-system, non-view spaces]
    C --> Z{Any eligible targets?}
    Z -- No --> Z1[Successful no-op / no publication]
    Z -- Yes --> D[Include all indexes]
    B -- Named --> E[Resolve space name]
    E --> F{Missing or view?}
    F -- Yes --> X[Return SQL error]
    F -- No --> G{Named target category?}
    G -- System --> H[No collection / no publication]
    G -- Data-temporary --> X2[Fail closed / no publication]
    G -- Persistent --> I[Include all indexes]
    D --> J[Open one shared read view]
    I --> J
    J --> K[Build one complete detached candidate]
    K --> Q{Any target or index unsupported?}
    Q -- Yes --> R[Fail whole request; preserve installed snapshot]
    Q -- No --> L{Bare or named?}
    L -- Bare --> M[Use complete batch candidate]
    L -- Named --> N[Replace one relation in prior snapshot]
    M --> O[Publish once]
    N --> O
    K -- Any failure --> P[Keep exact installed snapshot]
    O -- Revalidation failure --> P
  ```
- [x] **S1.3a prototype** Volatile collection core — consume sampled tuples, build and
  validate relation/index summaries, then atomically publish one immutable
  candidate snapshot. No persistence or grammar dependency; test rollback on
  any incomplete/invalid relation or index summary. *parallel: yes*.
  **Status note:** complete as a volatile one-relation prototype. A normalized
  result and pure completeness validator build a detached, deep-copied snapshot
  candidate. Caller-defined width/population/confidence provenance tokens
  plus the width denominator count are retained without selecting estimator
  policy; relation/index/prefix completeness (including duplicate result-ID
  rejection) and catalog/schema/visibility/index-definition generations are
  checked. A narrow bridge now converts a known engine-sampler population
  into exact relation cardinality without mistaking delivered draws for the
  population; a second bridge exposes fractional sample-average serialized
  tuple width with its row denominator, without truncating the mean or
  inventing width for an empty sample. The standalone helpers are now composed
  by the shared-view candidate builder and publisher described below. A new
  `sql_stats_index_summary` unit API now
  computes sampled prefix-NDV values through HLL from a caller-provided
  canonical SQL-value extractor. It counts delivered rows/bytes, bounds
  accumulator memory, and suppresses output on extractor failure; it
  deliberately does not hash raw MessagePack. A separate bounded helper now
  inverts the uniform-occupancy model for both memtx independent draws and
  Vinyl reservoir samples, returning rounded population-prefix NDVs plus a
  confidence evidence score that accounts for sample coverage, HLL precision,
  and native 32-bit hash collision risk. Unknown/inconsistent populations fail
  closed without partial outputs; empty populations produce zero NDVs and a
  census returns observed HLL NDV without extrapolation. The estimator's
  equal-frequency assumption and confidence semantics are documented in
  `sql_stats_sampling.md`; ten focused tests exercise both sampling designs,
  a complete census, a low-coverage skew probe, provenance rejection, and the
  temporary-memory and work bounds. `sql_stats_collection_index_from_sample()`
  now converts one bounded index summary plus its engine sample into a detached
  collection record with caller-supplied visibility and definition tokens.
  `sql_stats_collection_build_sample_candidate()` assembles all expected index
  summaries for one relation into a detached candidate atomically. It requires
  exact common population counts, caller-supplied relation confidence and
  provenance, and only returns per-index confidence outputs after full
  candidate validation. Two-index success, empty-population exact-zero/no-width,
  and population-mismatch no-partial-output cases pass in the six-check
  `sql_stats_collection_samples.test` target. The helper does not independently
  verify each summary's index association or establish that caller-supplied
  visibility tokens represent one shared engine snapshot. The separate
  shared-view context now validates that association and publishes the exact
  candidate; ANALYZE/job wiring is tracked under S1.2. There is no agreed
  corpus validation for the
  distributional assumption. The native index-hash
  adapter supports
  verified STRING, DOUBLE, BOOLEAN, UNSIGNED, and signed INTEGER parts for
  TREE/HASH definitions. INTEGER uses canonical MessagePack values (negative
  values as MP_INT, nonnegative as MP_UINT), matching its numeric comparator;
  the native test verifies duplicate negative values deduplicate and positive
  values remain distinct. A dedicated
  `sql_stats_index_summary_native.test` target checks unsigned acceptance,
  duplicate deduplication, distinct-value recognition, and signed INTEGER
  hashing; all five checks pass locally. The native adapter
  now deletes its unreferenced runtime tuple returned by `tuple_new()` rather
  than decrementing a reference it does not own. The existing `key_def.test`
  native-adapter checks now delete their locally owned fixture tuples
  directly. A focused runtime luatest also exercises the
  owned READ_CONFIRMED context against real memtx and Vinyl primary and
  secondary indexes; `sql_stats_test` passes locally. Neither slice provides
  a common cross-index visibility boundary or a complete candidate. An
  earlier attempt to load these helpers as a separate DSO failed on private
  engine symbols (`space_cache_version` / `mp_type_hint`); that attempt is
  superseded by the TEST_BUILD in-process wrapper described below, which
  provides live sampler confirmation without adding production exports. The
  collection unit now sweeps the snapshot byte budget from immediate rejection
  through the first complete deep copy, releasing candidates and checking that
  every incomplete budget fails closed. Snapshot unit tests also inject a
  one-shot failure at every deep-copy allocation point and verify fail-closed
  cleanup until complete construction succeeds; the hook is compiled only
  into that unit target. The collection unit separately injects failures at
  both staging-array allocations and verifies no candidate is returned before
  a complete build succeeds. These tests close detached builder coverage. The
  shared-view builder and publisher below now derive complete one-relation
  stats from canonical sampled values and capture/revalidate catalog/schema/
  index identity plus the data-view generation before one atomic install;
  failure preserves the prior snapshot. The new opt-in core `read_view` supplies
  a volatile memtx/Vinyl visibility cut for selected indexes. Candidate
  assembly/publication now uses the shared-view context; the older
  READ_CONFIRMED transaction context remains a separate prototype. The engine
  samplers can now be called
  over multiple requested indexes using the same caller transaction/read view;
  Vinyl runtime coverage verifies an uncommitted tuple appears in both primary
  and secondary samples. A new reusable context now owns a core `read_view`,
  records its engine-assigned ID and schema version, validates requested
  indexes, and scans pinned indexes into a bounded reservoir. Its unit tests
  cover ownership and fail-closed open cases; `sql_stats_collection.test`
  passed locally, including pinned scan, tuple-budget, and schema-drift
  rejection cases. The core context pins data through memtx and Vinyl
  primary/secondary full-scan views. Its new single-relation candidate builder
  derives all index summaries from that view, validates captured
  catalog/schema/index generations, and publishes only the exact assembled
  candidate while the commit-vclock signature remains unchanged. A held
  candidate is rejected after a committed write without replacing the
  installed snapshot; an undersized aggregate staging budget also returns no
  candidate and preserves installed state. This closes the
  shared-view-to-candidate prototype slice, not production ANALYZE. Core
  read-view allocation has no explicit
  resource budget, and modification epoch/confidence/extractor policy remain
  caller supplied. A separate
  `sql_stats_tx_context` now owns a box transaction, sets READ_CONFIRMED before
  sampling, validates transaction ID/isolation/schema/index definitions, and
  bounds/stages each requested index sample before delivery. Its finish commits
  only after every requested target succeeds; otherwise it rolls back. The
  focused `sql_stats_collection.test` target builds and passes locally,
  including all 32 transaction-context checks. The production `tarantool`
  target links with the API. A prior runtime luatest exercised live memtx/Vinyl
  primary- and secondary-index samples. A test-only wrapper now compiles into
  the server process under `TEST_BUILD`, registers its Lua modules at startup,
  and calls the private context against the live engine registry without adding
  exported symbols. The wrapper was corrected to decode positive MessagePack
  integer keys as unsigned values. The full focused `sql_stats_test.lua` suite
  passes in a TEST_BUILD Clang-19 binary; with `TEST_BUILD=OFF`, the suite also
  passes while marking its two wrapper-dependent live checks skipped. `nm`
  confirms that the ordinary server binary does not contain either test entry
  point. The earlier DSO loader failure is therefore avoided without exporting
  engine internals or linking a second box archive. The unit test still uses
  engine stubs for lifecycle/error injection; live wrapper coverage currently
  verifies sampling only, not complete candidate publication or shared
  cross-engine visibility.
  The context now captures the local commit-vclock signature and rejects
  sampling/finish if it changes, with unit coverage for drift during engine
  sampling and again at finish. It now also captures and revalidates the local
  catalog generation (`box_catalog_version()`) alongside schema version and
  per-index unique IDs; tests inject catalog-cache drift both during sampling
  and at finish, and the catalog token is exposed to the volatile collector.
  These are local volatile generation guards, not durable/cross-node snapshot
  identities. `READ_CONFIRMED` excludes prepared/unconfirmed writes but does
  not itself freeze confirmed commits. A completed earlier sample
  can reach the caller's off-side staging sink if a later target fails, so the
  caller must discard all staging unless the full collection validates. This
  owned transaction context now exposes
  `sql_stats_tx_context_finish_and_publish()`: it requires the expected
  relation/index set to match every sampled target and captured runtime index
  identity, builds the complete detached candidate, commits only a complete
  sample set, revalidates schema/catalog/vclock generation after commit, then
  synchronously installs through `sql_set_stats_snapshot()`. Any build,
  validation, commit, or generation failure leaves the previously installed
  snapshot untouched. Focused unit tests cover target-definition mismatch,
  commit failure, commit-time generation drift, stale-generation preservation,
  and successful publication. This closes the local publication step, not
  complete summary derivation or live reader/engine integration. The immutable
  snapshot validator now accepts zero distinct-prefix counts only for an
  index with zero tuples, matching the schema draft's empty-index encoding;
  nonempty indexes still reject zero NDV. Focused snapshot tests cover both
  sides, and collection now builds a complete empty-relation candidate with
  exact zero rows and prefixes while leaving average width explicitly absent.
  The collection unit test verifies that no width denominator is fabricated.
  The collection unit target passes the new publication/old-snapshot-preserve
  checks. The production target plus replay-input unit target pass. Full
  relation/index summary derivation from sampled values and live collection
  wiring remain open; `ANALYZE` stays disabled. See
  `sql_stats_sampling.md` for the exact contract and local unit evidence. A new
  `sql_stats_tx_context_build_sample_candidate()` binds every requested target
  to an expected index, request, and canonical extractor; owns per-index
  summaries; derives relation population/width from a designated sampled index;
  and returns a detached candidate only after all samples and conversions
  succeed. Its focused transaction-context checks cover reordered,
  missing, duplicate, and later-extractor-failure cases, while asserting that
  the installed snapshot is unchanged. A dedicated
  `sql_stats_tx_context_finish_sample_candidate_and_publish()` now accepts
  only the exact candidate retained by this context, commits its owned
  transaction, revalidates local schema/catalog/vclock generations after
  commit, and installs that same immutable candidate. The context and caller
  retain separate snapshot references. Candidate mismatch, repeated assembly,
  commit failure, and post-commit visibility drift preserve the old installed
  snapshot. The 10-check `sql_stats_collection.test` target passes in the root
  Clang-19 build. Live engine-sampler runtime coverage now runs through
  test-only wrappers linked into the server under `TEST_BUILD`; the focused
  `sql_stats_test.lua` suite passes and samples primary and secondary indexes
  on memtx and Vinyl. `TEST_BUILD=OFF` also passes the suite while explicitly
  skipping its two wrapper-dependent live cases, and symbol inspection confirms
  that the production-configured server has no linked wrapper entry points.
  A TEST_BUILD-only live assembler now drives the complete candidate path with
  canonical UNSIGNED extraction and both memtx and Vinyl primary/secondary
  indexes. Direct server-runtime checks build and publish the candidate for
  each engine, then confirm relation/index populations of 8 and a width
  denominator of 4 from four delivered sample rows; snapshot cleanup follows
  each case. The runtime regression also asserts four canonical extractor
  calls per index and zero extraction errors. This verifies the older
  transaction-based candidate path, not a common cross-engine visibility
  boundary. Local READ_CONFIRMED and vclock/catalog/schema checks
  are not durable or cross-node snapshot claims. A source audit committed as
  `a68268c9a4` records the pre-implementation gap: neither core `read_view`
  nor READ_CONFIRMED then pinned one shared memtx/Vinyl cut. That gap is now
  closed for the volatile core read-view boundary, full-scan index adapter,
  and one-relation candidate assembly/publication. Production ANALYZE remains
  open.
  **Update (2026-09):** the core read-view API now passes each engine's pinned
  view to selected index views; Vinyl is opt-in and pins one committed VLSN
  for full-scan iterators. A TEST_BUILD held-view commit regression opens one view over
  memtx and Vinyl relations with primary/secondary indexes, commits delete
  and insert changes, and confirms all four scans remain `{1..8}` until that
  view closes; reopening yields `{3..10}`. The Clang-19 TEST_BUILD target and
  direct runtime invocation pass. The integrated focused `sql_stats_test.lua`
  and `read_view_test.lua` luatests plus the `sql_stats_collection.test` and
  `sql_stats_collection_samples.test` unit binaries pass in the root build.
  These passed via focused test-run or direct unit-binary invocation in the
  root Clang-19 build; an earlier isolated-worktree startup failure with a
  Fiber GC leak report and no backtrace frames did not
  reproduce here. The shared-view candidate builder now runs against memtx and
  Vinyl and stale-after-commit publication fails closed; production ANALYZE
  wiring and target discovery remain open. Fixed budget policy is decided;
  runtime wiring must still enforce it atomically.
  The persistence schema remains
  DRAFT; no IDs or formats changed. See `sql_stats_sampling.md` for runtime
  details.
- [ ] **S1.3b** Persist collection generation transactionally after S1.1 review.
  This is the persistence half of S1.3 and must not start before human approval
  of system-space IDs and tuple/payload formats. *parallel: no*.
  The sampler boundaries are implemented, but engine population semantics
  are not interchangeable: memtx `index_size()` subtracts active-transaction
  invisible tuples, whereas
  Vinyl `index_size()` is an approximate LSM-statement count that may include
  obsolete versions/tombstones. Vinyl visible population requires successful
  exhaustive EOF, so budget exhaustion must fail collection closed. See
  `sql_stats_sampling.md` for the volatile candidate-builder contract; see
  `sql_stats_schema.md` for the explicitly unapproved persistence proposal.
- [x] **S1.4 prototype** `SqlStatsSnapshot` API — immutable deep copy,
  reference-counted ownership, catalog/schema versions, relation/index
  cardinalities, confidence and freshness metadata, stale/missing lookup
  states, and a caller-specified memory budget. Unit tests cover deep copy,
  lifetime, schema mismatch, invalid values, and budget rejection. This is
  not yet attached to prepare/prepared-statement lifetime; that integration
  remains part of S1.7. *parallel: yes*.
- [x] **S1.5 prototype** `engine_sql_stats_sample` dispatch and a bounded,
  seeded memtx sampler exist in `src/box/sql/sql_stats_sample.{h,c}`. It uses
  requested-index random access with replacement in the caller's active
  transaction and reports delivered rows/bytes; focused unit tests validate
  the bounded loop, and the production target compiles. A runtime engine-
  dispatch test now covers memtx transaction visibility, deterministic draws,
  row/byte limits, sink accounting, and unsupported/invalid inputs. The result
  also reports the transaction-visible requested-index population from memtx
  `index_size()`, including uncommitted writes; runtime checks cover nonempty
  and empty transaction states. It is not
  wired to ANALYZE/collection and does not create an independent read view;
  these gates remain open.
  *parallel: yes*.
- [x] **S1.6 prototype** Vinyl now has a bounded exhaustive requested-index
  sampler using the caller's transaction/read view when active (including
  own writes), or a short-lived autocommit view otherwise, and seeded
  Algorithm R reservoir selection without replacement. Standard Vinyl
  iterator read tracking applies in active transactions and may affect later
  conflict outcomes. The sampler publishes no callbacks before successful EOF and
  fails closed on tuple, buffer, disk-source, uncached-page, key-advance, or
  iterator failure. Separate `max_bytes` (payload) and `max_buffer_bytes`
  (metadata + slots + copies) limits bound retained memory. The tuple cap must
  leave room to observe EOF; key steps include tombstone skips and terminal
  EOF. Focused unit/runtime tests cover deterministic no-replacement output,
  fixed-fixture frequency smoke checks, visibility after update/delete/
  compaction, and exhaustion paths. The production `tarantool` target and
  test module compile locally; `sql_stats_sample.test`,
  `vy_iterator_budget.test`, `vy_point_lookup.test`, and the standalone Vinyl
  runtime guard pass. Both storage samplers accept a requested index ID;
  runtime tests cover memtx secondary-index random draws and Vinyl secondary
  iteration with visible-primary-tuple resolution under the shared work
  budget, including fail-closed key-, source-, and page-budget exhaustion.
  Vinyl runtime coverage also samples a secondary index in the caller's
  active transaction and observes its uncommitted insert, matching the primary
  index sample's visible population.
  The test module reads
  its optional failure argument
  before constructing the result table, and the runtime fixture uses the
  nested update-operation shape required by Vinyl. This does not yet integrate
  ANALYZE or a collection job, calibrate confidence, or establish workload-
  level latency / read-amplification limits; therefore it is a prototype, not a completed
  statistics sampler. See `sql_stats_sampling.md`. *parallel: yes, against
  the S1.5 contract*.
- [x] **S1.7** Compatibility adapter — `index_field_tuple_est()` and
  `whereRangeScanEst()` consume snapshot, fall back to defaults on absence.
  `sql_set_stats_snapshot()` now installs a retained immutable snapshot in the
  SQL core; `index_field_tuple_est()` and `sql_space_tuple_log_count()` use
  schema-validated relation cardinality and average rows-per-index-prefix
  (index tuple count divided by matching prefix NDV, for sparse-index safety)
  estimates when available, preserving the legacy estimates on missing/stale
  relation/index data. `whereRangeScanEst()` applies its existing reduction
  to the resulting snapshot-backed input cardinality; S2 histogram range
  integration is not implied. Unit coverage exercises relation/prefix
  estimates, distinct relation/index tuple populations, stale schemas and
  missing indexes (both leave caller output untouched for legacy fallback),
  and definition-length mismatch,
  and the production SQL target links the snapshot API. A test-only C module
  now installs an immutable fixture through `sql_set_stats_snapshot()` and a
  live SQL luatest compares relation and index-prefix estimates before/after
  install, after clear, for a missing prefix, and for a stale schema. The SQL
  result is checked unchanged. Separate EXPLAIN statements compiled with a
  hot-key versus unique-key fixture also report the corresponding higher vs
  lower estimated row counts through the live `where.c` path; a range EXPLAIN
  confirms `whereRangeScanEst()` scales down when snapshot relation
  cardinality replaces the legacy default. On a near-uniform three-key SQL
  fixture, the measured equality estimate also lowers q-error against the
  actual returned-row count relative to the legacy estimate. Replacing the
  snapshot also expires cached VDBEs so the same SQL text is recompiled using
  the new estimates. Every VDBE now retains the immutable snapshot generation
  present at VDBE creation and releases it with VDBE destruction; a reprepare
  receives the then-current generation. A TEST_BUILD-only adapter compiles
  statements against catalog generations 101 and 202, replaces the installed
  provider between prepares, and verifies the older statement still owns
  generation 101 while the new statement owns 202. The focused runtime test
  passes. The WHERE candidate cardinality and automatic-index cost adapters,
  plus snapshot-EXPLAIN extraction, now read the VDBE-pinned generation;
  non-statement consumers retain the installed-provider wrappers. Both the
  snapshot estimate-adapter and statement-generation luatests pass locally.
  No collection path populates the provider, and estimates are not yet
  measured against actual SQL-corpus cardinalities. *parallel: no* (touches
  `where.c` integration surface).
- [ ] **S1.8** Re-enable disabled `analyze*.test.lua` tests, validate they
  pass. The volatile execution contract is now implemented under S1.2, but
  these compatibility suites also assert legacy `_sql_stat1` / `_sql_stat4`
  contents and historical diagnostics. Re-probing `analyze1.test.lua` with
  only that suite entry temporarily enabled confirms parsing now succeeds,
  while the test still fails on legacy case-sensitive error text and queries
  `_sql_stat4`, which intentionally does not exist under the current draft
  persistence gate. The suite config was restored after the probe. No safe
  file-level subset has yet been demonstrated; keep the suites disabled until
  the persistent format is approved and implemented, then adapt only where
  the approved contract preserves behavior. Do not map these tests onto the
  draft persistent schema. *parallel: yes*.
- [ ] **S1.9** Add synthetic uniform / skewed validation cases to the M0
  corpus, gate q-error improvement. The lower-level estimator already has
  passing synthetic unit probes for uniform equality/median-range and
  duplicate-heavy skewed MCV/residual/range q-error (`test_uniform_and_skewed_qerror`
  in `test/unit/sql_stats_selectivity.c`). These validate estimator behavior
  with supplied summaries, but do not collect statistics from the M0 SQL
  corpus or establish planner q-error improvement; S1.9 remains open pending
  reviewed-corpus integration and an accepted q-error criterion. The M0
  snapshot contract captures executed SQL results and planner path diagnostics;
  it has no field for estimated cardinalities or actual-vs-estimated rows.
  The standalone selectivity unit probes still use supplied summaries and are
  not SQL collection evidence. Writing their synthetic values as ordinary M0
  snapshots would falsely imply executed planner measurements. As a bounded
  validation improvement, the uniform unit
  fixture now checks q-error at all 20 histogram bucket edges (<= 1.05), while
  the separate skewed workload fixture checks each distinct CDF boundary
  (<= 1.06). Both remain algorithm-only probes. Closing S1.9 still requires a
  reviewed corpus workload integration through a real stats provider, skewed
  MCV planner validation, and an accepted q-error gate. The stage-matched
  estimate/actual JSONL sidecar and analyzer
  contract are now specified in `test/sql-baselines/E1_WORKLOAD.md`; they
  remain separate from M0 snapshot v1. A reproducible TEST_BUILD sidecar
  producer is now available at `test/sql-baselines/e1_sql_producer.py`; it
  executes ten prepared single-table predicates before and after named SQL
  `ANALYZE` collection and records matching `select-output`
  estimate/actual cardinalities with source, binary, data, and per-configuration
  statistics provenance. Its candidate uses the actual volatile ANALYZE
  collector, not the snapshot test adapter. The original uniform-only local
  pilot is at `/tmp/e1-s1-analyze-20260930.jsonl`. The follow-up pilot at
  `/tmp/e1-s1-skew-memtx-vinyl-20260930.jsonl` contains 168 rows (warmup plus
  five measured executions for fourteen queries under each state), and its
  analyzer report is `/tmp/e1-s1-skew-memtx-vinyl-20260930.report.json`.
  Results are reported separately by engine: memtx has 40 finite samples,
  median q-error 7.5 without statistics and 2.0 after ANALYZE; Vinyl has 15
  finite samples, median q-error 10 and 2.67. Ten memtx and five Vinyl
  empty-result executions have unbounded error in each state. On both engines,
  hot equality worsens from q-error 1.25 to 2.67, tail equality improves from
  10 to 3, and range improves from 131072 to 1. These are five-repeat, tiny
  observations, not acceptance evidence. Median elapsed times are 8 us / 7.5
  us for memtx and 20 us / 16 us for Vinyl; neither is latency evidence.
  Reproduce with the command in `test/sql-baselines/E1_WORKLOAD.md`. This
  narrows the producer gap but does not close S1.9: the fixtures are tiny and
  are not integrated into the reviewed M0 corpus. After volatile
  literal-equality MCV consumption landed, the current-source pilot was rerun
  at `/tmp/e1-mcv-current-20260930.jsonl` (report:
  `/tmp/e1-mcv-current-20260930.report.json`, source `8107ee88bb`). On both
  engines the skewed hot and tail equality estimates have q-error 1.0 after
  ANALYZE, versus 1.25 and 10 respectively without statistics. This is focused
  evidence that the live MCV route works, not reviewed-corpus improvement or a
  latency acceptance result. S1.9 remains open pending reviewed workload
  integration and an accepted q-error criterion. *parallel: yes*.
  A three-configuration TEST_BUILD pilot at source `8e49433dac` now also
  compares legacy+ANALYZE with the enabled M3 route under the same volatile
  statistics generation. The 252-observation sidecar and both reports are at
  `/tmp/e1-m3-current-8e49433dac*`; the same-stats planner report has equal
  median finite q-error (1.0) on memtx and Vinyl, but paired median M3/legacy
  latency ratios of 1.52 and 1.57. Ten memtx and five Vinyl measured empty
  executions per configuration remain unbounded q-error. This tiny pilot is
  not a reviewed M0 workload or E1/S1.9 acceptance result; production
  thresholds and integrated stage coverage remain open.

---

## S2 — Column Stats + Sketches

**Goal:** per-column NDV, null fraction, MCV, equi-depth histograms — the
statistics that turn selectivity estimation from "guess 25%" into
"estimate based on data."

**State:** `PROTOTYPE` (S2.2 HLL, S2.3 bounded MCV, S2.4 histogram builder,
and a narrow S2.5 conjunction joint-MCV estimator are implemented as
in-memory APIs with focused unit tests; none is integrated with persistence
or the planner). Sketch algorithms and synthetic validation may start against
a versioned S1 snapshot interface before S1 is end-to-end; persistence and
the `where.c` selectivity adapter wait for that interface.

**Exit criteria:**

- `_sql_stats_column` populated for analyzed tables;
- selectivity estimation uses MCV / histogram precedence (exact → multivariate
  MCV → FD → histogram → independence fallback);
- analytic workloads in the corpus show measurable plan-quality improvement.

**Subtasks:**

- [ ] **S2.1** `_sql_stats_column` system space + versioned payload. *parallel:
  no* (system-space allocation).
- [x] **S2.2** HyperLogLog implementation for NDV. Mergeable. *parallel: yes*.
  In-memory opaque C API at `src/box/sql/sql_stats_hll.{h,c}`; precision
  4–18, deterministic caller-seeded byte hashing, seed/precision-checked
  merges, and documented probabilistic accuracy. A composite tuple entry point
  uses arity, type tags, and length-delimited values so it can sketch joint
  NDV without ambiguous concatenation. Unit tests cover 100k NDV error,
  deterministic estimates, merge compatibility, field boundaries, and type
  tags. Callers still own canonical SQL encodings. No persistence or
  system-space IDs are included; payload integration remains pending S1/schema
  review.
- [x] **S2.3** SpaceSaving heavy-hitter sketch for MCV. *parallel: yes*.
  In-memory opaque API at `src/box/sql/sql_stats_spacesaving.{h,c}` with
  fixed capacity, deterministic lexical tie handling, conservative merge
  intervals, and single-stream `N / capacity` error bound. Unit tests cover
  heavy-hitter bounds, deterministic ties, and merge. Contract is documented
  in `statistics_implementation_plan.md`; no persistence or system-space IDs.
  A callback-driven index-summary constructor now composes bounded per-part
  SpaceSaving candidates with the existing sampled prefix-HLL summary. Its
  caller supplies canonical typed SQL bytes (type tag zero denotes NULL, which
  is excluded from MCV counts); candidate storage, maximum value size, and a
  reusable tagged-key scratch buffer are charged to the explicit summary byte
  budget, and oversize values poison the
  summary rather than exposing partial output. Focused tests cover budget
  boundaries, NULL exclusion, candidate/error access, and fail-closed bounds.
  A separate native MCV adapter now extracts canonical MessagePack scalar
  values for INTEGER, UNSIGNED, DOUBLE, BOOLEAN, and binary-collated STRING
  key parts; HLL continues to use native hashes. Non-binary collated strings
  remain NDV-only. SQL `ANALYZE` enables a fixed-capacity volatile MCV sketch
  for compatible indexes and candidate construction copies it to snapshots.
  This is not wired to persistence or the planner. No persistent format or ID
  is defined.
  The immutable in-memory `SqlStatsSnapshot` now optionally owns these
  per-part candidates (snapshot API version 4), validates their typed bytes,
  total/non-NULL sample denominators and conservative error intervals, and
  deep-copies them. Snapshot combine/replace retain the payload and remain
  byte-budgeted. This is only a
  volatile data-contract step: the callback-based sampled candidate builder
  now propagates summary MCV candidates into snapshots under its temporary
  byte budget. Owned transaction and shared-read-view collectors may opt into
  bounded MCV summaries; callback extraction or the native scalar adapter
  supplies values, and preflight charges worst-case candidate arrays before
  sampling. This remains volatile collection only: persistence integration is
  still open. A stale-checked snapshot lookup now matches a typed MCV and
  scales its SpaceSaving estimate/error interval to the index tuple
  population; an absent candidate stays explicitly unknown. `where.c` now
  consumes that interval midpoint for literal equality on the leading
  INTEGER/UNSIGNED part of a non-unique index, plus BOOLEAN and
  binary-collated STRING literals. Parameters, computed constants,
  non-leading parts, and unsupported types preserve legacy estimates. Generic
  selectivity-API and histogram consumption remain open. The
  candidate handoff also exposed and fixed a SpaceSaving eviction-error bug:
  replacement now resets error to the evicted counter floor instead of adding
  the evicted entry's stale error; a repeated-eviction regression checks every
  interval and the `N / capacity` bound.
- [x] **S2.4** Equi-depth histogram builder from sampled ordered values.
  *parallel: yes*. In-memory API at
  `src/box/sql/sql_stats_histogram.{h,c}` validates sorted caller-encoded
  values using a caller-supplied SQL-order comparator, deep-copies bounded
  quantile boundaries, and preserves equal-value groups (therefore may return
  fewer than the requested bucket count for duplicate-heavy samples). The
  opaque result reports cumulative sample counts; no SQL value encoding,
  persistence format, or system-space ID is defined. Focused unit tests cover
  quantiles, duplicate handling, deep copy, invalid ordering, and byte budget.
- [x] **S2.5 prototype** Single-column selectivity API implemented in
  `src/box/sql/sql_stats_selectivity.{h,c}`: unique equality, NULL fraction,
  sampled MCV equality, residual-NDV independence fallback, cumulative
  histogram ranges, and conservative AND confidence. A separate narrow joint-
  MCV API estimates fully specified non-NULL equality conjunctions from an
  exact sampled tuple match and otherwise falls back to the single-column
  independence product. A predicate-conjunction API now also estimates one
  equality or range term per column exactly when distinct joint tuples cover
  the complete non-NULL sample; partial joint samples use the independent
  estimate. Repeated same-column equalities and one-sided range bounds are
  combined using the supplied SQL comparator; contradictions are exact zero
  and a matching equality is estimated once. A non-point two-sided range
  requires an exhaustive joint sample; without one it is rejected because
  the histogram API cannot safely derive interval mass from interpolated
  CDFs. Focused tests cover source precedence, strict/inclusive range
  boundaries, mixed range/equality matching, correlated q-error improvement,
  fallback, and malformed input rejection. HLL has a composite tuple input
  for joint-NDV sketching. A separate equality-conjunction API now consumes
  a compatible same-sample joint-NDV estimate for a tuple absent from the
  joint MCV list:
  it divides residual sample mass by residual joint distinct count, marks the
  result as `JOINT_NDV`, and lowers confidence for the uniform residual-tail
  assumption. Exact MCV matches retain precedence; absent/invalid/incompatible
  sketch metadata preserves the prior independence estimate. This is a narrow
  standalone estimator prototype with focused unit coverage; it is not wired
  to snapshot collection, persistence, `where.c`, or the M0 workload corpus.
  This is a narrow full-tuple residual estimate, not partial-tuple matching,
  dependency statistics, or a general correlation adjustment. Partial-tuple matching
  beyond an exhaustive joint sample remains unavailable: matching MCV rows in
  a truncated joint sample gives only a lower bound, while the scalar
  joint-NDV input describes the full group and cannot recover marginal tail
  NDV. The existing predicate API is verified to sum partial-tuple matches
  exactly when the joint sample is exhaustive and to retain independence when
  it is partial. Dependencies, general correlation adjustment,
  normalized snapshot inputs, schema/group selection, and `where.c` integration
  remain open; this does not close S2.5. *parallel: yes for the standalone
  API; no for planner integration, which touches `where.c`.
- [x] **S2.6 prototype** Focused unit q-error probes cover a uniform
  1,000-value distribution, a 90%-hot skewed distribution, correlated joint
  equality, and a negatively correlated rare conjunction where the joint MCV
  corrects independence's q-error from 25 to 1. A complete joint sample also
  corrects a mixed range/equality correlation probe's q-error from 2 to 1.
  Uniform range estimates now also have a q-error matrix at every bucket edge
  (maximum 1.05), complementing the existing skewed CDF boundary matrix.
  A NULL-heavy (90%) fixture checks `IS NULL`, MCV, and residual equality
  estimates against their known frequencies, each at q-error 1.
  These validate the standalone estimator only; the M0 SQL corpus still lacks
  correlated/anti-correlated and stale-stat variants. A synthetic stale-MCV
  probe now demonstrates that a distribution shift can worsen q-error while
  carrying caller-supplied lower confidence; the estimator has no freshness
  policy and this fixture does not establish one. A separate joint-correlation
  drift fixture moves a selected pair's actual frequency from 80% to 1% and
  demonstrates q-error 80 for the stale joint sample versus 1 for the current
  sample, with lower caller-supplied confidence. These remain synthetic
  algorithm probes, not a freshness policy or corpus validation. A generated
  100-row, four-value head/tail fixture now checks cumulative range q-error at
  every distinct boundary (maximum 1.06) in
  `test_synthetic_workload_range_matrix`; this remains a unit-level workload
  probe, not an M0 SQL-corpus test or acceptance gate. No production q-error
  gate is active.
  *parallel: yes*.
- [x] **S2.7 prototype** Confidence/staleness metadata representation.
  The S1.4 snapshot records cardinality confidence/semantics, collection time,
  and modification epoch; lookup explicitly reports schema-version mismatch
  as stale. This records evidence, not a refresh/decay policy: thresholding,
  persistence, and planner fallback on age remain open integration work.
  *parallel: yes*.

---

## M3 — Single-Table Physical IR + VDBE Lowering

**Goal:** prove that a planner-IR + lowering layer can produce VDBE bytecode
equivalent (in result and diagnostic) to current `where.c` for a controlled
single-table query class.

**State:** `PROTOTYPE`. A default-off, session-gated executable route now
lowers direct-column scans, INTEGER/UNSIGNED primary-key point lookups, and
one-sided primary-key literal ranges through the physical descriptor and VDBE
emitter, plus bounded two-sided literal ranges on a single INTEGER/UNSIGNED
primary-key part. Focused memtx/Vinyl parity and fallback tests cover this
narrow slice; emitter unit checks pin all four range seek opcodes and
signed/unsigned key encoding. A one-part INTEGER/UNSIGNED TREE secondary-key
equality-run path seeks the secondary index, fetches each matching base tuple
by its complete primary key, and applies residual filters before projection.
Complete composite secondary equality and proper leading-prefix equality
scans are also supported for INTEGER/UNSIGNED keys, as are bounded secondary
ranges following a complete equality prefix. The descriptor records typed
prefix keys and the secondary scan resolves each matching primary key before
filtering and projection. M3.5's reviewed producer and route/reason gate is
closed; M3.6
capture/parity tooling is prototyped and M3.7's flag gates the executable
routes. General secondary ranges, broader range shapes, broader expression
parity, corpus-wide new-planner
coverage, and acceptance latency evidence remain open. M3 consumes M1 diagnostic
path-class/fallback reporting and the M0-A seed parity gate, but does not wait
for replay or S2; use fixed/current estimates until real statistics are
integrated.

**Scope (exact):**

- one base relation;
- point / range / full scan;
- deterministic scalar filters and projections;
- `ORDER BY`, `LIMIT`, `OFFSET`;
- result delivery.

**Explicit exclusions:** all subqueries (scalar, EXISTS, IN), CTEs and
recursive CTEs, compound SELECT, aggregates / GROUP BY / DISTINCT, joins,
DML, triggers, subprograms, non-deterministic functions.

**Exit criteria:**

- supported queries produce identical L1+L2 to current `where.c` across the
  corpus;
- unsupported queries fall back to `where.c` with stable reason code visible
  in the M0 snapshot;
- preparation/execution latency does not regress p95 by more than 5%.

**Subtasks:**

- [x] **M3.1** Physical-plan descriptor v1 — narrow form, single-table only.
  Written in `docs/vdbe/physical_plan_descriptor.md`. *parallel: no*
  (foundational contract).
- [x] **M3.2** Logical IR layer — resolved `Select` → single-relation
  `Scan` / optional `Filter` / `Project` / optional `Sort` / optional
  `Limit` chain. Expression trees are borrowed for statement lifetime; the
  builder rejects major unsupported SELECT shapes, but deterministic or
  side-effect-free expression validation remains a caller obligation. No
  resolver/VDBE routing is wired. A separate expression canonicalizer now
  handles a conservative resolved scalar subset with caller-supplied logical
  relation bindings; it is not yet wired into descriptor expression refs and
  treats resolved-column `EP_Lookup2` and `EP_NoReduce` as semantically inert,
  while still rejecting those flags on other operators. It rejects function
  calls because stable identity/effect proof is absent.
  Focused unit tests: `sql_logical_plan`, `sql_expr_canonical`.
  *parallel: yes*.
- [x] **M3.3** Physical IR prototype — choose the least-cost supplied access
  candidate for a supported single-table logical chain; emits the immutable
  descriptor for primary/secondary point, range, index full, and table full
  scans. Stable tie-break and reject reasons are unit tested. Estimates are
  fixed/current-provider inputs; no SQL routing or expression normalization
  is wired. *parallel: yes*.
- [x] **M3.4 (contract prototype)** Lowering emitter contract for scan,
  filters, projection, sort, limit, and result with stable callback ordering
  and error propagation. It deliberately does not emit executable VDBE:
  expression bytecode, cursor/engine setup, sort semantics, result delivery,
  parity, and production routing remain open. See
  `docs/vdbe/physical_plan_descriptor.md`. *parallel: yes*.
- [ ] **M3.4 executable lowering** — partial: a narrow production route now
  connects the producer, physical descriptor, VDBE loop emitter, and
  `SelectDest` result registers. It accepts resolved direct-column projections
  and canonical deterministic scalar-function/composed projections from one base table and
  requires a TREE primary index. Function-based ORDER BY remains on the legacy
  sorter path. The
  no-filter route supports optional primary-key ordering by scanning in the
  requested physical direction. Composite primary and secondary indexes now
  support ORDER BY on a leading key prefix when each requested direction
  matches the declared key-part directions or their complete inverse. Mixed
  patterns not represented by one forward/reverse key walk and non-prefix
  terms remain on legacy codegen with stable `UNSUPPORTED_EXPRESSION` fallback
  metadata. Memtx/Vinyl off/on result parity
  covers ascending prefix order, complete ascending/descending composite key
  order, and descending order over an equality-only composite prefix, while
  preflight unit tests reject unsupported mixed and non-prefix shapes. A
  composite secondary index with `(x ASC, note DESC)` now serves both its
  natural `ORDER BY x ASC, note DESC` and complete inverse; focused memtx/Vinyl
  off/on/off coverage checks the selected index and per-term result order, and
  VDBE lowering unit tests pin forward and reverse cursor walks. It
  now recognizes complete equality predicates over composite INTEGER/UNSIGNED
  primary keys through the supported 255-part key bound, independent of
  predicate order, as a true point lookup. Preflight and producer flatten
  bounded AND trees, and lowering emits one key register per part plus a
  `NotFound` seek with the composite arity. Memtx/Vinyl off/on/off parity
  covers two- and three-part keys, including a `UINT64_MAX` component, reversed
  predicates, and a miss; unit coverage checks signed/unsigned register
  ordering and three-part seek arity. Partial composite equality remains a
  leading-prefix range only when the existing supported form applies; other
  incomplete/mixed composite predicates remain on the fallback path.
  A separate prefix-scan path handles equality on a proper multi-part leading
  prefix of a longer composite key: it seeks with full prefix arity and
  terminates when any prefix column changes. Memtx/Vinyl off/on/off parity
  covers reversed two-part prefixes, misses, and UINT64_MAX in an UNSIGNED
  prefix component. Literal LIMIT/OFFSET (including zero LIMIT and positive
  OFFSET) retain `new_planner` and off/on result parity; ascending ordering on
  either the unfixed contiguous suffix or a leading key prefix (including
  equality-fixed parts) uses that same walk. Descending ordering on the
  contiguous suffix is supported for both equality-only and ranged prefixes by
  seeking on the prefix/bound and walking with `Prev` until the prefix guard
  fails. Mixed-direction and unrelated orderings remain stable fallbacks. The
  VDBE unit pins the multi-part seek, mismatch checks, reverse direction, and
  limit/offset placement. It also injects a projector failure after an opcode
  has been emitted and verifies that lowering restores the VDBE opcode count
  and Parse register state atomically (`sql_plan_vdbe_lowering.test`, 91
  assertions). Broader range semantics and corpus parity remain open; this
  fault-path coverage does not close M3.4. The multi-value primary-key point
  extension is described below; it adds bounded deduplicated seeks with
  primary-key ordering and global literal LIMIT/OFFSET, but does not close the
  broader M3.4 scope.
  **2026-09 ordered one-sided primary ranges:** the producer and lowerer now
  support both traversal directions for one-sided INTEGER/UNSIGNED primary
  ranges. Upper-only ASC rewinds and exits at the strict/inclusive upper guard;
  lower-only DESC starts at `Last` and exits at the lower guard. The opposite
  directions seek directly at their inclusive/exclusive endpoint. Unit tests
  pin seek/rewind/last, guard, and loop opcodes (84 assertions). Memtx/Vinyl
  off/on/off coverage verifies `<`/`<=` ascending and `>`/`>=` descending,
  plus composed deterministic projection over a bounded ordered range. The
  focused scalar-filter regression passes in generated, CnP, and LLVM modes.
  This remains primary-key range support, not M3.4 closure.
  *parallel: no (extends the existing producer/descriptor/lowering chain)*.
  The route also lowers `primary_key_part IS NOT NULL` as a full
  scan, relying on the primary-key non-null invariant, and `primary_key_part
  IS NULL` as an empty result using the same invariant, including secondary
  parts of a composite primary key. A typed scalar-filter descriptor and the
  table-scan lowerer now support direct non-primary-column `IS NULL` and
  `IS NOT NULL` predicates on full scans and primary-key ranges; rejected rows
  branch to the cursor's next-row opcode, after any range-end check. The
  conjunction scope is one direct non-primary NULL test plus one or two bounds
  on a single-part INTEGER/UNSIGNED primary key. A dedicated SQL regression
  checks exact rows and off/on/off parity on memtx and Vinyl, and descriptor,
  preflight, and VDBE unit tests pin filter validation and branch ordering.
  Literal LIMIT/OFFSET counts only rows surviving both the key range and NULL
  filter. The isolated fixture has exact generated/CnP parity on both engines
  (85/85 snapshots each, zero capture errors or diffs); generated-repeat also
  matches both engines exactly. A duplicate residual NULL-filter conjunction
  is explicitly retained as `fallback / UNSUPPORTED_FILTER`.
  Residual lowering now accepts up to eight direct non-primary `IS NULL` /
  `IS NOT NULL` checks on full scans, supported single-part primary-key ranges,
  and complete INTEGER/UNSIGNED primary-key points (single-part and composite).
  All checks execute before projection and share the reject/result exit;
  overflow and unsupported composite suffix-range shapes remain fail-closed.
  The VDBE unit target passes all 49 assertions. Rebuilt Debug runtime passes
  `planner_scalar_filter_test.lua`, `planner_final_paths_test.lua`, and
  `planner_flag_parity_test.lua` across memtx/Vinyl under generated and CnP
  dispatch. This remains bounded null-filter lowering, not general predicate
  lowering.
  **2026-09 secondary-index equality scan:** the producer recognizes a direct
  INTEGER/UNSIGNED equality against a single-part TREE secondary index. The
  equality run is walked with `SeekGE`/`IdxGT`/`Next`; each index hit resolves
  the full primary key through the primary cursor, so duplicate secondary
  values are all returned. Additional conjunction terms remain residual
  filters and rejected entries advance to the next index row. The immutable
  descriptor carries the selected index ID and typed key; unsupported
  secondary shapes remain on legacy codegen. Memtx/Vinyl regression coverage
  includes duplicate hits, reversed operands, signed and UNSIGNED equality
  through `UINT64_MAX`, a miss, SQL NULL fail-closed routing, contradictory
  repeated equalities, primary-key plus secondary-index predicates, text/NULL
  residuals, and LIMIT/OFFSET. Focused generated, CnP, and LLVM captures each
  contain 1,013 snapshots per engine; all four native-vs-generated
  comparisons have zero diffs. The debug `planner_scalar_filter_test.lua`
  runner passes.
  VDBE unit coverage also pins the secondary seek, equality-run guard,
  composite primary-key extraction, base-table lookup arity, and next-row
  target; mismatched index IDs and key columns reject atomically.
  **Composite secondary equality extension:** complete equality keys on
  composite TREE secondary indexes are now selected when every INTEGER or
  UNSIGNED part has a compatible literal equality, independent of predicate
  order. The immutable descriptor carries each typed key part in index order;
  the lowerer validates every part against live index metadata and emits
  `SeekGE`/`IdxGT` with the full key arity. Incomplete composite equality
  predicates remain `NO_ACCESS_PATH` fallbacks. Memtx/Vinyl off/on/off
  coverage checks duplicate hits, reversed predicate order, a miss, an
  additional residual predicate,
  an unsigned maximum component, and fail-closed partial keys. The focused
  Debug luatest passes; VDBE unit coverage additionally checks two-part seek
  arity, atomic rejection of mismatched key-part metadata, and `UINT64_MAX`
  encoding (67 assertions total at that checkpoint). Broader mixed-type,
  collation, and
  corpus parity remain outside this bounded extension.
  **Secondary-index range extension:** one-sided and two-sided literal bounds
  now use the first part of an ascending TREE secondary index, including the
  leading part of a composite index, when it is INTEGER or UNSIGNED. The
  producer intersects repeated same-side bounds, leaves additional predicates
  residual, and preserves the chosen secondary index ID in the range
  descriptor. VDBE lowering seeks at the direction-appropriate endpoint,
  performs base-row lookup, enforces the opposite endpoint, skips NULLs on
  upper-only reverse walks, and applies LIMIT/OFFSET after residual filtering.
  Memtx/Vinyl off/on/off tests cover signed and unsigned endpoints, reversed
  operands, duplicate values, residual bounds, upper-only and bounded ranges,
  NULL keys, and LIMIT/OFFSET; `EXPLAIN QUERY PLAN` confirms the index is
  selected. The `planner_scalar_filter_test.lua` Debug runner passes and the
  VDBE unit target passes 72 assertions, including both bounded directions,
  endpoint guards, and NULL termination opcodes. Ranges on non-leading
  composite parts and broad corpus parity remain open; this does not close
  M3.4. The route now also accepts descending TREE secondary-key definitions.
  Scan direction is physical: `Next`/`Prev` is combined with the declared key
  direction to produce SQL order, and descending definitions invert the seek
  comparison while preserving inclusive/exclusive endpoint meaning. On
  ascending secondary keys, ordered one-sided scans now cover both traversal
  directions: lower-only scans seek/rewind from the appropriate bound or
  prefix end, and upper-only scans seek/restart from the bound or prefix start;
  each enforces its stopping bound and NULL behavior. Bounded scans support
  either direction when requested order matches the traversal. Focused
  memtx/Vinyl off/on/off coverage exercises signed endpoints, descending
  bounded and one-sided ranges, an UNSIGNED `UINT64_MAX` bound, and selected
  index plans. Bounded and upper-only walks terminate at NULL keys before
  filtering/projection. This does not claim general secondary-index access or
  close M3.4.
  **Upper-only ascending secondary ranges (2026-09):** upper-only `<` / `<=`
  ranges can now satisfy ascending order on an ascending TREE secondary index.
  The lowerer rewinds or seeks to the fixed equality-prefix start, skips NULL
  key values, and stops at the first value outside the upper bound. The
  memtx/Vinyl off/on/off regression verifies selected `(x,y,z)` index access,
  `SeekGE`, ordered results, and continuation past a NULL key; the VDBE unit
  pins the prefix guard, upper-bound guard, and NULL skip branch (87 assertions
  total). The focused producer matrix at source `51c1f03d3e` passes all 30
  generated/CnP/LLVM × memtx/Vinyl cases; report:
  `/tmp/m34-secondary-upper-asc-51c1f03d3e/report.json`. This remains a bounded
  access-path increment, not M3.4 closure.
  **Lower-only descending secondary ranges (2026-09):** lower-only `>` / `>=`
  ranges now also satisfy descending order on an ascending TREE secondary
  index. The lowerer seeks to the end of the fixed equality prefix, walks
  backward, and stops below the lower bound (with NULL termination at the
  prefix end). Memtx/Vinyl off/on/off coverage asserts the selected composite
  index, `SeekLE`, descending result order, and parity; VDBE tests pin the
  prefix seek/guard, lower-bound guard, reverse step, and NULL exit (88
  assertions total). The refreshed producer matrix at source `bf9957bc40`
  passes all 30 generated/CnP/LLVM × memtx/Vinyl cases; report:
  `/tmp/m34-secondary-lower-desc-bf9957bc40/report.json`. This is still a
  bounded access-path increment, not M3.4 closure.
  **Declared-descending secondary keys (2026-09):** the one-sided guarded
  walks now distinguish SQL value order from the index's declared key order.
  Both `x DESC, y DESC` upper-only/ascending and lower-only/descending
  equality-prefix queries pass memtx/Vinyl off/on/off checks with the selected
  index and exact ordered rows. The current-source producer matrix at
  `d9744bd930` passes all 30 generated/CnP/LLVM × memtx/Vinyl cases; report:
  `/tmp/m34-secondary-key-direction-d9744bd930/report.json`. No general mixed
  direction or non-prefix ordering support is implied.
  **Literal `BETWEEN` secondary ranges (2026-09):** normalize a literal
  `BETWEEN` on an INTEGER/UNSIGNED indexed part to inclusive lower/upper
  bounds for candidate extraction, while preserving and evaluating the
  original expression as a residual. This works for a single-part secondary
  index and for the next key part after an equality prefix; unrelated
  non-indexed `BETWEEN` predicates retain their prior residual behavior.
  Off/on/off memtx/Vinyl coverage verifies primary-key ranges, composite
  primary-key prefix ranges, unsigned secondary endpoints through
  `UINT64_MAX`, equality-prefix composite secondary access, and row parity.
  Secondary `BETWEEN` cases also assert `SeekGE` in the experimental VDBE
  program, proving the selected range lowerer executes instead of a residual
  full scan. The focused Debug luatest passes; all 85 VDBE lowering assertions
  pass. The refreshed producer matrix at source `7d4e86936f` passes all 30
  generated/CnP/LLVM × memtx/Vinyl fixture cases; report:
  `/dev/shm/m34-between-7d4e86936f/report.json`. This is a bounded M3.4
  increment, not general predicate/access-path support or M3.4 closure.
  **Unary-plus integer bounds (2026-09):** the typed primary/secondary bound
  parser now unwraps SQL unary `+` without weakening literal validation.
  Off/on/off memtx/Vinyl coverage pins `id = +2` as a primary point route and
  `uk = +10` as a selected unsigned secondary-index equality route; the
  focused Debug `planner_scalar_filter_test.lua` passes. Other computed
  constants remain outside the access-bound grammar. This is a small M3.4
  compatibility increment, not closure.
  **Singleton `IN` equality access (2026-09):** a one-element `IN` list with
  a parseable INTEGER/UNSIGNED literal on a primary-key part is normalized to
  an equality candidate. Single-part and composite-primary point lookups use
  the typed point lowerers. A single-part INTEGER/UNSIGNED secondary TREE
  index also serves singleton `IN` equality; composite TREE indexes accept it
  on a leading key part for prefix scans and on a later key part when every
  preceding part has a parseable equality predicate. This enables both proper
  equality-prefix scans and complete composite-key lookups. The original `IN` expression is retained
  as a residual for duplicate-hit correctness. A bounded one-part primary-key
  literal `IN` path now deep-copies up to 16 distinct INTEGER/UNSIGNED keys and
  emits ordered `NotFound` seeks. Duplicate literals are removed; NULL members
  do not generate seeks, while all-NULL lists retain fallback (`6c2647c185`). This first
  multi-value form supports unordered SELECTs or `ORDER BY` on the primary key
  (ASC or DESC), plus literal LIMIT/OFFSET across the candidate set and
  zero-LIMIT seek suppression. A subsequent bounded extension accepts
  supported non-key residual predicates, evaluates them before offset and
  limit accounting, and clears expression-column caches between seeks
  (`2e36a2819a`). A conflicting additional primary-key bound remains on
  fallback; other unsupported forms also retain fallback. Off/on/off
  memtx/Vinyl coverage asserts primary and secondary point operators and pins
  multi-value primary `IN` result parity, duplicate suppression, misses, and
  route selection. The focused Debug `planner_scalar_filter_test.lua` passes
  with non-key equality and NULL residuals, plus descending
  ORDER BY with LIMIT/OFFSET after filtering, on memtx and Vinyl. The Debug
  VDBE unit passes all 91 assertions. The post-extension all-producer matrix
  passes all 30 generated/CnP/LLVM × memtx/Vinyl fixture cases, with 6,278
  captured queries per dispatcher mode and accepted component ledgers
  (`/tmp/m34-filtered-in-5bc85a1f2e/report.json`, source `5bc85a1f2e`).
  Mixed NULL/key lists and indexed equality/range residuals are now pinned in
  the same off/on/off fixture. A refreshed 30-case matrix at source
  `0243f1f4d0` passes all modes and engines with 6,374 captured queries per
  mode (`/tmp/m34-indexed-in-0243f1f4d0/report.json`).
  The first production cost-metadata input now uses fresh statement-pinned
  relation statistics for descriptor row count, average width, and confidence;
  missing/stale snapshots keep the storage population estimate (`841a399f21`).
  A TEST_BUILD regression checks fresh 32-row versus fallback four-row
  `EXPLAIN QUERY PLAN` estimates. This does not yet rank alternative access
  paths with a statistical cost model or close M3.4.
  Secondary equality/equality-prefix descriptor estimates now also consume
  fresh pinned index population and prefix NDV through a statement-scoped
  callback (`e65e225942`); absent/stale summaries keep the prior heuristic.
  The TEST_BUILD regression pins an enabled secondary-equality route and an
  eight-row estimate from 32 index tuples / four distinct prefixes. Physical
  unit and SQL stats luatest pass. Access-path ranking remains open.
  For a scalar non-unique INTEGER/UNSIGNED secondary equality, the descriptor
  now prefers the pinned value-specific SpaceSaving MCV midpoint over the
  prefix-NDV average (`960b384e30`). A live volatile-ANALYZE regression pins
  hot/tail estimates (eight/one rows), `new_planner` routing, and stale-schema
  fallback to the previous estimate. This is value-specific cost metadata,
  not a production comparison among access candidates.
  A generated-mode SQL-suite off/on/off rerun just before that cost-metadata
  change passed on memtx (1,077 queries) and Vinyl (1,085 queries), with zero
  semantic diffs and no unreviewed route transitions
  (`/dev/shm/m34-current-sql-generated-9f68/report.json`, source
  `9f68d4ae79`). The other suite/mode combinations have not been refreshed at
  this source.
  Before this extension, the focused all-producer matrix passed this fixture
  in generated, CnP, and LLVM modes on
  memtx and Vinyl: 2,443 captured queries per case, with accepted component
  ledgers (`/tmp/multi-in-limit-producer-matrix/report.json`, source
  `a76ae4f71a`). The VDBE unit now has 91 assertions, including multi-candidate
  LIMIT/OFFSET counter and jump placement. This is focused fixture evidence,
  not full-corpus M3.4 acceptance. A fresh producer
  matrix at source `ec6ebeaf0a` passes all 30 generated/CnP/LLVM × memtx/Vinyl
  fixture cases; report: `/tmp/m34-suffix-in-ec6ebeaf0a/report.json`. The new
  regression pins `x = 7 AND y IN (10) AND z = 3` to the composite `(x,y,z)`
  index and its `SeekGE` route. This is a
  bounded access increment, not general `IN` lowering or M3.4 closure.
  **Composite secondary equality-prefix scan (2026-09):** equality on a
  non-empty proper leading prefix of a composite INTEGER/UNSIGNED TREE index
  now selects an unordered index-prefix scan when no suffix range or complete
  equality path applies. The descriptor records typed prefix parts; lowering
  seeks and guards exactly that prefix arity, resolves all matching primary
  rows, then applies residual filters and LIMIT/OFFSET. Memtx/Vinyl off/on/off
  coverage checks duplicate matches and selected-index evidence, and the VDBE
  unit pins the prefix seek and mismatch guard. Ordering on a contiguous prefix
  of the immediately following key parts is supported when one forward/reverse
  walk matches every requested direction; candidate selection prefers an
  index that can supply that order. Tests cover two-term suffix order with
  LIMIT/OFFSET and a natural descending suffix on a mixed-direction index.
  Unrelated orderings and skipped leading parts remain unsupported for this
  access kind. This
  bounded route does not close M3.4.
  A further reverse-prefix regression at source `42c3af217c` now pins
  `WHERE x = 7 ORDER BY y DESC, z DESC` on `(x,y,z)`, including duplicate
  suffix values and a nullable suffix key, to the selected index and `SeekLE`;
  focused memtx/Vinyl off/on/off coverage passes. Its producer matrix passes
  all 30 generated/CnP/LLVM × memtx/Vinyl cases at
  `/tmp/m34-secondary-prefix-reverse-42c3af217c/report.json`. This strengthens
  the finite prefix-order evidence without broadening the supported order
  shapes.
  **Composite secondary prefix-range extension (2026-09):** the producer now
  recognizes complete equality predicates on leading INTEGER/UNSIGNED key
  parts followed by one-sided or bounded literal bounds on the immediately
  following key part. The descriptor carries typed prefix values and suffix
  range endpoints; lowering seeks with prefix-plus-suffix arity, stops when
  the prefix changes, enforces the opposite range endpoint, resolves the
  primary key, and applies residual predicates. An `ORDER BY` over a contiguous
  prefix beginning with the ranged suffix is accepted only when the available
  one-way traversal satisfies every requested term. Equality-fixed leading
  key columns may also appear in `ORDER BY`; they are dropped from produced
  order metadata, while all varying suffix terms must still match the same
  forward or reverse walk. The off/on/off regression verifies
  `ORDER BY x ASC, y DESC, z DESC` over an equality-fixed `x` and ranged `y`,
  including exact order and selected-index evidence on memtx and Vinyl;
  preflight unit tests reject a varying suffix that mixes incompatible walk
  directions. Focused
  memtx/Vinyl off/on/off tests cover bounded and upper-only ranges, duplicate
  prefix matches, a two-part mixed-type equality prefix, suffix ordering, and
  ascending/descending composite indexes;
  VDBE unit coverage pins key arity, both guards, and multi-term suffix order;
  runtime coverage combines ascending and descending order with LIMIT/OFFSET.
  Unsupported/incomplete prefixes and skipped index parts remain on the legacy
  path. The former one-sided fallback cases are now covered by guarded walks:
  lower-only DESC and upper-only ASC on ascending composite indexes, plus both
  corresponding logical-order cases on a declared-descending index. Focused
  memtx/Vinyl off/on/off regressions pin ordered results, selected indexes,
  bound termination, and NULL handling. This is another bounded M3.4
  increment, not M3.4 closure or broad corpus parity.
  A deeper-prefix regression at source `b1003162f3` now verifies equality on
  both `x` and `y`, a bounded range on `z`, and descending order over the
  composite `(x,y,z)` index. It asserts the selected route and `SeekLT`, with
  exact memtx/Vinyl off/on/off parity. The fresh producer matrix passes all 30
  generated/CnP/LLVM × memtx/Vinyl cases; report:
  `/tmp/m34-secondary-deep-range-b1003162f3/report.json`.
  **Secondary ordered full-scan extension:** predicate-free SELECTs may now
  order by a leading prefix of an ascending TREE secondary index with uniform
  ASC or DESC direction. The descriptor records the selected index and every
  produced-order term; lowering walks forward or backward, resolves each
  complete primary key, and projects the fetched base row. Memtx/Vinyl
  off/on/off coverage includes one- and two-term order, duplicate keys,
  `UINT64_MAX` suffix order, and both directions. The focused
  `planner_scalar_filter_test.lua` runner passes, and VDBE unit tests pin the
  direction-specific cursor operations, multi-term order metadata, and base-row
  lookup (72 assertions total). Off/on/off result coverage also verifies
  LIMIT/OFFSET on full scans and ascending/descending range traversals, with
  selected-index plans. The scan direction is mapped relative to a matched
  index definition; a memtx/Vinyl descending-only composite-index fixture
  exercises both its natural forward walk and reverse traversal. Its
  single-part primary key also verifies that preflight uses the secondary key
  length for order validation, while nullable keys verify NULL placement in
  both natural and reverse traversal. Direct string equality, inequality,
  and `IN` residuals can accompany ordered full traversal when no more
  selective key access path applies; the executor resolves the base row before
  filtering. Coverage includes LIMIT/OFFSET after rejecting earlier ordered
  rows, and checks the selected secondary index and results.
  Arbitrary predicates, non-prefix ordering, and mixed-direction requests not
  matching a key definition or its complete inverse remain unsupported. This is a bounded M3.4
  increment, not closure.
  **2026-09 scalar-comparison extension:** direct comparison residuals now
  also accept `=`, `<>`, `<`, `<=`, `>`, and `>=` between a non-primary source
  column and a constant expression accepted by the canonicalizer and free of
  column, variable, and function references, including reversed
  constant/column operands. This includes integer, float, string, BLOB,
  boolean, NULL, and literal-only arithmetic/concatenation expressions. The
  immutable expression reference is
  resolved to its original WHERE term only at lowering, and SQL expression
  bytecode plus `IfNot` preserves false/NULL rejection semantics.
  Function calls remain fail-closed in this comparison shape; simple primary-
  key conjuncts remain bounded to the existing access-bound grammar. The
  producer accepts direct same-table column-to-column comparisons as
  expression residuals (for example `a <= b` and `t > s`) and collated direct
  comparisons (for example `s COLLATE "unicode_ci" = 'A'`); both are covered
  by `planner_scalar_filter_test.lua`. Thus the earlier status text's claims
  that both were unsupported were stale. Compound `AND`/`OR` trees are
  admitted only when every leaf is a supported direct-column comparison or
  NULL test. Regression coverage checks equality, inequality,
  ordered/reversed comparisons, and mixed primary-key bounds and
  primary/composite-point residuals on both engines; generated/CnP captures
  match exactly (583 snapshots per engine), and the VDBE lowering unit target
  passes all 62 assertions at that checkpoint. The current Debug build also
  passes `planner_scalar_filter_test.lua` on memtx and Vinyl.
  M3.4 remains partial: this is a bounded direct scalar comparison extension,
  not general predicate lowering.
  **2026-09 BLOB literal extension:** canonical expressions now encode
  resolved `X'…'` literals as lowercase hex without changing their byte
  identity, and the direct residual producer accepts them alongside other
  scalar literals. The SCALAR/BLOB probe previously classified as a physical
  filter fallback now transitions from `current_where_c` to `new_planner`;
  the route/result regression passes on memtx and Vinyl, and focused
  generated/CnP captures compare exactly (107 snapshots per engine). The
  canonicalizer unit target passes all 19 assertions, including malformed-hex
  rejection. This is focused evidence, not a refreshed reviewed-corpus route
  report.
  **2026-09 composed projection verification:** the executable route also
  evaluates a composed deterministic projection (`ABS(a) + b`) for each row
  of a three-row primary-key range using the original SQL expression bytecode,
  with planner-off/on/off result checks on memtx and Vinyl. The focused
  `planner_scalar_filter_test.lua` passes under
  generated, CnP, and LLVM dispatch, and checks that the enabled route is
  `new_planner`. This verifies that composition for this canonical expression
  shape; it does not establish general function/operator projection coverage.
  Direct residual comparisons also accept SQL `TRUE` and `FALSE` literals,
  canonically distinct from integer 1/0. The memtx/Vinyl scalar-filter
  regression checks both values with off/on/off result parity; generated/CnP
  captures compare exactly at 583 snapshots per engine.
  The scalar-filter matrix also covers bounded boolean residual trees:
  `OR` across non-primary comparisons, NULL-test disjunctions, unary `NOT`
  with SQL NULL behavior, a nested OR combined with a primary-key range, and a
  disjunction containing a primary-key equality. The original boolean subtree
  is retained as one immutable
  expression reference and evaluated with SQL's existing expression bytecode;
  false and NULL results reject the row. Preflight admits only comparison,
  NULL, AND, and OR structure, while the physical producer further requires
  single-source columns and canonical supported constants. Runtime off/on/off
  checks and generated/CnP captures pass on memtx and Vinyl (583 snapshots per
  engine, exact parity). Arbitrary boolean trees remain outside the contract.
  **2026-09 BETWEEN residual extension:** direct non-primary-column `BETWEEN`
  and `NOT BETWEEN` with two canonical constant-expression bounds are retained
  as one expression filter; primary-key `BETWEEN` continues to normalize into
  inclusive access bounds. Runtime off/on/off checks pass on memtx and Vinyl,
  and focused generated/CnP captures compare exactly (607 snapshots per
  engine). Canonicalizer unit coverage includes valid and malformed BETWEEN
  nodes. Other BETWEEN operand shapes remain unsupported.
  **2026-09 IN residual extension:** direct non-primary-column `IN` and `NOT
  IN` over non-empty lists of canonical constant expressions are retained as
  expression filters, including their use as leaves in bounded boolean trees.
  Subquery IN, empty/malformed lists, and noncanonical list members remain
  unsupported. The focused regression passes on memtx and Vinyl; generated,
  CnP, LLVM, and generated-repeat captures each validate 631 snapshots per
  engine, with exact comparisons and observed native participation. The
  canonicalizer unit target passes all 23 assertions.
  A follow-up SQL matrix pins `IN` with a NULL list member (non-matching rows
  evaluate UNKNOWN and are rejected) and an OR of two IN leaves. It passes on
  memtx and Vinyl; generated/CnP/LLVM/repeat captures compare exactly at 655
  snapshots per engine.
  **Same-source column-comparison residual extension (2026-09-29):** direct
  `=`, `<>`, `<`, `<=`, `>`, and `>=` comparisons between two resolved columns
  of the same single-table source now lower as residual expressions, not key
  bounds. The original expression bytecode preserves SQL NULL behavior; the
  existing bounded boolean grammar admits these comparisons as leaves under
  AND/OR/NOT, including conjunction with primary-key access bounds. Column
  references from other sources, computed operands, and explicit collation
  expressions remain fail-closed. The off/on/off regression covers all six
  comparison operators, integer and text comparisons, NULL operands, boolean
  OR/NOT, and a primary-key point plus a residual comparison; it passes on
  memtx and Vinyl under generated, CnP, and LLVM dispatch. This extends residual
  coverage only; broader access-path and expression support remain open.
  **Expression NULL-test residual extension (2026-09-29):** `IS NULL` and
  `IS NOT NULL` now also accept canonical scalar expressions whose column
  references all resolve to the same source (for example, `a + b IS NULL`).
  The expression is evaluated by the existing SQL bytecode path. This initial
  slice kept function calls fail-closed; see the deterministic-call extension
  below.
  **Computed comparison residual extension (2026-09-29):** comparison
  operands may now be canonical scalar expressions (for example,
  `a + b = 3` or `a + b = id`) when every column reference belongs to the
  scanned source and at least one operand is row-dependent. Such predicates
  remain residuals and do not become access bounds; direct column/constant
  primary-key predicates retain their existing bound behavior. Boolean OR
  coverage includes computed and direct-column leaves. The focused memtx/Vinyl
  off/on/off regression passes under generated, CnP, and LLVM dispatch.
  **Computed BETWEEN/IN residual extension (2026-09-29):** canonical,
  same-source scalar expressions are also accepted as the left operand of
  `BETWEEN`/`NOT BETWEEN` and `IN`/`NOT IN`, with bounds/list members either
  canonical constants or canonical expressions over the same source. The
  focused regression checks arithmetic operands, a row-relative IN member and
  BETWEEN bound, including NULL propagation from source rows. It passes on
  memtx and Vinyl under generated, CnP, and LLVM dispatch. Subqueries,
  cross-source operands, and noncanonical expressions remain rejected; a bare
  scalar predicate is not admitted because SQL requires a boolean result.
  **Deterministic scalar-call residual extension (2026-09-29):** resolved
  deterministic scalar calls now have canonical identities consisting of a
  case-folded function name and ordered canonical arguments. `EP_Lookup2` is
  accepted on function nodes as resolution metadata with no remaining semantic
  effect. The producer admits these calls only as residual operands in the
  bounded WHERE grammar when their columns bind to the scanned source; they
  cannot become access bounds. SQL bytecode preserves evaluation semantics.
  Memtx/Vinyl off/on/off coverage checks `ABS(a) IS NULL` and `abs(a) > 1`;
  generated, CnP, and LLVM dispatch all pass. A `random()` predicate remains
  `UNSUPPORTED_NONDETERMINISTIC`, and `ABS(a)` in projection remains
  `UNSUPPORTED_FUNCTION`. The canonicalizer unit target passes 14 supported
  and 10 rejection assertions. This is residual-filter support only; M3.4
  remains partial.
  **Deterministic scalar projection extension (2026-09-29):** a deterministic
  scalar call used as a complete SELECT-list expression can now be evaluated
  by the existing SQL expression bytecode on the new route, provided the
  single-table preflight and canonical-expression contract accept the query.
  The memtx/Vinyl `ABS(a)` projection case passes off/on/off checks under
  generated, CnP, and LLVM dispatch. Function-based ORDER BY remains
  `UNSUPPORTED_FUNCTION` on the legacy route; nondeterministic projection calls
  remain rejected. A further off/on/off regression now covers a deterministic
  call nested over arithmetic source expressions and composed with an outer
  arithmetic operator (`ABS(a + b) * 2`); it passes for memtx and Vinyl and
  verifies the new route. The scalar-filter fixture is included in the
  producer matrix: all 30 fixture/engine/dispatcher cases pass at source
  `f3592727a3`, including generated/CnP/LLVM for memtx and Vinyl; the scalar
  fixture records observed CnP and LLVM executions. Report:
  `/tmp/m34-nested-composed-projection-f359/producer-matrix/report.json`.
  Function-based ORDER BY remains outside this projection support. The SQL
  suite's generated audit identifies
  19 `fallback / UNSUPPORTED_FUNCTION` to `new_planner` transitions per engine
  in collation (`UPPER`/`LOWER`), `gh-4697-scalar-bool-sort-cmp` (`TYPEOF`),
  and `types.test.lua` (`ABS`, `TYPEOF`, `QUOTE`, `LEAST`) projections; off/on
  and off-repeat semantics are exact across all 1,077 memtx / 1,085 Vinyl
  queries. SQL-TAP's generated audit passes exact semantics across 47,946
  memtx and 37,990 Vinyl statements. Five alias-predicate cases refine their
  diagnostic from `UNSUPPORTED_FUNCTION` to `UNSUPPORTED_FILTER`; the
  side-effecting deterministic UDF remains on legacy codegen and results are
  unchanged. Both route classes are documented in
  `planner_flag_route_classes.json`. This does not close M3.4.
  **Explicit collation residual extension (2026-09-29):** canonicalization
  now includes resolved `COLLATE` nodes using a case-folded collation name and
  their canonical operand. Explicit collations are admitted only inside
  bounded WHERE residuals; the wrapped expression cannot match direct key
  access extraction, while existing SQL bytecode retains comparison semantics.
  Memtx/Vinyl off/on/off cases distinguish `unicode_ci` from `binary`, cover
  `IS NULL`, and assert `new_planner`; a separate SQL-TAP case confirms
  collated projection remains `fallback / UNSUPPORTED_COLLATION`. The
  canonicalizer unit target passes 15 supported and 11 rejection assertions.
  This remains residual-only support and does not close M3.4.
  **LIKE residual extension (2026-09-29):** the parser's deterministic
  function-form `LIKE` expression is admitted as a bounded boolean residual
  when it has two or three canonical arguments and all column references bind
  to the scanned source. `NOT LIKE` remains unary NOT over the same leaf;
  `MATCH` and other standalone function predicates stay rejected. Existing SQL
  bytecode preserves pattern, escape, and NULL semantics. Focused memtx/Vinyl
  off/on/off coverage passes generated, CnP, and LLVM dispatch for LIKE and
  NOT LIKE, including NULL rows. Indexed LIKE and collated-key predicates
  retain legacy access paths until their range/index requirements are modeled;
  SQL-TAP collation coverage confirms the indexed LIKE EQP/result contract.
  This remains residual-only and does not close M3.4.
  **LIKE ESCAPE regression coverage (2026-09-30):** the scalar-filter fixture
  now exercises positive and negated escaped-percent patterns, plus an escaped
  percent occurring after a numeric prefix. The three queries require the
  `new_planner` route and pass off/on/off result comparison on both memtx and
  Vinyl; this pins the optional third `LIKE` argument's runtime semantics
  without expanding the supported expression grammar.
  The SQL-TAP preflight regression now asserts `new_planner` for direct scalar
  residuals, mixed primary-key/residual predicates, and bounded boolean
  filters. At this checkpoint, a parameterized primary-key bound was an
  explicit `fallback / UNSUPPORTED_FILTER` counter case. A full local Debug SQL-suite
  run passes 136 tests with 2 disabled and no failures; both memtx and Vinyl
  preflight variants pass.
  **Parameterized residual extension (2026-09-29):** canonical identities
  now represent resolved bind parameters by their 1-based variable ordinal,
  independent of positional/named spelling. Non-primary scalar comparisons,
  `IN`, and `BETWEEN` can therefore retain prepared parameters as residual
  expressions; runtime values are still read by the existing SQL
  `OP_Variable` bytecode. A focused memtx/Vinyl regression asserts the
  `new_planner` route, checks parameterized `IN`/`BETWEEN` results, and executes
  the same prepared equality statement with matching, nonmatching, and NULL
  values. A separate
  primary-key `id = ?` assertion was then `fallback / UNSUPPORTED_FILTER`, since
  variable-valued seek-key emission was not implemented at that checkpoint.
  The canonicalizer unit test passes 17 supported and 12 rejection assertions; the focused scalar
  filter test passes locally. The post-commit producer matrix passes all 30
  fixture/engine/dispatcher cases at source `7daed303b6`, including the new
  parameterized `IN`/`BETWEEN` cases; its scalar-filter fixture records 2,581
  CnP and 1,001 LLVM executions per engine. Report:
  `/tmp/m34-parameterized-operators-7daed/producer-matrix/report.json`.
  After integrating the composite-point filter extension and CTE role update,
  the broader `planner_flag_parity_test.lua` also passes on the rebuilt Debug
  binary under generated and CnP dispatch; this remains focused route evidence,
  not reviewed-corpus feature acceptance.
  **Scalar integer primary-key parameter point lookup (2026-09-30):** the
  executable route now stores the resolved one-based bind ordinal in the
  immutable descriptor and emits `OP_Variable` plus `OP_MustBeInt` before a
  primary `NotFound` seek. Invalid/non-integral and NULL values skip the seek
  and return no rows. The feature is limited to equality on one-part signed
  INTEGER primary keys; composite/UNSIGNED parameter keys and parameterized
  ranges remain on legacy codegen. Off/on/off SQL parity covers both operand
  orders, hits, a miss, exact-integral and non-integral numeric binds, and
  NULL on memtx and Vinyl. The focused SQL luatest passes under generated and
  CnP dispatch; the lowering unit target passes 85 assertions, including
  variable ordinal, type guard, NULL guard, and seek opcode order. This is a
  bounded M3.4 increment, not closure or reviewed-corpus acceptance.
  **Filtered composite primary suffix ranges (2026-09-29):** the
  `planner_composite_prefix_range_test.lua` off/on/off fixture now composes a
  direct residual equality with an equality-prefix-plus-suffix range, and a
  bounded OR of residual equalities with a range over the preceding key part.
  Both assert `new_planner` when enabled and exact rows/order on memtx and
  Vinyl; the focused luatest passes. This verifies only the existing bounded
  comparison/NULL boolean grammar over those range shapes. Boolean trees
  outside that grammar and scalar operators outside the direct-column/
  constant-expression contract remain outside this route. Direct-column full
  scans and primary-key ordering also pass
  off/on/off parity for a TEXT primary key on both engines; the enabled route
  preserves descending order and LIMIT. A second
  route supports equality between the sole INTEGER/UNSIGNED primary-key part
  and a matching signed-64-bit/unsigned-64-bit
  integer literal; it emits a
  primary cursor NotFound seek and returns at most one row. The equality
  route supports ORDER BY only on that one primary-key column; ordering is
  redundant for a point result. Literal LIMIT/OFFSET are accepted because
  equality can return at most one row (zero limit and positive offset emit no
  result). Nonnegative signed-64-bit integer-literal
  `LIMIT` and optional `OFFSET` are retained in the descriptor; counters
  above `INT_MAX` use unsigned `OP_Int64` constants, and offset rows are
  skipped before projection and a result counter handles the limit. `LIMIT 0`
  skips scan execution. Other LIMIT/OFFSET expressions remain on legacy codegen.
  The producer uses `index_size()` for a coarse row
  estimate; successful lowering emits cursor open, ascending
  `Rewind`/`Next` (or descending `Last`/`Prev`), `Column`, `ResultRow`, and
  cursor close inside a codegen checkpoint. Physical candidate or recoverable
  lowering rejection records a stable physical fallback reason and resumes
  legacy codegen; hard diagnostics propagate. The session flag is default-off.
  Focused SQL parity passes on memtx and Vinyl for one-/two-column projection,
  NULL and empty-table results, literal `LIMIT 0`/`LIMIT 1`/`LIMIT 1 OFFSET 1`,
  descending primary-key order with LIMIT, primary-key point hit/miss with
  LIMIT 1 / LIMIT 0 / OFFSET 1 semantics, signed-64-bit point keys through
  INT64_MIN/MAX, UNSIGNED point keys through UINT64_MAX, and INTEGER/UNSIGNED
  one- and two-sided primary-key literal ranges (`>`, `>=`, `<`, `<=`) with
  reversed operands, ascending/descending ordering, and LIMIT/OFFSET. A
  two-sided route intersects one or more lower and/or upper literals on the
  same key, then terminates at the strongest opposite endpoint; mixed filters
  remain on legacy codegen. Additional memtx/Vinyl SQL assertions pin a
  singleton inclusive interval, an empty interval sharing an exclusive
  endpoint, and an inverted interval; all three preserve flag-off/on results.
  UNSIGNED range seek
  constants retain uint64 values through UINT64_MAX; negative UNSIGNED values
  fail closed to legacy codegen, while literals above UINT64_MAX are rejected
  by SQL parsing before planner dispatch. Unsupported predicates still fall
  back. One-sided bounds now also reject a scan direction that cannot
  terminate correctly (lower-bound scans must ascend; upper-bound scans must
  descend) before VDBE mutation; the focused unit target passes all 32 checks.
  A SQL-luatest exercises unsafe opposite directions on memtx and Vinyl:
  `id > 1 ORDER BY id DESC` and `id < 3 ORDER BY id ASC` preserve ordered
  legacy results with the feature enabled, report
  `fallback / UNSUPPORTED_FILTER` on that attempted route, and retain the
  existing `fallback / UNSUPPORTED_EXPRESSION` classification when disabled.
  The focused planner-flag parity test passes all five cases.
  Those emitter checks pin all four range opcodes (`SeekGT`,
  `SeekGE`, `SeekLT`, `SeekLE`), ascending/descending step opcodes, signed
  negative range key encoding, and full-width unsigned `P4_UINT64` preservation, alongside
  unbounded, limited, offset, zero-limit, descending, signed-64-bit counter
  initialization, register overflow, and checkpoint rollback after late
  point-projection rejection. SQL regressions
  verify `LIMIT 2147483648` and paired wide LIMIT / OFFSET
  execute on the new route with unchanged row semantics; a point lookup at
  `INT64_MAX` also exercises wide key-register encoding on both engines.
  Boundary regressions now cover the full signed INTEGER domain, inclusive
  `INT64_MIN` and `INT64_MAX` singleton ranges, and the empty `id > INT64_MAX`
  range on memtx and Vinyl. They exposed a lowering defect: positive wide
  signed range bounds were emitted as `P4_INT64`, causing invalid signed
  MsgPack key encoding (and an assertion for the max-exclusive seek). Both
  bounded-end and seek-key registers now encode nonnegative wide values as
  `P4_UINT64`, matching the existing point-key path; negative wide bounds
  remain `P4_INT64`. A follow-up endpoint specialization lowers
  `id <= INT64_MIN` as a point lookup and `id < INT64_MIN` as an empty result
  for a single-part signed primary key; it avoids requiring a descending scan
  for unordered endpoint queries and leaves composite-prefix semantics
  unchanged. The corrected SQL golden and test pass on memtx and Vinyl. Typed
  generated/CnP/LLVM/repeat capture validates all 34 statements per engine;
  every dispatcher and repeat comparison is exact (34/34).
  Leading INTEGER/UNSIGNED parts of composite TREE primary keys now support
  equality-prefix scans (returning every row with that prefix), one-sided and
  two-sided literal ranges, and compatible ASC/DESC key-prefix ordering. The
  producer represents prefix equality as a bounded range rather than an
  incorrect one-row point lookup. Memtx/Vinyl off/on/off coverage checks row
  parity for equality, both one-sided directions, a two-sided range, and
  descending equality-prefix order; all enabled cases report `new_planner`,
  and generated plus CnP focused runs pass. Equality over all composite parts,
  non-leading-only predicates, and mixed/non-prefix ordering remain outside
  this route.
  The executable producer and prefix-scan lowerer now also support a contiguous
  equality prefix followed by lower-only, upper-only, or bounded integer-range
  predicates on the next primary-key part. The lower bound participates in a
  composite `SeekGT`/`SeekGE`; upper bounds terminate the ascending walk after
  the prefix guard. Descending order now uses an upper endpoint as the seek
  key and walks with `Prev`; bounded ranges stop at the lower endpoint, while
  upper-only ranges stop at the prefix boundary. A descending lower-only range
  seeks with `OP_SeekLE` using only the equality-prefix key, then walks backward;
  the prefix guard and lower-bound check stop before projection when the walk
  leaves the prefix or crosses its lower endpoint. This avoids a synthetic
  maximum suffix value. The route preserves signed/unsigned key encodings and
  uniform compatible key order.
  The isolated
  `planner_composite_prefix_range_test.lua` checks exact rows and off/on/off
  parity for the ascending range shapes and descending upper-only/bounded
  shapes on memtx and Vinyl, including an UNSIGNED suffix above `INT64_MAX`
  and a literal-left bound whose resolved comparison expression is commuted
  by the parser. The earlier three-part point fixture
  now confirms equality on the first two parts plus a range on the third uses
  `new_planner`. Before the descending extension, generated, CnP, and
  repeated-generated captures each recorded 72 snapshots per engine with exact
  parity. After the extension, all three modes record 126 snapshots per engine
  with zero capture errors; CnP and repeated-generated comparisons each have
  exact 126/126 parity on memtx and Vinyl, and CnP execution is observed.
  LLVM was not observed because this build has JIT disabled. Descriptor and
  VDBE lowering unit target passes all 57 assertions, including an opcode-level
  check that the DESC lower-only seek uses just the prefix arity and executes
  the lower-bound guard before projection. The SQL fixture adds both strict and
  inclusive lower-only DESC cases, an inclusive `UINT64_MAX` endpoint, and a
  missing equality-prefix case that must stop at the prefix guard; generated
  and CnP focused runs pass on memtx and Vinyl. LLVM was not run for this
  change. Other range predicates and gaps in
  the equality prefix remain fail-closed. Composite
  equality-prefix scans/ranges now also accept up to eight direct non-primary
  `IS NULL` / `IS NOT NULL` residual filters. The lowerer checks prefix and
  range termination before the residual, then branches rejected rows to the
  next cursor step; LIMIT/OFFSET count only accepted rows. The regression adds
  prefix-equality filtering, bounded ASC filtering, DESC `IS NOT NULL`, and
  filtered LIMIT/OFFSET, with
  exact off/on/off row parity on memtx and Vinyl. Typed generated/CnP capture
  validates 216 snapshots per engine with exact 216/216 comparisons; CnP
  participation is observed. The VDBE unit target passes all 60 checks,
  including the prefix-range filter jump target. LLVM was not run because the
  build has JIT disabled. Composite-primary-key `IS NOT NULL` terms in a
  bounded AND conjunction are now treated as redundant, including the case
  where they are the only predicates; unsupported siblings still fall back.
  `IS NULL` within a composite-key conjunction remains an explicit
  `UNSUPPORTED_FILTER` fallback. The memtx/Vinyl off/on/off regression passes,
  and generated/CnP capture validates 319 snapshots per engine with exact
  319/319 parity. The immutable descriptor now
  also distinguishes direct projection columns from canonical scalar
  projection expressions by expression reference. The production route calls
  SQL's existing `sqlExprCode()` for those expression slots inside each row's
  VDBE loop; direct columns retain `OP_Column`. Off/on/off SQL parity on
  memtx and Vinyl covers arithmetic projections, NULL propagation, a
  primary-key point lookup, a compatible range/order, and a composite prefix scan.
  This is a bounded canonical-expression
  projection slice, not arbitrary scalar/function support; the broader M3.4
  producer, operator, parity, and capture gates remain open.
  Descriptor values
  above the signed-64-bit counter range are rejected before VDBE mutation.
  Rollback coverage is specifically post-emission validation rejection, not
  an allocation failure inside the opcode emitter: `late_invalid_point_desc` in
  `test/unit/sql_plan_vdbe_lowering.c` emits the wide-key opcode and then
  rejects the projection. Its strengthened assertion now pins unchanged
  opcode count and pre-existing opcode contents, cleared speculative opcode
  slots/P4 ownership, and restored register, cursor, label, expression-cache,
  temporary-register, abort, and column-cache state. Meanwhile
  `sql_vdbe_codegen_checkpoint.test` checks checkpoint cleanup with manually
  emitted P4/comment state. A broad production failure hook to force allocator
  failure inside `sqlVdbeAddOp*()` would distort the API and is not planned.
  An additional storage route now supports unordered `ITER_ALL` scans over a
  HASH primary index when the experimental flag is on. The regular cursor
  opener still rejects non-TREE indexes; the planner-specific opener is
  limited to unordered full scans (including primary-key IS NULL/IS NOT NULL
  identity predicates), and `Rewind` selects HASH's
  supported `ITER_ALL` rather than TREE's ordered `ITER_GE`. A memtx SQL
  regression checks the existing flag-off rejection and flag-on result/path;
  it passes under generated and CnP dispatch. HASH point/range/order routes
  remain unsupported and retain the legacy non-TREE error. This is a narrow
  extension, not general secondary-index support.
  **BETWEEN range extension (2026-09-28):** the producer now normalizes a
  direct `BETWEEN` on a supported primary-key part into the same inclusive
  lower/upper bounds emitted by SQL expression codegen. Existing bound parsing
  still rejects non-literal, non-key, and malformed bounds; the existing range
  producer/lowerer then applies its prefix, endpoint, and ordering checks. The
  SELECT preflight admits only a structurally valid two-bound node, leaving
  key/literal eligibility to the producer. The composite-prefix regression
  includes ascending and descending inclusive ranges plus a full-width
  UNSIGNED interval. The focused test passes; generated/CnP capture validates
  all 276 statements on both memtx and Vinyl with exact 276/276 parity and
  observed CnP execution. Validation used the local Clang scratch build.
  **Same-side bound intersection (2026-09-28):** composite suffix-range
  planning now accepts multiple lower and/or upper literal bounds on that same
  suffix part and keeps the strongest bound; a strict bound wins when equal
  endpoints are repeated with mixed inclusivity. The regression covers a
  weaker/stronger pair, strictness ties on both ends, and a contradictory
  interval, with off/on parity on memtx and Vinyl. Generated/CnP capture
  validates 312 statements per engine with exact 312/312 parity and observed
  CnP execution. Ranges split across different key parts retain their existing
  limits.
  **Leading-part range extension (2026-09-29):** the same bound reducer now
  handles ranges over a single-part primary key and the leading part of a
  composite primary key, including multiple one-sided bounds and bounded
  intersections. New single-part and composite-leading regressions cover
  strongest-bound selection, strictness, DESC upper-only behavior, empty
  intersections, and off/on/off parity on memtx and Vinyl. The focused test
  passes; generated/CnP capture validates 408 statements per engine with exact
  408/408 parity and observed CnP execution. Only the earliest varying part
  supplies access bounds; later-part predicates are still evaluated as
  residuals, not as additional key-range bounds.
  **Later-key residuals on a leading primary-key range (2026-09-29):** the
  producer now bounds a composite primary-index scan with the earliest varying
  key part and retains predicates on later key parts (and equality predicates
  on the ranged part) as residual SQL expressions. Predicates on preceding
  key parts must still be equalities, and only one varying part contributes
  access bounds; later comparisons do not claim a lexicographic multi-part
  interval. The memtx/Vinyl scalar-filter regression covers a leading bounded
  range with a later-part residual and confirms the `new_planner` route.
  Focused Debug build and luatest pass. The post-change producer matrix also
  passes all 24 fixture/engine/dispatcher cases (generated, CnP, and LLVM ×
  memtx and Vinyl) at source `4f1a30d646`, with no failed cases; report:
  `/tmp/m34-later-key-residuals-4f1a/producer-matrix/report.json`. This is not
  arbitrary range splitting or broad corpus parity.
  Do not infer rollback of AST, parser, or schema state.
  **Current-source parity refresh (2026-09-30, `fe76f616f2`):** the reviewed
  SQL and SQL-luatest off/on/off corpora pass on both memtx and Vinyl under
  generated, CnP, and LLVM dispatch. SQL reports cover 1,077 memtx / 1,085
  Vinyl queries per mode; SQL-luatest covers 499 / 447 queries in generated
  mode and 498 / 446 in CnP/LLVM. All have zero semantic diffs, exact
  off-repeat semantics, and zero unreviewed route transitions. The complete
  generated SQL-TAP captures also pass with 47,946 memtx and 37,990 Vinyl
  queries, with zero semantic diffs and unreviewed transitions. Reports:
  `/tmp/m34-current2-sql-{generated,cnp,llvm}/report.json`,
  `/tmp/m34-current2-sql-luatest-{generated,cnp,llvm}/report.json`, and
  `/tmp/m34-current3-sql-tap-generated-{memtx,vinyl}/report.json`. SQL-TAP
  CnP/LLVM refreshes were not completed: the initial parallel attempt exhausted
  the already nearly-full `/dev/shm`, and a full two-engine generated capture
  exceeded available disk. This is partial current-source corpus evidence,
  not complete M3.4 capture coverage.
  **Completion of the SQL-TAP matrix (2026-10-01):** rerunning one engine at a
  time with disk-backed temporary directories completed the missing SQL-TAP
  CnP and LLVM comparisons. Both memtx (47,946 queries) and Vinyl (37,990
  queries) pass in each dispatcher mode: zero semantic diffs, exact off-repeat
  semantics, and zero unreviewed transitions. The mode/engine reports are
  `/tmp/m34-current3-sql-tap-{cnp,llvm}-{memtx,vinyl}/report.json`; generated
  reports are listed above. Across SQL, SQL-luatest, and SQL-TAP, all nine
  reviewed-corpus suite/mode combinations are now parity evidence. The SQL-TAP
  CnP/LLVM reports record `cd17106ade`, a docs-only commit after the
  `fe76f616f2` reports; no SQL or test source changed between those revisions.
  The binary used throughout was built at `fe76f616f2`, so the executed
  production/test source tree is identical at both report revisions.
  This does not cover all descriptor operators, arbitrary lexicographic ranges
  spanning multiple varying key parts, all storage edge cases, or
  corpus-wide parity;
  checkpoint rollback does not include
  arbitrary parser/AST/schema mutation. Keep M3.4 open pending broader producer,
  parity, and capture coverage. Details:
  `docs/vdbe/physical_plan_descriptor.md`. *parallel: no* (shares
  `SELECT`/VDBE integration).
- [x] **M3.5** Per-component fallback gate — complete for the adopted
  per-component ledger contract. The `planner_flag_acceptance.py` aggregate
  was refreshed after the deterministic projection increment, with nine
  suite/mode reports (generated, CnP, LLVM ×
  SQL-TAP, SQL, and SQL-luatest) plus 24 focused producer runtime cases (four
  fixtures × two engines × three modes) passed with zero
  semantic/off-repeat diffs and unreviewed route transitions; the focused
  matrix verifies ledger v1 and observed 1,706 CnP / 722 LLVM executions.
  Documented fixed-mode exclusions remain explicit and generated-mode covered.
  The refreshed reports are under `/tmp/m35-final-c4bb/`. This closes the
  producer accounting gate, not M3.4's broader lowering or M3.7's functional
  feature scope. The implementation
  history follows. Producer-contract
  prototype now maps logical/physical reject enums to stable reason codes and
  exposes `path_class` plus an optional descriptor (`sql_plan_fallback.*`),
  with focused mapping tests. `sqlWhereBegin()` now records the actual legacy
  route for multi-relation queries as `fallback` /
  `UNSUPPORTED_RELATION_COUNT`; summary/snapshot EXPLAIN and per-reason
  counters are wired and exercised locally. Aggregate SELECTs with a WHERE
  path plus GROUP BY/HAVING, DISTINCT, compound, CTE, and FROM-subquery forms
  now report their stable structural reasons and increment corresponding
  counters; GROUP BY/HAVING has an explicit runtime capture regression.
  Scalar, EXISTS, and IN subqueries embedded in a single-relation projection
  or predicate are also detected before rewrite and report
  `UNSUPPORTED_SUBQUERY`; runtime assertions cover each form and the shared
  per-reason counter delta.
  Explicit `INDEXED BY` and `NOT INDEXED` constraints now fail the logical
  plan prototype closed and report `UNSUPPORTED_ACCESS_HINT`; the planner IR
  does not yet represent those access-path requirements. Builder and reason-
  mapping unit tests cover both forms, with SQL runtime/counter coverage in
  `planner_fallback_access_hint.test.lua`.
  After name resolution, ordinary scalar calls lacking the deterministic
  function property now report `UNSUPPORTED_NONDETERMINISTIC` before logical
  plan construction/flattening; runtime coverage includes built-in `random()`,
  a user-defined function registered non-deterministic, deterministic `abs()`,
  and per-reason counter deltas. This property detects declared volatility,
  but Tarantool has no separate function side-effect property, so deterministic
  UDF side effects and argument-dependent volatility are not proven excluded.
  Deterministic scalar calls are now also classified as
  `UNSUPPORTED_FUNCTION`: the canonical expression contract does not yet
  carry function identity/evaluation semantics, so they remain on `where.c`.
  Runtime `abs(v)` coverage asserts this reason in projection, predicate, and
  ordering expressions; `SUM(abs(v))` retains the higher-priority aggregate
  reason. Stable reason mapping is unit tested. No executor routing has
  changed. Explicit `COLLATE` expressions now report
  `UNSUPPORTED_COLLATION`, since the canonical expression contract does not
  carry collation semantics; inherited/default column collations are not
  treated as explicit hints. A focused live summary/counter test covers the
  reject path. Otherwise-supported SELECTs now also consult the resolved
  scalar expression canonicalizer and report `UNSUPPORTED_EXPRESSION` when
  CAST or another scalar operator falls outside its grammar; LIKE is
  represented as a function operator and reports `UNSUPPORTED_FUNCTION`.
  Structural rejects retain precedence; LIMIT/OFFSET are restricted to nonnegative integer
  literals as required by the replay/descriptor model. Focused runtime and
  counter tests cover CAST, LIKE, arithmetic and negative LIMIT, and
  parameterized LIMIT and OFFSET; LIKE is correctly classified as an
  unsupported function because the parser represents it through the function
  operator. Parameterized primary-key access bounds remain on the legacy route
  because their runtime values are not yet represented as VDBE seek-key
  producers in the immutable access descriptor; parameterized non-primary
  residuals are represented by parameter ordinal and evaluated through the
  existing `OP_Variable` SQL bytecode.
  For shapes that pass the feature-gated table-scan preflight, this expression
  classification is deferred until the physical attempt: a successful TEXT
  primary-key ordered scan reports `new_planner` without a stale fallback
  reason or counter, while the disabled legacy route retains the stable
  `UNSUPPORTED_EXPRESSION` diagnostic. Focused memtx/Vinyl route and result
  parity passes.
  The new-planner producer now refuses to attempt lowering after a structural
  fallback has already been recorded, preventing flattening from erasing a
  rejected subquery shape and the later physical route from overwriting its
  statement-level `fallback` classification. `misc.test.lua` verifies the
  FROM-subquery and scalar/EXISTS/IN subquery reasons with the feature enabled;
  memtx and Vinyl pass. This protects the current first-reason statement
  contract but does not provide per-SELECT route records for shared VDBEs.
  The per-reason SQL counter array and exported stat fields now use an
  exclusive reason-count sentinel, so appended function/collation/expression
  codes are counted instead of silently disappearing after access-hint code
  12. A focused unit assertion pins the sentinel after the last stable code.
  Zero-source constant SELECTs also enter `sqlWhereBegin()` but are outside the
  single-relation logical-plan contract. They now report
  `UNSUPPORTED_RELATION_COUNT`; a focused
  `EXPLAIN (planner = 'summary') SELECT 1` regression asserts the route and
  per-reason counter. This classifies only this legacy route; it does not claim
  that constant SELECT execution is implemented by the new planner.
  Structural
  classification runs before flattening/rewrite can erase the rejected shape;
  simple `COUNT(*)` stays unclassified because its fast path does not enter
  `where.c`. A bounded audit against every current reject in
  `sql_logical_plan_from_select()` found the structural cases covered by
  pre-/post-resolution classification; the remaining direct `COUNT(*)` path
  emits `OP_Count`, so assigning it a fallback-to-`current_where_c` reason
  would misstate the execution route. The existing SQL regression keeps that
  path unclassified; its runtime regression now pairs the null planner
  classification with an `EXPLAIN` opcode assertion for `OP_Count`, proving
  this query bypasses `sqlWhereBegin()` rather than falling back through it.
  When the experimental table-scan route is enabled, simple predicate queries
  that the executable slice cannot lower now report stable
  `UNSUPPORTED_FILTER` and increment its per-reason counter; earlier, they
  were indistinguishable from ordinary `current_where_c` execution. The new
  primary-key `IS NOT NULL` scan route has a paired non-primary-key regression
  that asserts `fallback / UNSUPPORTED_FILTER` and identical flag-on/off rows
  on memtx and Vinyl. The complementary primary-key `IS NULL` predicate
  lowers to a zero-row result and reports `new_planner`; non-primary `IS NULL`
  remains `fallback / UNSUPPORTED_FILTER`, with row parity covered on both
  engines. The same null-predicate lowering now recognizes a non-leading part
  of a composite primary key on both engines, with off/on result parity and
  empty-result assertions. A text-primary-key descending scan is accepted by
  the new planner;
  when disabled, it reports `fallback / UNSUPPORTED_EXPRESSION`. The enabled
  route carries no fallback reason, and its attempt adds no total or
  per-reason fallback count; the disabled EXPLAIN and execution each increment
  the fallback counters once. The point
  lookup boundary now has focused fallback coverage for a bind parameter,
  equality on a non-primary column, NULL/computed values, unsupported ranges
  (including non-primary-key ranges), OR, and negative literals against
  UNSIGNED primary keys, with matching legacy results and stable fallback
  reasons on both engines. Literals above `UINT64_MAX` are rejected by SQL
  parsing before planner fallback classification. More specific
  expression/function rejection reasons retain precedence.
  At that checkpoint M3.5 remained partial: the narrow table-scan route records physical
  rejection reasons at the attempted producer/lowering boundary, but the
  remaining legacy planner rejects are not all classified and routed through
  one complete producer gate. Physical candidate selection from the general
  logical-plan API still has unit-test-only callers; this direct scan slice
  uses its dedicated table-scan producer. Unsupported shapes continue to use
  existing structural/expression classifications or the legacy route. This is
  an incremental integration, not complete fallback coverage. A focused
  fallback SQL matrix (six `planner_fallback_*` tests, `planner_preflight`,
  and `misc`) passes locally on both memtx and Vinyl (16 cases). The snapshot
  assertion in `misc` now pins the disabled ORDER BY's
  `fallback / UNSUPPORTED_EXPRESSION` classification and non-replayable
  disposition instead of the stale `current_where_c` expectation. Also passing
  `sql_plan_fallback.test` (34 Lua assertions and 9 TAP checks); refreshed
  result baselines no longer preserve earlier assertion-error output.
  Full-corpus
  capture/parity review is complete under M3.6; it does not imply the missing
  physical-reject accounting or new-planner route is implemented.

  A bounded live route audit distinguishes the direct multi-row VALUES and
  simple `COUNT(*)` emitters from SELECTs that enter `sqlWhereBegin()`. The
  latter are `current_where_c` when no earlier reject has been recorded, or
  `fallback` with the structural/physical reject when one has; `OP_Count` is
  deliberately left unclassified because it does not fall back through
  `where.c`. The runtime regression now asserts that one attempted unsupported
  filter reports `UNSUPPORTED_FILTER` and increments both the total and
  per-reason counters exactly once. This test runs alongside the existing
  memtx/Vinyl filter-result parity checks and does not alter route selection.
  A focused rerun against the rebuilt current source shows that the no-predicate
  `ORDER BY` on a non-primary column reports `fallback` with the existing stable
  `UNSUPPORTED_EXPRESSION` reason and increments total/per-reason counters
  exactly once. This is the conservative canonical-expression rejection, not a
  distinct order-specific reason. The memtx/Vinyl `planner_preflight` regression
  now checks the reason and counter delta around a single EXPLAIN execution;
  both engines pass. An earlier double execution of the same EXPLAIN obscured
  this behavior through statement reuse, and its intermediate test failure was
  not evidence about the route. No new reason code is needed for this shape.
  A focused canonicalization correction also accepts the resolved-column
  `EP_NoReduce` marker, which name resolution may set alongside `EP_Lookup2`.
  This removes false `UNSUPPORTED_EXPRESSION` rejection for otherwise-supported
  direct column expressions. The legacy planner now correctly reports
  `current_where_c` for ordinary projections and primary-key predicates when
  the feature flag is off; targeted `misc`, `planner_preflight`, and all five
  fallback SQL suites pass on memtx and Vinyl against the rebuilt executable.
  At that checkpoint M3.5 remained open for the other legacy/producer rejection routes.

  A follow-up audit of the production `SELECT` path found no additional
  unclassified ordinary rejection that can safely be closed by adding a reason
  enum alone. Classification is split across source-shape preservation before
  rewrite, post-resolution logical/expression checks, the experimental
  table-scan preflight/lowering attempt, and the actual legacy
  `sqlWhereBegin()` route. In particular, the non-primary-key `ORDER BY v`
  preflight case is already recorded as `fallback` / `UNSUPPORTED_EXPRESSION`
  by the resolved canonical-expression check before physical preflight; the
  existing memtx/Vinyl regression asserts that reason and its exact counter
  delta. A separate `UNSUPPORTED_ORDER` code would duplicate and misstate that
  producer rejection. Simple `COUNT(*)` remains a deliberate direct `OP_Count`
  route, not a legacy-WHERE fallback. The remaining M3.5 blocker is
  architectural completeness: dedicated SELECT emitters and recursive SELECT
  branches do not pass through one producer gate, while the general
  logical-to-physical API is still not the production producer for all SELECT
  routes. The scope decision is now per-component: root, recursive
  compound/CTE/subquery, direct VALUES, and direct OP_Count components all get
  records; statement summary mirrors a uniform root route, otherwise reports
  `mixed` with no reason. Component records, not the summary, are authoritative.
  Close M3.5 only after the producer inventory and mixed/direct/nested runtime
  cases verify this contract. See `planner_vm_migration.md` for the Mermaid
  flow. Do not infer closure from the current reason-mapping table.

  **Live `sqlSelect()` producer inventory (2026-09-29).** A source audit of
  every direct call site in `src/box/sql` confirms the following producer
  families. In planner-snapshot mode, each successful invocation registers a
  component on the top-level VDBE;
  recursive and embedded producers are not collapsed into the enclosing
  statement route.

  | Call-site family | Source sites | Component role / route contract |
  | --- | --- | --- |
  | SQL statement root | `parse.y` SELECT command; `select.c` `sqlSelect()` entry | Root role by default; one route record for the component. |
  | Plain multi-row VALUES | `select.c` `sqlSelect()` VALUES dispatch and legacy VALUES row chain | `values` component with `direct_values` when emitted directly; compound-dispatch record for the legacy chain. |
  | Compound branches | `select.c` `multiSelect()` UNION/INTERSECT/EXCEPT branch calls | `compound_branch` components; dispatcher and each branch retain independent routes. |
  | Recursive CTE | `select.c` recursive setup and recursive-term calls | `recursive_anchor` / `recursive_term`; direct queue/row routes remain distinct from WHERE fallback. |
  | FROM subquery and CTE source | `select.c` coroutine and materialization calls | `from_subquery` or `cte`, linked to the enclosing SELECT component. |
  | Expression subquery | `expr.c` scalar, EXISTS, and IN-with-SELECT codegen calls | `scalar_subquery` or shared `expression_subquery`, linked to its containing component. |
  | Direct count | `select.c` simple `COUNT(*)` fast path | `count` component with `direct_op_count`; not a legacy-WHERE fallback. |
  | INSERT-SELECT | `insert.c` SELECT input producer | `insert_select_root`, rooted independently in the statement ledger. |
  | View DML materialization | `delete.c` shared DELETE/UPDATE view helper | `dml_view_materialization_root`, with its actual fallback/new-planner route. |
  | Trigger SELECT step | `trigger.c` trigger-program SELECT compiler | `trigger_select_root` for the first component and `trigger_select` children, owned by the enclosing top-level VDBE. |

  This inventory is source-level coverage, not proof that every reviewed SQL
  corpus topology has passed typed capture. Focused runtime tests now cover
  all listed direct/nested producer families, including mixed roots and
  children and trigger ownership. Full reviewed-corpus inclusion and route
  dispositions remain the M3.5 acceptance gate. A fresh focused capture of the
  INSERT-SELECT, view-DML, and trigger producer fixture validates 41 snapshots
  per engine in generated, CnP, LLVM, and generated-repeat modes; all three
  comparisons are exact, with 39 observed CnP and 32 observed LLVM executions
  per engine. A fresh full Debug `sql-luatest` normal run also passes 54 tests,
  with 2 disabled and the volatile ANALYZE test skipped because this build is
  not `TEST_BUILD`.

  ```mermaid
  flowchart TD
    S[SELECT entry] --> V{Plain multi-row VALUES?}
    V -- yes --> VE[Direct VALUES emitter]
    V -- no --> P[Preserve structural rejects before rewrite]
    P --> E[Resolve; classify logical and expression rejects]
    E --> C{Simple COUNT(*) fast path?}
    C -- yes --> CO[Direct OP_Count; unclassified]
    C -- no --> N{Feature-gated table-scan lowering succeeds?}
    N -- yes --> NP[new_planner]
    N -- no --> W[Continue through sqlWhereBegin]
    W --> R{Earlier reject recorded?}
    R -- no --> CW[current_where_c]
    R -- yes --> FB[fallback + stable reason]
  ```

  **Route-integration audit (2026-09-27, source inspection at M3.5 tip).**
  This inventory confirms M3.5 is not yet a complete SELECT producer gate:

  The component-route regression now includes an actual recursive CTE in its
  producer matrix and asserts complete ancestry plus the `values` anchor and
  `recursive_term` component roles. It also pins the anchor as `direct_values`
  and the recursive branch as `fallback / UNSUPPORTED_COMPOUND`. This records
  each generated branch individually; the outer statement remains a structural
  fallback, so this improves route evidence without claiming a unified
  producer gate. The recorder no longer stops classifying components after
  the first statement-level fallback reason is set: a mixed nested-query test
  pins the root as `fallback / UNSUPPORTED_SUBQUERY` and its child as
  `fallback / UNSUPPORTED_FUNCTION`, while the statement summary retains the
  root's first reason. The focused component-route and fallback SQL matrices
  pass on the rebuilt binary.

  The same component ledger now allows a nested table-scan attempt after the
  root has already fallen back, while preventing a component already classified
  as fallback from being overwritten. Preflight distinguishes an unsupported
  destination only after validating the SELECT shape, so an invalid filter or
  projection retains its more specific rejection. The regression enables the
  feature flag for a scalar subquery compiled to a non-output destination and
  asserts root `UNSUPPORTED_SUBQUERY`, child `UNSUPPORTED_DESTINATION`, and a
  statement summary that retains the root reason with two component fallbacks.
  This improves nested route evidence but does not close M3.5's universal
  producer-gate requirement.

  | Producer / branch | Current boundary | M3.5 implication |
  | --- | --- | --- |
  | Plain multi-row `VALUES` | `sqlSelect()` returns through `multiSelectValues()` before VDBE creation and pre-opt fallback classification. | Direct emitter; not a `where.c` fallback. Keep out of the single-table SELECT gate or define an explicit direct path class. |
  | Compound SELECT / recursive CTE | `sqlSelect()` dispatches `pPrior` to `multiSelect()`; that invokes `sqlSelect()` again for component SELECTs. Recursive generation separately calls it for setup and recursive terms, and directly emits queue/current-row loops. | Classification and route evidence must be per SELECT component, not inferred from the outer compound route. Recursive components may share a VDBE. |
  | FROM-subquery | `sqlSelect()` prepares/compiles subqueries or coroutine producers before attempting the outer table-scan lowering; flattening may also rewrite the outer tree. | The outer attempt is not a statement-wide producer gate. Preserve pre-rewrite rejects and account for nested producers independently. |
  | Simple `COUNT(*)` | Aggregate branch opens a cursor and emits `OP_Count`, returning without `sqlWhereBegin()`. | Deliberate direct route, not fallback. It needs its own explicit path class if the contract requires every SELECT to be classified. |
  | Feature-gated table scan | `sql_select_try_lower_table_scan()` is called once for an eligible non-compound SELECT after preparation/subquery work. It builds its own table-scan descriptor and lowers it; it does not call the general logical-to-physical selector. | This is the only production new-planner SELECT producer today. Its rejects are useful for this attempt, not a universal classification of every path. |
  | Ordinary scan / aggregates except simple count | Reaches one of several `sqlWhereBegin()` call sites (ordinary row loop, grouped aggregate, aggregate-without-group). `where.c` only adds a multi-relation reason if no earlier reason exists. | `current_where_c` vs `fallback` currently describes the shared VDBE's recorded metadata, not a complete per-producer route ledger. |
  | Scalar / expression subquery | Expression codegen can recursively compile a SELECT into the same statement VDBE; source-tree walker may mark the containing SELECT unsupported first. | A statement-global reason can mask which nested component rejected, and a component-level `new_planner` assignment is not automatically a truthful statement-level classification. |

  **Required integration before closure:** define a route-result record with an
  explicit scope (SELECT component versus whole statement) and direct-route
  classes; establish a common entry/exit gate around each `sqlSelect()` producer
  before it mutates shared VDBE state; preserve source-shape evidence before
  flattening; and ensure each legacy dispatch attaches the reason from the
  producer that actually rejected the candidate. Then add runtime tests that
  combine nested/recursive components with different route outcomes and assert
  that neither a direct emitter nor a nested `new_planner` attempt silently
  overwrites or inherits another component's classification. Only after that
  integration should the production path build logical inputs/candidates and
  call the general physical selector. This is a design/coverage prerequisite,
  not a proposed routing change; no additional enum alone closes it.

  **Access-hint producer regression (2026-09-27).** The existing
  `planner_fallback_access_hint` SQL test checked `INDEXED BY` and `NOT INDEXED`
  only with the feature disabled. It now repeats both `EXPLAIN (planner =
  'summary')` cases with `sql_new_planner_single_table` enabled and asserts the
  pre-normalization `UNSUPPORTED_ACCESS_HINT` reason plus exact total and
  reason-counter deltas. This covers the ordering contract at the attempted
  table-scan producer boundary: the structural rejection must survive rather
  than be replaced by a later lowering result. It does not address nested
  producer ownership, direct emitters, or the missing statement/component
  route ledger; M3.5 remained open at that checkpoint. The focused `planner_fallback_access_hint`
  test-run passed on both memtx and Vinyl in the root Clang-19 build.

  **Recursive CTE producer regression (2026-09-27).** The prior CTE check used
  only a non-recursive CTE and ran with the feature disabled. A focused SQL
  regression now enables the new planner for a recursive CTE whose anchor is a
  direct `VALUES` emitter and whose recursive term is a SELECT. It asserts the
  enclosing statement remains `fallback` / `UNSUPPORTED_CTE` and increments
  exactly one total and CTE-specific fallback counter. This proves stable
  statement-level preflight classification across this mixed recursive shape;
  it does not supply per-component route evidence or close the shared-VDBE
  ownership gap in the inventory above.

  **Plain multi-row VALUES classification audit (2026-09-27).** The direct
  `VALUES (1),(2)` producer is not truthfully testable as a fallback using the
  current summary/counter contract. Source inspection shows `sqlSelect()`
  dispatches a plain multi-row VALUES statement to `multiSelectValues()` before
  the outer VDBE/preflight fallback gate; the helper invokes `sqlSelect()` for
  each single-row value. A runtime probe with
  `sql_new_planner_single_table` both disabled and enabled nevertheless reports
  `fallback` / `UNSUPPORTED_RELATION_COUNT` and increments the total and
  relation-count counters once in each case. Those statement-global values do
  not describe the direct emitter's route, so a regression asserting them
  would codify a misleading classification. Accurate coverage requires an
  explicit decision about direct-route classification and its scope (whole
  statement versus per SELECT component); until then, do not add fallback
  assertions or infer a route class for VALUES.

  **Route-ledger scope decision (2026-09-27): resolved.** M3.5 uses every
  SELECT component as its coverage unit across every `sqlSelect()` producer,
  including top-level root, recursive compound/CTE/subquery, direct multi-row
  VALUES, direct OP_Count, INSERT-SELECT, view-DML materialization, and trigger
  program producers. Each component needs an identity, parent and producer
  role, and route result. Direct
  emitters are explicit route classes, not fallback errors. Statement summary
  mirrors a uniform root route; if component routes are mixed it reports
  `mixed` with no fallback reason. Component records are authoritative. The
  current statement-level `path_class` / `fallback_reason` fields and
  first-reason-wins behavior do not satisfy this contract. Remaining work is
  the reviewed producer inventory and broad mixed/direct/nested runtime
  evidence. The v5 snapshot ledger is authoritative when complete; legacy
  statement fields remain a compatibility view. No additional route enum
  alone closes the remaining producer-coverage gate.

  **Component-ledger integration (2026-09-27).** The bounded internal model
  now records unique SELECT component IDs, parents, producer roles, and route /
  reason results, with explicit direct VALUES / OP_Count and compound-dispatch
  classes. `sqlSelect()` and `where.c` populate it at root, nested, structural
  fallback, legacy WHERE, new-planner, direct-values, direct-count, and
  compound producer boundaries. Snapshot envelope v5 exposes complete records
  and derives the statement summary as uniform root route or `mixed` with no
  reason. The M0 harness rejects incomplete successful SELECT ledgers. Focused
  runtime cases cover scan, join fallback, VALUES rows, OP_Count, compound,
  recursive CTE, FROM-subquery, and scalar-subquery; these all pass locally.
  At this audit checkpoint M3.5 remained open pending reviewed-corpus producer coverage and wider route
  matrix evidence. A second focused producer matrix now also covers constant
  SELECT, DISTINCT, grouped aggregation, MIN/MAX, EXISTS, UNION, INTERSECT,
  and SELECT without FROM. Every successful snapshot is required to have a
  complete non-empty component ledger with no pending routes and valid parent
  references; the expanded luatest passes against the v5 build. It now also
  asserts a mixed shared-VDBE case exactly: the scalar-subquery root is
  `fallback`, while its child is `subquery` / `direct_op_count` with the root
  as parent; the statement summary is `mixed` with no fallback reason. This
  strengthens focused branch coverage but is not a
  reviewed-corpus inventory. The M0
  focused producer matrix now additionally covers direct non-primary NULL
  filters both on full scans and conjoined with primary-key bounds; both emit
  complete single-component `new_planner` ledgers when enabled. The expanded
  planner-final-path luatest passes locally; generated/CnP and generated-repeat
  captures match exactly (43/43 snapshots each, zero errors or diffs on both
  engines). This extends
  component-level route evidence to both accepted new-planner and rejected
  filter candidates without changing the reviewed-corpus gate.
  Component producer roles now distinguish FROM-subqueries at the two
  recursive code-generation call sites (coroutine and materialized) and scalar
  expression subqueries via `SF_SingleRow`. The component matrix asserts the
  FROM label on a DISTINCT FROM-subquery that cannot be flattened, and the
  scalar label on a `count(*)` component. The rebuilt Debug binary and regular
  test-runner pass `planner_final_paths_test.lua` with both role assertions
  under generated and CnP dispatch.
  `sqlCodeSubselect()` now assigns the shared `expression_subquery` role to
  EXISTS and IN-with-SELECT producers; AST/codegen does not justify separate
  role claims for these paths. Runtime matrix assertions pin both roles while
  scalar SELECT retains `scalar_subquery`. The snapshot validator accepts the
  appended role. At this implementation checkpoint, M3.5's reviewed-corpus
  producer gate remained open.
  CTE-expanded FROM sources now retain their `cte` identity instead of being
  mislabeled as ordinary `from_subquery` producers at coroutine/materialized
  codegen. Recursive CTE setup SELECTs now receive `recursive_anchor` at their
  producer callsite; the runtime component matrix checks that role and its
  `direct_values` route. The component tests pass under generated and CnP
  dispatch; syntax and diff checks passed before integration, and the
  integrated Debug build passed. The typed SQL-TAP capture fixture also
  captures and validates the recursive-anchor role in a complete manifest.
  The capture extension now preserves component records in manifest
  `component_ledger_version: 1`; its validator checks parent ordering and
  references, unique identities, stable routes/reasons, and summary agreement.
  `VALUES` is now included in successful planner snapshot capture. The typed
  capture fixture exercises fallback, current-WHERE, new-planner, mixed,
  direct-VALUES, and direct-OP_Count paths; corruption probes verify rejection
  of missing parents and mismatched summaries. The end-to-end typed capture,
  M0 corpus/policy tests, and focused component runtime matrix all pass. A
  validator registry mismatch for the producer's `UNSUPPORTED_FILTER` and
  `UNSUPPORTED_DESTINATION` reasons was corrected in the M0 schema/validator;
  the scalar-filter generated/CnP captures now validate rather than being
  rejected as an inconsistent run outcome.
  A
  generated-mode standalone SQL-TAP audit then covered all 275 files on each
  engine (reports under `/dev/shm/m35-sqltap-{memtx,vinyl}.json`): memtx
  captured 66,055 statements / 53,592 component records; Vinyl captured
  66,051 / 53,584. Every emitted successful-SELECT manifest used component
  ledger v1, every ledger validated, and there were zero incomplete component
  ledgers. Of those files, 235 memtx and 234 Vinyl standalone captures passed
  TAP plus snapshot validation; seven timed out on each engine. Among the 234
  files accepted on both engines, per-file component role and route histograms
  matched exactly. The single acceptance difference was the known
  concurrency-attribution test (`gh-2723-concurrency.test.lua`); rejected
  ANALYZE/system-stat cases still depend on the unapproved persistence gate.
  This is complete SQL-TAP producer coverage evidence, but the audit is not a
  normal-runner parity decision and does not cover SQL or SQL-luatest corpus
  entries. At that point M3.5 remained open until those reviewed-corpus producers and route
  changes are dispositioned.

  **Current SQL-TAP ledger re-audit (2026-09-28, generated mode).** Re-ran all
  275 standalone SQL-TAP files on both engines against the integrated
  component-ledger implementation; reports are in
  `/dev/shm/m35-current-ledger-audit/{memtx,vinyl}-generated.json`. Memtx had
  250 accepted, 23 rejected, and 2 timed-out files; Vinyl had 249 accepted,
  24 rejected, and 2 timeouts. Accepted captures contain 148,637 memtx and
  148,008 Vinyl statements and 120,857 / 120,853 component records,
  respectively. Every accepted manifest uses ledger v1 and validates with
  zero incomplete ledgers. All 249 files accepted on both engines have exact
  per-file component route- and role-histogram agreement. The one additional
  memtx acceptance is the known concurrency-attribution fixture. Rejections
  and timeouts remain explicit audit outcomes, not approvals. This strengthens
  SQL-TAP ledger completeness evidence only; it is generated-mode standalone
  capture, not SQL-luatest coverage, planner off/on parity, or route-transition
  disposition. M3.5 remained open for those gates at that checkpoint.

  **Current SQL-TAP ledger re-audit (2026-09-29, generated mode).** Re-ran all
  275 standalone SQL-TAP files against the current integrated Debug binary on
  memtx and Vinyl, then retried each default-timeout file with a 60-second
  per-file cap. The combined accepted results match the preceding broad audit:
  memtx accepted 250 files (148,637 snapshots / 120,857 components), rejected
  23, and left two timed out; Vinyl accepted 249 (148,008 / 120,853), rejected
  24, and left the same two timed out (`sort.test.lua` and
  `gh-3332-tuple-format-leak.test.lua`). All accepted manifests validate as
  component ledger v1, with zero incomplete ledgers. The 249 files accepted on
  both engines have identical per-file component route and role histograms;
  the only acceptance difference is `gh-2723-concurrency.test.lua`, the known
  concurrency-attribution fixture. The fresh default-cap sweeps and 60-second
  retry reports are under `/dev/shm/m35-sqltap-current-20260929-{memtx,vinyl}.json`,
  `/dev/shm/m35-sqltap-current-memtx-retry60-20260929.json`, and
  `/dev/shm/m35-sqltap-current-vinyl-retry60-20260929.json`. This confirms
  standalone generated-mode ledger completeness only; it does not close the
  SQL/SQL-luatest corpus, off/on route-transition, or remaining M3.5 disposition
  gates.

  **Reviewed SQL-TAP planner-flag audit (2026-09-29, generated mode).** The
  off/on/off audit now compares 232 reviewed SQL-TAP files / 47,946 snapshots
  on memtx and 224 files / 37,990 snapshots on Vinyl. It reports zero semantic
  differences for both off-to-on and off-repeat parity on both engines; the
  output-only differences are EXPLAIN diagnostics. Every observed route/reason
  transition is classified by the route policy, including 961 transitions per
  engine from `current_where_c` to `fallback / NO_ACCESS_PATH`.
  This class is the intentional primary-only executor boundary: when a WHERE
  predicate can use a leading secondary-index field, the new full-table-scan
  route declines so it cannot displace the existing indexed `where.c` choice.
  The 36 `UNSUPPORTED_EXPRESSION` to `NO_ACCESS_PATH` reason refinements per
  engine record that the physical table-scan producer has no candidate;
  they do not change query semantics or claim that a new route executed. The
  latter outcomes have no selected path, so their component route stays null.
  The focused `planner_flag_fallback_parity` regression checks an actual
  secondary-index lookup retains results and reports `NO_ACCESS_PATH`; TAP
  `eqp` / `whereG` and reverse-singleton-IN cases also pass focused off/on/off
  checks. Reports are under `/dev/shm/m35-planner-flags-sqltap-current-20260929-reviewed`
  and its `-vinyl` counterpart. This closes route disposition for this
  reviewed SQL-TAP slice only; the separate SQL and SQL-luatest audits below
  disposition their selected suites, while the full M3.5 producer gate remains
  open.

  **Reviewed SQL-luatest planner-flag audit (2026-09-29, generated mode).**
  The reviewed selection passes off/on/off semantic parity with zero diffs on
  both engines: 32 tests / 499 snapshots on memtx and 31 / 447 on Vinyl. All
  route transitions are classified, with no unreviewed classes. The two
  `current_where_c` to `fallback / NO_ACCESS_PATH` transitions per engine are
  the same primary-only/secondary-index boundary described above; representative
  cases are `gh_5183_fix_index_field_missing` (`a IS NULL` with a secondary
  index) and `gh_8418_select_lead_to_assertion` (`_space.owner = 1`). They
  retain exact result parity. Reports are under
  `/dev/shm/m35-planner-flags-sql-luatest-current-20260929-reviewed-{memtx,vinyl}`.
  This dispositions the reviewed SQL-luatest selection, not every luatest
  topology; the standalone `sql` audit follows below, and the full M3.5
  producer gate remains open.

  **Reviewed `sql` planner-flag audit (2026-09-29, generated mode).** The
  documented fixed-mode selection excludes only `sql/iproto.test.lua`, whose
  `box.stat().EXECUTE` assertion observes snapshot-EXPLAIN instrumentation.
  The remaining reviewed SQL cases pass off/on/off semantic parity with zero
  diffs: 33 tests / 1,077 snapshots on memtx and 34 / 1,085 on Vinyl. All 106
  route transitions per engine fall into six reviewed classes: supported
  `current_where_c` to `new_planner` adoptions, conservative `NO_ACCESS_PATH`
  and `UNSUPPORTED_FILTER` fallbacks, and precise no-path/filter reason
  refinements for unsupported expression or aggregate shapes. There are no
  unreviewed route classes. Reports are under
  `/dev/shm/m35-planner-flags-sql-current-20260929-reviewed-{memtx,vinyl}`.
  This closes generated-mode route disposition for the reviewed `sql`
  selection; the explicitly incompatible iproto observer and multi-mode/full
  producer acceptance remain open.

  **Current reviewed SQL CnP planner-flag audit (2026-09-29).** Repeated the
  fixed off/on/off route and semantic audit with CnP dispatch enabled on the
  current Debug binary. SQL-TAP passes for 232 memtx tests / 47,946 snapshots
  and 224 Vinyl tests / 37,990 snapshots; SQL passes for 33 / 34 tests and
  1,077 / 1,085 snapshots; SQL-luatest passes for 31 / 30 tests and 498 / 446
  snapshots. All six engine/suite reports have zero semantic diffs, exact
  off-repeat semantic parity, and zero unreviewed route transitions. The same
  two known fixed-mode exclusions apply: `sql/iproto.test.lua` observes
  snapshot instrumentation through `box.stat().EXECUTE`, and
  `gh_8676_exists_in_multiselect_test.lua` contains only a direct `VALUES`
  producer so no native CnP execution is attributed. Generated-mode audits
  still include both. SQL-TAP retains only EXPLAIN-row differences (28/27
  off/on and 5/4 off-repeat for memtx/Vinyl); the SQL and SQL-luatest captures
  are exact. Reports are under `/dev/shm/m35-planner-flags-*-cnp-current-20260929*`.
  This is broad CnP route/parity evidence, not the LLVM run or the complete
  M3.5 producer gate.

  **Current reviewed SQL LLVM planner-flag audit (2026-09-29).** Repeated the
  off/on/off evaluation with LLVM JIT enabled on the current Clang-19 / LLVM-19
  Debug binary. SQL-TAP passes on both engines: 232 memtx tests / 47,946
  snapshots and 224 Vinyl / 37,990, with zero semantic and off-repeat diffs
  and no unreviewed route transitions. Only EXPLAIN rows differ (35 / 34
  off/on and 5 / 4 off-repeat). The reviewed `sql` selection passes 33 / 34
  tests and 1,077 / 1,085 snapshots; SQL-luatest passes 31 / 30 tests and
  498 / 446 snapshots. Both suites have zero semantic or repeat diffs and no
  unreviewed routes. These retain the documented `iproto` observer-counter
  exclusion and no-native-execution direct-VALUES exclusion; those tests are
  still covered in generated mode. A focused LLVM run also passed `eqp`,
  `whereG`, and reverse-singleton-IN coverage on both engines (243 snapshots,
  zero semantic/repeat diffs). Reports are under
  `/dev/shm/m35-planner-flags-sql-tap-llvm-current-20260929-{memtx,vinyl}`,
  `/dev/shm/m35-planner-flags-sql-llvm-current-20260929-reviewed-{memtx,vinyl}`,
  and `/dev/shm/m35-planner-flags-sql-luatest-llvm-current-20260929-reviewed-{memtx,vinyl}`.
  This verifies current-source LLVM route and semantic parity for reviewed
  suites; M3.5 producer closure and the remaining M3.7 functional scope still
  need explicit acceptance review.

  The full current-source LLVM reports for the EQP emitter correction are
  `/dev/shm/sql-tap-eqpfix4-llvm-memtx-20260929/report.json` and
  `/dev/shm/sql-tap-eqpfix4-llvm-vinyl-20260929/report.json`. They confirm zero
  semantic/off-repeat diffs and zero unreviewed transitions with the corrected
  secondary-index covering labels, legacy range operators and estimates, and
  singular row wording. Commits: `fe0a84344c` (emitter) and `3f8305a552`
  (route classification).

  **Generated SQL-TAP EQP revalidation (2026-09-29).** The complete generated
  captures pass semantic and off-repeat parity: 232 memtx tests / 47,946
  queries and 224 Vinyl tests / 37,990. Raw differences are EXPLAIN-only
  (35 / 34 off/on; 5 / 4 off-repeat). Reports:
  `/dev/shm/sql-tap-eqpfix4-generated-memtx-20260929/report.json` and
  `/dev/shm/sql-tap-eqpfix4-generated-vinyl-20260929/report.json`. Those
  captures started before the newly observed `fallback/UNSUPPORTED_EXPRESSION`
  to `current_where_c` class was added, so their route-review bit is stale.
  The updated policy passes its unit tests and focused `whereA` A/B/A runs on
  both engines with `route_review_required=false`:
  `/dev/shm/wherea-route-review-memtx-20260929/report.json` and
  `/dev/shm/wherea-route-review-vinyl-20260929/report.json`. This addresses
  that classification delta, not the universal M3.5 producer gate.

  **Unsigned secondary equality correction (2026-09-29).** The focused failure
  was `SELECT i FROM t WHERE i = 18446744073709551613` after creating an index
  on `i INT`; the adjacent range query did return its row. The cause was that
  an `INT` secondary key accepts the full signed/unsigned MsgPack integer
  domain, while equality extraction let `sql_atoi64()` wrap a positive wide
  literal through a signed output parameter. `parse_pk_bound()` now retains
  positive equality literals above `INT64_MAX` as unsigned keys. The first
  correction exposed a lowerer's declared-type check that rejected unsigned
  probes on an `INT` index; equality lowering metadata now validates the
  encoded probe representation. Focused generated and LLVM off/on/off captures
  of the full `sql/types.test.lua` file pass on memtx and Vinyl on
  `4d3cba0e6b` (395 queries per engine and mode; zero semantic or repeat diffs,
  zero unreviewed route transitions). Reports are
  `/dev/shm/types-ab-current-generated-1790672690/report.json` and
  `/dev/shm/types-ab-current-llvm-1790672690/report.json`. The observed
  `fallback/UNSUPPORTED_EXPRESSION` to `new_planner` adoption is now explicitly
  included in the SQL route policy because this file exercises the supported
  wide-integer equality and range cases. Current-source full reviewed SQL
  captures were then rerun in generated and LLVM modes on `e9c930fc02`,
  excluding only the documented fixed-mode `iproto.test.lua` observer-counter
  incompatibility. Both modes pass off/on semantics and exact off-repeat
  semantics with zero unreviewed transitions: 33 memtx tests / 1,077 queries
  and 34 Vinyl tests / 1,085 queries. Reports:
  `/dev/shm/sql-full-generated-1790672830919806609/report.json` and
  `/dev/shm/sql-full-llvm-1790672830970913834/report.json`. These close current
  reviewed SQL generated/LLVM parity, but not the wider M3.5 universal producer
  gate or remaining M3.7 functional scope.

  **Embedded INSERT-SELECT route slice (2026-09-28).** `insert.c` now labels
  its snapshot-mode `sqlSelect()` producer as the explicit root role
  `insert_select_root`; ledger validation and statement-summary selection treat
  that role as a root while retaining the normal root identity/parent
  invariants. A focused luatest checks the snapshot records one complete
  non-fallback embedded route and that executing the INSERT populates target
  rows. DELETE view materialization and SELECT trigger-step producers remain
  open, and this slice does not close M3.5's broader reviewed-corpus gate.

  **View-DML materialization route slice (2026-09-28).** The shared helper
  used by DELETE and UPDATE against a view now records its embedded SELECT
  root as `dml_view_materialization_root`; the ledger accepts that role as a
  root and uses it for the statement summary. A DELETE-from-view snapshot test
  verifies the route, stable `UNSUPPORTED_SUBQUERY` reason, and actual
  INSTEAD-OF-trigger row change. A matching UPDATE-from-view snapshot/runtime
  test now verifies the same route and that the INSTEAD-OF trigger updates the
  underlying row. SELECT trigger-step producers and the full reviewed producer
  inventory remain open.

  **Trigger-step producer probe (2026-09-28).** Attempted to expose SELECT
  steps in INSERT trigger bodies as independent component roots. The snapshot
  did not contain a trigger-body component even when `sqlSelect()` was invoked
  during trigger compilation; only the enclosing INSERT-SELECT root was
  recorded. The speculative role and test were reverted. Trigger-step route
  coverage remains open until the producer lifecycle/ledger ownership is
  understood; no unsupported route claim is made.

  **Trigger ledger ownership follow-up (2026-09-28).** Instrumented diagnosis
  confirms `sql_row_trigger_program()` is reached for snapshot EXPLAIN with
  both the sub-parse and top-level parse in snapshot mode (`explain == 4`),
  and the INSERT-SELECT producer hint is present before trigger compilation.
  Nevertheless the top-level snapshot has no planner ledger for a standalone
  trigger SELECT, and the combined INSERT-SELECT case lacks its expected root.
  The parse-mode propagation hypothesis is therefore ruled out. The ownership
  fix attaches registration and route updates to the top-level VDBE, uses one
  monotonic SELECT ID sequence across trigger sub-parses, and assigns explicit
  `trigger_select_root` / `trigger_select` roles. The focused INSERT-trigger
  luatest verifies standalone trigger ownership, parent linkage under an
  INSERT-SELECT root, route classification, and successful DML execution.
  The `sql_plan_component.test` target passes 40 route-ledger assertions and
  the typed-capture validator suite passes 4 tests. The integrated Clang-19
  Debug build with SQL CnP enabled passes for `box` and `tarantool`; the
  focused luatest passes under generated and CnP dispatch, and
  `sql_plan_component.test` passes all 40 assertions. The full SQL-luatest
  suite passed in both generated and CnP modes:
  54 passed, 1 skipped because volatile ANALYZE requires a TEST_BUILD server,
  and 2 disabled in each mode. Focused generated and CnP typed captures plus
  manifest validation passed with 41 snapshots per engine (memtx and Vinyl);
  CnP observed 39 native executions per engine. LLVM is not enabled in this
  build, and the reviewed SQL-TAP corpus remains unverified for this integrated
  revision. The implementation commits `2a02e93e67` through `95a93e83a7` are
  now integrated into the canonical branch. Focused generated and CnP captures
  each validate 41 snapshots on memtx and Vinyl, with 39 observed native CnP
  executions per run; strict generated-vs-CnP comparisons are exact (41/41)
  on both engines, and the typed-capture validator passes 4/4. LLVM is not
  enabled in this build, and the reviewed SQL-TAP corpus remains unverified
  for this integrated revision. M3.5 remained open for full reviewed-corpus
  inclusion and the complete producer/route inventory.

  The updated INSERT-SELECT/view-DML focused luatest also passes the typed
  per-engine capture audit in generated, CnP, LLVM, and generated-repeat
  modes on memtx and Vinyl: all 41 statements per run validate, with zero
  snapshot differences and positive native participation in CnP/LLVM. Its
  route assertions remain a diagnostic exclusion from immutable result
  snapshots, now explicitly recorded in the full-corpus policy with normal
  runner evidence for both engines. The reviewed inventory has 402 tests,
  588 included pairs, 216 excluded pairs, and no pending engine decisions.

  **SQL-language capture sweep (2026-09-28).** The local single-child audit
  attempted generated typed capture for all 66 `sql/*.test.lua` files across
  both engines: 89 of 132 test/engine runs were accepted and 43 failed capture
  or normal-runner verification, with failures concentrated in persistence,
  DDL/protocol, multi-child, and specialized runner fixtures. The planner
  signed-boundary fixture initially failed because its checked-in `.result`
  still expected the earlier `INT64_MIN` fallback; after the endpoint route
  fix and golden update it passes on both engines and in generated/CnP/LLVM/
  repeat captures. This is a broad triage report, not a clean SQL-corpus
  acceptance; it is retained locally at `/dev/shm/m3sql-normal-report.json`.

  **Capture-harness recheck (2026-09-28).** Fixed a lifecycle bug in
  `luatest_capture.py`: test-run deletes its `--vardir`, so that path must be
  nested below (rather than equal to) `TemporaryDirectory`'s root. A fresh
  complete run of `audit_sql_normal.py` against the fixed harness attempted
  all 66 SQL-TAP Lua tests on both engines. It accepted 89/132 runs (44
  memtx, 45 Vinyl), matching the previous aggregate count. One asymmetric
  result is `transitive-transactions.test.lua`, which captures on Vinyl but
  yields during capture on memtx. The remaining
  failures now expose runner/capture outcomes rather than a temp-directory
  cleanup exception: among them are tests with only remote/local configs,
  runner restarts or secondary servers, transaction-yield capture failures,
  and the 45-second `misc.test.lua` timeout. The audit is recorded at
  `/dev/shm/sql-normal-fixed-report.json`; this generated-only pass does not
  establish CnP/LLVM parity and leaves the M3.5 corpus gate open.

  **Extended timeout triage (2026-09-28).** A fresh standalone retry with a
  60-second per-file limit accepted `in2`, `select2`, and `select9` on both
  engines, with 6,001 / 30,073 / 21,313 captured statements respectively and
  complete component ledgers. `sort.test.lua` still exceeded the cap on both
  engines (61.3s memtx / 61.8s Vinyl); `autoindex1` and two tuple-memory stress
  cases were not retried. This changes the outstanding SQL-TAP timeout count
  from seven to four per engine, but remains standalone capture evidence, not
  normal-runner parity or final M3.5 closure.

  **Remaining timeout follow-up (2026-09-28).** Extending the retry to the
  other timeout cases accepted `autoindex1.test.lua` (20,496 statements) and
  `gh-3083-ephemeral-unref-tuples.test.lua` (11,001 statements) on both
  engines, again with complete ledgers. `sort.test.lua` and
  `gh-3332-tuple-format-leak.test.lua` still exceeded 60 seconds on both.
  Thus two known standalone SQL-TAP files remain uncollected per engine; this
  does not disposition normal-runner parity or the persistence-dependent
  ANALYZE rejections.

  **Normal-runner spot check (2026-09-28).** Using the project Debug build and
  the regular test-run harness, `in2`, `select2`, `select9`, and `autoindex1`
  passed on all configured engine variants (7 runs: `in2` memtx; the other
  three on memtx and Vinyl). This resolves the previously missing ordinary
  runner evidence for these four timeout-triage files only. It does not cover
  the then-still-uncollected two standalone files, the full SQL-TAP corpus,
  SQL/SQL-luatest reviewed corpus, or the remaining M3.5 route/reason
  dispositions.

  The same regular-runner check passes `sort.test.lua` and
  `gh-3332-tuple-format-leak.test.lua` on their explicitly configured memtx
  variants (5.8 seconds each, with the latter run under `--long`). These
  ordinary test passes did not by themselves change the separate
  baseline-capture timeout known at that point: both exceeded the 60-second
  standalone capture budget, and Vinyl was not configured for these two
  ordinary runs. The following extended capture follow-up supersedes that
  timeout status.

  **Extended standalone capture follow-up (2026-09-28).** Re-running those
  two files without the 60-second subprocess cap accepted both engines:
  `sort` wrote 100,119 snapshots per engine and
  `gh-3332-tuple-format-leak` wrote 100,014 per engine, with zero skipped or
  errored records and accepted manifests. The TAP cases passed in each run.
  This clears the two previously outstanding standalone SQL-TAP captures;
  their long capture duration is a resource/runtime concern, not missing
  baseline evidence. The broader M3.5 corpus and route/reason gates remain.

  **SQL-luatest capture extension (2026-09-27).** The child capture adapter
  now records planner metrics and component routes for successful
  SELECT/WITH-SELECT/VALUES executions, alongside each SQL snapshot's
  `path_class` and `fallback_reason`; the manifest identifies ledger v1 and
  metrics v2. `planner_final_paths_test.lua` now executes direct scan,
  new-planner scan, VALUES, OP_Count, and mixed compound routes in addition
  to EXPLAIN assertions. Generated memtx capture passed (27 snapshots, 5
  route-bearing statements, 9 component records); generated Vinyl capture
  passed with the same counts. The CnP crash was an ABI mismatch: stencils
  called `SQL_PRESERVE_NONE` handlers using SysV argument registers. A typed
  bridge now performs the calling-convention transition; the expanded
  `planner_final_paths_test.lua` and `planner_flag_parity_test.lua` both pass
  under CnP and generated dispatchers. The flag matrix exposed a second
  correctness bug in fast MessagePack string extraction: argument evaluation
  could store the encoded string marker instead of its payload. Both string
  decoders now advance to the payload before constructing the ephemeral Mem;
  a direct multirow string VALUES assertion guards the path. The component
  cap is now 4,096 records (the previous cap of 128 truncated the reviewed
  1,000-row VALUES/MAP query); `map_test.lua` now captures successfully.
  The full single-child generated audit passed 41 memtx files (912 SQL
  statements / 1,357 component records); 12 files were explicitly not run
  because they use multiple/restarted children, prepared/net.box bypasses, or
  are long-run. The Vinyl audit captured 39 files (494 statements / 1,163
  records); two existing engine-specific tests fail when the adapter forces
  Vinyl and remain dispositioned, and the same 12 were not run. Every captured
  SELECT ledger was complete. On both engines, the targeted `ANALYZE`, 1,000-
  row MAP/VALUES, and planner fixtures passed CnP vs generated parity and a
  repeated generated capture. Explicit planner-snapshot results now normalize
  only `planner.elapsed_us`; route and all other fields remain compared, while
  raw elapsed values remain in manifest metrics. The current build has
  `ENABLE_SQL_JIT=OFF`, so LLVM execution is not observed here. These are
  broad but adapter-limited SQL-luatest results; the remaining unsupported
  topologies and SQL-suite audit keep M3.5 open.

  **CnP arithmetic fallback follow-up (2026-09-27).** The first full audit
  exposed an additional calling-convention hole: arithmetic selectors that
  rejected integer specialization resolved to raw preserve-none handlers
  instead of the SysV bridges. `cnp_resolve_handler_by_opcode()` now returns
  the bridge for Add/Subtract/Multiply/Divide/Remainder. The four reproducing
  tests (`defaults_test.lua`, `seq_scan_test.lua`,
  `gh_6773_arithmetic_operands_test.lua`, and
  `gh_8460_wrong_int_to_dec_test.lua`) pass under CnP, and the rerun full
  adapter audit has no CnP capture failures: 33 memtx and 32 Vinyl files pass
  exact native-vs-generated parity. One additional memtx datetime capture
  differs only at the nondeterministic current-time query. Seven files per
  engine have CnP not observed; 12 are explicitly not run for unsupported
  topology/long-run reasons, and two additional Vinyl CnP runs are gated by
  forced-Vinyl generated-capture failures. Generated-repeat comparisons pass
  except the datetime current-time query (one result differs by timestamp) and
  `explain_modifiers` disassembly (unstable address-dependent bytes); datetime's
  Vinyl forced-engine capture and `show_create_table`'s forced-Vinyl capture
  remain engine-specific failures. LLVM remains not observed because this
  build has JIT disabled. This improves CnP coverage but does not close M3.5's
  unsupported-topology or SQL-suite gates.

  **Full SQL-luatest recapture (2026-09-28).** A resumable normal-runner audit
  generated and validated 991 memtx snapshots across 42 files and 573 Vinyl
  snapshots across 40 files. Generated-repeat and CnP comparisons produced
  146 exact file-mode parities in the full sweep, including the isolated
  prefix-range fixture. A focused rerun after fixing the fallback capture
  assertion adds four exact CnP/repeat comparisons across the two engines.
  The fallback-parity test initially failed only because its exact global
  counter assertion observed the capture adapter's internal snapshot EXPLAIN;
  the assertion now accounts for that observer-only increment when capture is
  active, and the standalone test plus its generated/CnP/repeat audit pass on
  both engines. Remaining sweep findings are two forced-Vinyl generated
  capture failures (`datetime_test.lua`, `show_create_table_test.lua`), one
  nondeterministic current-time datetime result in CnP and repeat comparisons,
  and address-dependent `explain_modifiers` disassembly in repeat comparisons
  on both engines. LLVM is not observed with JIT disabled, and unsupported
  multi-child/restarted/direct-net.box topologies and long tests remain
  explicitly unrun. This is expanded review evidence, not a clean full-suite
  parity decision; M3.5 remained open pending these dispositions and the SQL
  suite/corpus gate.

  **Current full SQL-luatest ledger capture audit (2026-09-28, source
  `0dcfbd92a3bab76e48f28b2ed2387f73003e42ba`).** The current resumable audit
  enumerated 57 files; 9 were explicitly not run because of multiple-child,
  restart, net.box bypass, or long-run topology. Generated capture passed on
  46 memtx and 44 Vinyl files (1,642 / 1,224 statements). Across all modes,
  256 accepted manifests contain ledger v1 and 8,572 validated snapshots.
  Exact CnP parity passed for 38 memtx and 37 Vinyl files; one additional
  memtx datetime case differs only at volatile `NOW()`. Exact generated-repeat
  parity passed for 44 memtx and 43 Vinyl files; remaining drifts are the same
  volatile datetime result plus address-dependent `explain_modifiers`
  disassembly. The known TEST_BUILD-only ANALYZE test and forced-Vinyl
  `datetime` / hard-coded-memtx `show_create_table` captures still fail;
  seven CnP runs per engine are not observed, and LLVM is unavailable in this
  build. Report: `/dev/shm/m35-luatest-ledger-current.json`. This strengthens
  component-ledger coverage but preserves M3.5 open for unsupported topology,
  engine-scoped inclusion, volatile/repeat dispositions, LLVM, and full
  reviewed-corpus coverage.

  **Prepared SQL topology slice (2026-09-28).** The one-child adapter now
  records SQL passed to `box.prepare`, maps returned statement IDs until
  `box.unprepare`, and captures later `box.execute(stmt_id, bindings)` calls
  and statement-handle `:execute()` methods under the original SQL text
  without changing execution arguments or results. Prepared executions carry
  explicit query indices in the manifest;
  any non-string execution whose SQL cannot be recovered makes capture fail
  closed after the normal test run. `gh_6422_autoinc_ids_reset_test.lua` and
  `gh_7358_prepared_stmt_truncation_test.lua` each passed generated, CnP,
  LLVM, and generated-repeat capture/parity on memtx and Vinyl (4 and 8 SQL
  statements respectively, including the prepared executions). This closes
  the prepared-statement hook gap for these source topologies only; it does
  not accept the existing full-corpus exclusions or establish corpus-wide
  parity, which still requires the normal review/anchor process. Direct
  `net.box`, multiple/restarted children, and unobserved prepared IDs remain
  excluded.

  ```mermaid
  flowchart TD
    R[Root SELECT component] --> C[Child SELECT components]
    R --> RR[Route + optional fallback reason]
    C --> CR[Route + optional fallback reason]
    RR --> S{All routes uniform?}
    CR --> S
    S -- yes --> U[Summary mirrors root route]
    S -- no --> M[mixed; no statement fallback reason]
  ```

  **M3.5 acceptance audit and closure (2026-09-29; post-projection refresh).**
  The Debug off/on/off matrix passed in all
  nine suite/dispatcher combinations (SQL-TAP, SQL, SQL-luatest × generated,
  CnP, LLVM), across memtx and Vinyl, with zero semantic diffs, exact
  off-repeat semantics, and no unreviewed route classes. SQL-TAP covers 232
  memtx / 224 Vinyl tests and 47,946 / 37,990 snapshots in every mode. The
  reviewed SQL selection covers 33 / 34 tests and 1,077 / 1,085 snapshots;
  SQL-luatest covers 32 / 31 tests and 499 / 447 snapshots in generated mode,
  and 31 / 30 tests and 498 / 446 snapshots in CnP/LLVM. The one SQL exclusion
  is the `box.stat().EXECUTE` observer-counter incompatibility in
  `iproto.test.lua`; `gh_8676_exists_in_multiselect` is excluded from native-
  mode capture because it emits only a direct VALUES route, and both remain
  covered in generated mode. All observed route classes, including
  secondary-index `NO_ACCESS_PATH` fallbacks, reason-only refinements, and the
  reviewed unindexed-collation residual transition, are dispositioned in the
  route policy. The fresh aggregate report is
  `/tmp/m35-after-m34-48fc/acceptance.json`; all nine suite reports and the
  current-source 24-case producer matrix are under
  `/tmp/m35-after-m34-48fc/`. This checkpoint predates the deterministic
  projection extension documented below; its source was separately checked
  against focused generated/CnP/LLVM cases and the SQL-TAP generated corpus.
  A current-source focused rerun passes `planner_final_paths`,
  `planner_insert_select_snapshot` (including INSERT-SELECT, DELETE/UPDATE view
  materialization, and trigger SELECT), `planner_flag_fallback_parity`, and
  `planner_composite_prefix_range`. Those focused results and earlier
  aggregate files under `/dev/shm` are historical checkpoints; the fresh
  `/tmp/m35-after-m34-48fc/acceptance.json` supersedes them for current-source
  M3.5 evidence. These results close the M3.5 accounting/coverage gate, not
  broad execution support for additional logical shapes or M3.4/M3.7. The fresh
  aggregate has zero blockers and records `feature_acceptance_passed=true` for
  source `48fc881413c494535fa3d30aa830cc1de8c77ad0`.

  **Post-projection evidence refresh.** The nine broad suite/mode reports
  were rerun against source `c4bb6215e2` in `/tmp/m35-final-c4bb/`; the
  24-case producer matrix was rerun against the updated expectation-only
  fixture at the current source. Both have zero semantic diffs and no
  unreviewed transitions. The current aggregate is
  `/tmp/m35-final-c4bb/acceptance-postdocs.json`. Only roadmap documentation
  and acceptance-test policy files changed since the broad capture; no
  production or corpus expectations changed. M3.4 remains partial.

  **Post-range-lowering evidence refresh (2026-09-29).** After the ordered
  one-sided primary-range lowering landed at source `3dfb754eec`, the complete
  off/on/off corpus matrix was rerun across SQL, SQL-luatest, and SQL-TAP in
  generated, CnP, and LLVM modes. All nine reports cover both memtx and Vinyl,
  have zero semantic diffs and zero unreviewed route transitions, and record
  the same source commit. The fresh 24-case all-producer matrix also passes;
  `planner_flag_acceptance.py` aggregates all nine reports with
  `feature_acceptance_passed=true` and no blockers at
  `/tmp/m35-post3dfb-resized/acceptance.json`. The SQL-TAP LLVM run includes
  47,946 memtx and 37,990 Vinyl queries, zero semantic diffs, and reviewed
  off-repeat captures. This refresh confirms M3.5 acceptance after the M3.4
  range change; M3.4 remains partial and this does not imply broad lowering
  or M3.7 completion.

  **Planner-flag runner exclusion fix (2026-09-29).** The runner now applies
  the documented fixed-mode exclusions by default and records their reasons
  per engine; an explicit request for an excluded test fails with the reason.
  SQL LLVM off/on/off was rerun using the default selection on source
  `a8960c388ff53da4ceff26e718906f0a3a9b846d`: 33/34 tests and 1,077/1,085
  queries, zero semantic or repeat diffs, and zero unreviewed route classes.
  The report is `/dev/shm/sql-llvm-default-reviewed-20260929/report.json`.
  Six unit tests cover route classification and mode-specific selection.
  Individual reports keep `feature_acceptance_passed=false` because a single
  suite cannot certify the milestone; the aggregate verifier now supplies the
  complete M3.5 decision.

  **Fresh M3.5 aggregate (2026-09-30).** Re-ran all nine suite/mode reports
  (SQL-TAP, SQL, and SQL-luatest × generated, CnP, and LLVM) against source
  `e19fa284b657534d0330108249538b6322425814`, including both memtx and Vinyl
  for every report. All reports have reviewed route transitions and certified
  semantic parity, including exact off-repeat semantic parity. The producer
  runner completed all 30 cases (the 24 required four-fixture cases plus the
  scalar-filter fixture); every case passed. The aggregate verifier reports
  `feature_acceptance_passed=true`, nine suite/mode reports, and no blockers.
  Evidence is under `/dev/shm/m35-refresh-e19fa284b6/`; the authoritative
  aggregate is `acceptance.json`. LLVM/Vinyl SQL-TAP has 35 EXPLAIN-only
  off/on row differences; these are not semantic diffs and are explicitly
  allowed by the acceptance comparator. This closes the adopted M3.5 gate,
  not M3.4's broader executable-lowering work or M3.7 functional acceptance.

  *parallel: no*.
- [x] **M3.6 prototype** M0 snapshot capture now asks
  `EXPLAIN (planner = 'snapshot')` for SELECT statements and records its
  `path_class` / `fallback_reason`, instead of hardcoding
  `current_where_c`. A multi-relation statement now produces and captures
  `fallback` / `UNSUPPORTED_RELATION_COUNT`; live harness capture now also
  verifies aggregate, compound, DISTINCT, subquery, and CTE reasons. Constant
  zero-source SELECTs now use the same relation-count reason; this changes L3
  from their prior unclassified `current_where_c` value, so baseline recapture
  and review of that path shift are still pending. The M0
  normalizer preserves path, reason, and `fallback_to: current_where_c`; live
  harness capture and schema validation exercise the complete route. Other
  new-planner/fallback classes, baseline recapture, and parity validation
  remain open. The capture validator now checks fallback metadata as a
  coherent contract: a fallback requires a recognized stable reason and
  `fallback_to: current_where_c`, while non-fallback paths must not carry
  either field. Negative validation tests cover missing/unknown reasons,
  wrong fallback destinations, and fallback metadata attached to a
  `current_where_c` path. This closes a schema-validation hole, not the
  broader M3.6 capture/parity gate. The live harness now also fails capture if
  planner-snapshot EXPLAIN fails, returns no MsgPack, or violates the v5
  envelope's replayable/input consistency contract; it no longer silently
  records missing metadata as an ordinary path. It also preserves a nil path
  as `l3_path_class.taken: null` for statements that do not enter the WHERE
  planner, instead of crashing or mislabeling them `current_where_c`; the
  schema, validator, and a DML fixture cover this case.
  A targeted standalone audit now captures and validates all 30,073 statements
  in `select2.test.lua` under both memtx and Vinyl; this is not a baseline
  recapture or full-corpus parity result. When a manifest includes planner
  metrics, validation now checks unique ordered query indexes, valid
  path/reason combinations, and per-query agreement with the snapshot's L3
  path metadata; a capture regression changes a valid metric reason to a
  different valid code and verifies rejection. A v2 metrics manifest must
  also contain an entry for every successful SELECT/WITH snapshot; older
  manifests that omit the extension remain accepted. Expected failed
  SELECT/WITH probes stay gated on L2 but do not require a planner envelope:
  EXPLAIN may reject the same invalid statement, so their path is null. A
  missing-relation SELECT regression exercises this exception while successful
  SELECT metric coverage remains fail-closed. This closes the metrics-coverage
  hole, not baseline recapture or full M3.6 parity. The old `04b63d19` anchor
  cannot be recaptured because its server predates the planner snapshot envelope
  required by `EXPLAIN (planner = 'snapshot')`. L3 transitions were reviewed
  against snapshot-capable captures, then the reproducible anchor was promoted
  to `d8fc1e339b0bb8579c8b08e39d062e20edf66666`. Recaptures from that anchor
  have matching coverage (298 memtx tests / 49,535 statements; 290 Vinyl
  tests / 39,535 statements) and exact zero-diff results on both engines.
  The exploratory comparison against `2bae9591` had no L1, L2, column
  metadata, or other field drift; its 13,258 memtx / 11,225 Vinyl L3 changes
  across 11 transition classes were reviewed as intended classification
  refinements. `planner_fallback_no_from` remains an evidence-backed
  diagnostic-only exclusion; baseline capture permits its absence only when
  the selected immutable anchor lacks the fixture, while candidate inventory
  always requires it. M3.6 capture/parity prototype is complete. At this
  checkpoint, new planner implementation, M3.5 classification closure, and
  M3.7 remained open.
  *parallel: yes*.
- [x] **M3.7** Feature flag `sql_new_planner_single_table=on/off` — accepted for
  the current production route scope. The flag is default-off and session-local;
  every executable SELECT path in the current bounded planner is gated.
  A default-off session setting now gates the narrow direct-column table scan,
  sole INTEGER/UNSIGNED primary-key point lookups, one-sided primary-key
  literal ranges, intersected lower/upper bounds on one primary-key part, and
  ranges on the next key part after a contiguous equality prefix in
  `sqlSelect()`. When enabled, only the supported
  single-table shape with a TREE primary index can report `new_planner`: direct
  projections, primary-key ordering compatible with the range direction, and
  literal LIMIT/OFFSET. This happens only after
  physical descriptor creation and VDBE lowering succeed; tested physical
  rejection (including non-primary ordering) and recoverable codegen rejection
  retain legacy codegen with a reason. The
  General physical candidate selection is not yet wired into production
  (M3.4 scope); this does not leave any currently executable new-planner route
  outside the flag. Default-off behavior and off/on/off summary route
  checks for scan and point routes pass in the focused memtx/Vinyl
  regression. A bound one-part INTEGER primary-key equality now also follows
  the executable `new_planner` route without changing fallback counters. The
  off/on/off matrix permits either `current_where_c` or a named fallback for
  a disabled query whose shape is not executable by the current planner, while
  preserving row parity. A two-sided INTEGER primary-key range now also has
  explicit flag-on `new_planner` and flag-off `fallback` route assertions plus
  result parity on both memtx and Vinyl. `planner_preflight.test.lua` now adds a
  seven-query off/on/off row-parity matrix spanning table scan, LIMIT/OFFSET,
  ordered scan, point lookup, one-sided bounds, and a two-sided range; it also
  asserts `new_planner` while enabled and accepts `current_where_c` or a
  reasoned fallback while disabled. The focused test passes on memtx and
  Vinyl. This is representative route
  coverage, not yet the wider supported-shape/corpus acceptance gate. A
  separate `sql-luatest/planner_flag_parity_test.lua` now extends off/on/off
  row parity to eight supported UNSIGNED-primary-key queries across memtx and
  Vinyl, including signed-boundary/UINT64_MAX points, one- and two-sided
  ranges, compatible ordering, LIMIT, and OFFSET. All enabled queries assert
  `new_planner`; disabled queries assert `current_where_c`, except the
  high-half UNSIGNED literals, which report
  `fallback / UNSUPPORTED_EXPRESSION` while retaining result parity. The local
  focused luatest passes against a binary rebuilt from current HEAD. This
  broadens typed/boundary coverage, but remains a focused sample rather than
  corpus-wide feature acceptance. The same matrix now also checks a fresh
  session before any explicit setting change, pinning default-off route and
  result behavior against explicit-off on both engines; the focused luatest
  passes locally. The same test now covers unordered and ordered TEXT primary-
  key scans in off/on/off phases, verifies descending LIMIT results, and checks
  fallback-reason absence plus exact counter deltas; it passes on memtx and
  Vinyl. The signed INTEGER range-boundary regression also checks the full
  domain and strict/inclusive minimum/maximum bounds. All supported enabled
  cases report `new_planner`; all flag-off cases report `current_where_c`.
  The canonical expression grammar now recognizes the parser's unary-minus
  representation of `INT64_MIN`, avoiding a false unsupported-expression
  fallback. The test asserts unchanged total and per-reason fallback counters
  across EXPLAIN plus execution and confirms row parity; memtx and Vinyl pass.
  A new `planner_composite_pk_order.test.lua` acceptance case confirms the
  prefix and full-key routes report `new_planner`, preserves ASC/DESC row order
  on both engines, and leaves mixed-direction or non-prefix ordering on the
  legacy route with stable fallback reasons and exact flag-off/on result
  parity. The SQL-luatest off/on/off matrix now covers composite-key prefix,
  full ascending/descending order, prefix/full-key LIMIT/OFFSET, zero LIMIT,
  and mixed-direction rejection on memtx and Vinyl. It asserts enabled output
  order, row parity, stable disabled
  `UNSUPPORTED_EXPRESSION` classification for ordering outside the resolved
  expression contract, and exact total/per-reason fallback deltas (14 for
  seven disabled EXPLAIN+execution pairs, two for the single rejected enabled
  pair). Opposite-direction one-sided ranges now also pin flag-on
  `UNSUPPORTED_FILTER` versus flag-off `UNSUPPORTED_EXPRESSION` with ordered
  result parity. The focused luatest passes locally. A separate two-connection case
  also verifies session isolation: enabling or disabling
  `sql_new_planner_single_table` changes only that net.box session's route,
  while another connection retains its prior/default route. This focused
  runtime check passes; broad parity/corpus validation and feature acceptance
  remain open. After the v5 per-component snapshot integration, the complete
  `planner_flag_parity_test.lua` SQL-luatest was rerun against the rebuilt
  binary and still passes on memtx and Vinyl, confirming the new ledger does
  not disturb default-off, off/on/off, or session-isolation behavior.
  The matrix also covers opt-in unordered HASH-primary full scan on memtx:
  flag-off preserves the existing non-TREE rejection, while flag-on reports
  `new_planner` and returns all rows; generated and CnP dispatch pass. This
  does not change the default-off session contract.
  A focused fallback-parity luatest now runs deterministic-function and
  `NOT INDEXED` queries through off/on/off phases on both memtx and Vinyl. It
  asserts stable `UNSUPPORTED_FUNCTION` / `UNSUPPORTED_ACCESS_HINT` routes,
  identical executed rows, and exact total-reason counter deltas for enabled
  and disabled EXPLAIN plus execution. This extends feature-flag evidence to
  unsupported shapes without claiming corpus-wide acceptance. A fresh
  generated/CnP capture on both engines matches exactly (47/47 snapshots per
  engine, zero errors or diffs), including the capture observer's counter
  contribution.
  Composite-primary-key prefix predicates now also participate in the same
  off/on/off feature gate: equality scans, one-sided bounds, a two-sided bound,
  and descending equality-prefix order return identical rows on memtx and
  Vinyl. Every enabled case reports `new_planner`; disabled cases preserve
  legacy execution with either `current_where_c` or a stable fallback reason.
  Generated and CnP focused runs pass. This remains targeted route evidence,
  not corpus-wide feature acceptance. The same off/on/off matrix now covers
  exact equality over two- and three-part INTEGER/UNSIGNED primary keys,
  including reversed predicate order, UINT64_MAX, and a miss. Enabled queries
  assert `new_planner`; disabled `fallback` outcomes must include a reason.
  Three-part point cases also verify LIMIT and OFFSET result parity and retain
  `new_planner` when enabled. Generated and CnP runs pass on memtx and Vinyl.
  Prefix-scan cases verify ascending ORDER BY over single- and multi-column
  unfixed contiguous primary-key suffixes, plus a key-order prefix including
  equality-fixed columns. Descending suffix order over an equality-only prefix
  now uses `SeekLE` on the prefix and `Prev` until the prefix guard fails;
  off/on/off coverage includes LIMIT/OFFSET and passes on memtx and Vinyl.
  Descriptor and VDBE unit tests pin acceptance and opcode/guard placement
  (7 contract assertions, 61 lowering assertions). The expanded isolated
  fixture captures 240 snapshots per engine in generated, CnP, and repeated-
  generated modes; both comparison runs are exact, and CnP participation is
  observed. The feature-flag matrix now asserts `new_planner` and exact rows
  for descending single- and multi-column suffix ordering over an equality
  prefix; the scalar-filter matrix likewise expects the already-supported
  composite suffix-range plus residual-filter route. The full local
  SQL-luatest suite passes at this revision (54 passed, 1 skipped for volatile
  ANALYZE's TEST_BUILD requirement, 2 disabled). LLVM remains unavailable in
  this build.
  The scalar-filter off/on/off matrix now also asserts route selection for
  non-primary comparisons, BETWEEN/NOT BETWEEN, IN/NOT IN (including NULL
  list semantics), bounded OR/NOT trees, and combinations with primary-key
  bounds on both engines. All supported enabled cases report `new_planner`;
  disabled cases preserve legacy route and results. Generated/CnP/LLVM/repeat
  captures compare exactly at 655 snapshots per engine. This closes another
  focused flag slice, not corpus-wide feature acceptance.
  Equality-prefix-plus-next-part suffix ranges now also report
  `new_planner` for lower-only, upper-only, bounded, high-UNSIGNED, and
  literal-left forms. Their isolated off/on/off fixture has 72 snapshots per
  engine and exact generated-repeat/CnP parity; descriptor and VDBE lowering
  unit coverage pins accepted/rejected metadata and seek/termination opcodes.
  The scalar non-primary `IS NULL` / `IS NOT NULL` scan route also has exact
  off/on/off result assertions, plus exact generated/CnP parity on memtx and
  Vinyl (85/85 snapshots per engine); generated-repeat also matches exactly.
  Single-part primary-key points admit up to eight such residual filters.
  Complete composite INTEGER/UNSIGNED primary-key equality now also accepts
  the bounded residual list. The memtx/Vinyl off/on/off matrix verifies mixed
  NULL/NOT NULL predicates, reversed order, hit/reject/miss behavior, and
  `new_planner` on enabled execution. Multiple residuals now also pass through
  supported scans and single-part ranges. Composite suffix ranges remain
  fail-closed; leading-part equality uses the existing bounded-range route.
  Bounded residual lowering now spans full scans, supported single-part
  primary-key ranges, and complete single-/composite-key points. Up to eight
  direct non-primary NULL / NOT NULL checks run before projection; unsupported
  composite suffix ranges and filter-list overflow remain fail-closed. The
  memtx/Vinyl off/on/off matrix covers conjunction-only scans, one-/two-sided
  ranges, point hit/reject/miss, contradictory filters, and composite point
  equality. The integrated Debug build and all 49 VDBE assertions pass; the
  focused SQL and broader flag/component matrices pass under generated and CnP
  dispatch. This adds bounded route evidence only; broader M3.4 operators,
  producers, and reviewed-corpus acceptance remain open.
  **Reviewed SQL-TAP flag capture (2026-09-28).** The new fixed-mode capture
  option (`--planner-flag=off|on`) pins the session setting before test load,
  rejects tests that mutate it, and records the mode in each manifest. The
  off/on/off corpus runner currently scopes this path to reviewed SQL-TAP
  entries; ordinary M0 capture/inventory remains unchanged. Both engines
  passed semantic off/on parity and exact off/off repeatability across all
  accepted SQL-TAP tests: 47,946 memtx queries and 37,990 Vinyl queries.
  A fresh generated-mode audit at source `3d9429c3e9e370da26bf65b2d62ba97d5c8dc741`
  reports 1,719 / 1,691 enabled `new_planner` statements and 24 EXPLAIN-row-
  only differences per engine. Its seven route/reason transition classes
  total 6,078 memtx / 4,044 Vinyl query transitions: five classes now have
  evidence-backed structural dispositions, and representative runtime cases
  now pin the two reason-only precedence shifts. This does not disposition
  every query in those class counts or close M3.5's universal producer gate;
  the detailed inventory follows below.
  The route audit now matches transition classes against
  `test/sql-baselines/planner_flag_route_classes.json`: known classes are
  labeled `documented`, while any newly observed class is marked
  `unreviewed` and keeps `route_review_required=true`. This is a drift alarm,
  not blanket approval: `feature_acceptance_passed` remains false until the
  M3.5 producer gate and LLVM reviewed-corpus parity close. Unit tests cover
  both exact policy matches and unknown-class rejection.

  **Current reviewed SQL-TAP flag audit (2026-09-28, source
  `fe76fc128907bd33f3aaadb884176228ccac0d38`).** Re-ran all reviewed tests in
  generated mode; report: `/dev/shm/m35-current-route-audit/report.json`.
  Memtx covered 232 tests / 47,946 queries and Vinyl 224 tests / 37,990
  queries. Off/on semantic parity and off-repeat semantic parity both passed
  with zero semantic diffs on both engines. The remaining hard diffs are
  EXPLAIN-only (24 memtx / 23 Vinyl off/on; 5 / 4 repeat). The audit observed
  1,776 / 1,737 enabled `new_planner` routes and 6,130 / 4,085 route
  transitions; every transition matched the reviewed route-class policy, so
  `route_review_required=false`. `feature_acceptance_passed` remains false:
  this closes neither the SQL-luatest route inventory nor LLVM parity nor the
  universal M3.5 producer gate.

  **Current reviewed SQL-luatest flag audit (2026-09-28, source
  `0dcfbd92a3bab76e48f28b2ed2387f73003e42ba`).** The same generated-mode
  off/on/off audit covered all 32 reviewed memtx tests (499 queries) and 31
  reviewed Vinyl tests (447 queries). Both semantic and repeat comparisons
  passed with zero diffs, including EXPLAIN output. Each engine had 31 route
  transitions and 26 enabled `new_planner` queries; all transitions matched
  the reviewed policy (`route_review_required=false`). This is the reviewed
  subset only, not all SQL-luatests; it does not close M3.5's producer
  inventory or the LLVM acceptance gate.

  **Current reviewed SQL-luatest follow-up (2026-09-29, source
  `7f470a14ad978f53116f3d1a6fbd09fe9e5b16f8`).** Re-ran generated-mode
  off/on/off for the accepted reviewed subset: 32 memtx tests / 499 queries
  and 31 Vinyl tests / 447 queries. Semantic off/on and repeat comparisons
  both pass with zero diffs on both engines, including EXPLAIN output. The
  enabled capture records 31 `new_planner` routes per engine; 32 transitions
  per engine (31 to `new_planner`, one to `fallback / NO_ACCESS_PATH`) all
  match the reviewed route-class policy. This updates only the reviewed
  subset evidence; it does not close the broader producer inventory or LLVM
  acceptance gate. The local report is `/tmp/planner-flag-ab-1SuBev/report.json`.

  ```mermaid
  flowchart LR
    A[off/on/off capture] --> B[semantic and repeat parity]
    B --> C[route transition inventory]
    C --> D{class listed in reviewed policy?}
    D -->|no| E[unreviewed: require disposition]
    D -->|yes| F[documented class; no blanket acceptance]
    E --> G[M3.5 + LLVM gates still required]
    F --> G
  ```

  `sql_reverse_unordered_selects`
  stays on legacy codegen until reverse-order intent is represented in the
  descriptor. Upper-only ascending bounds now scan from the beginning and
  stop at the correct strict/inclusive boundary, covered by `in1.test.lua`.
  The route also preserves `sql_seq_scan`: when the session prohibits scans,
  an otherwise supported full-scan descriptor falls back to the legacy
  `ER_SQL_SEQ_SCAN` check, while keyed routes and explicit `SEQSCAN` remain
  available. `seq_scan_test.lua` now passes in a fixed off/on/off capture on
  both engines.
  A reviewed SQL-luatest route audit found an upper-only INTEGER range
  (`SELECT * FROM t WHERE i < 2`) transitioning to
  `fallback / INVALID_CANDIDATE` when `sql_seq_scan` was disabled. The range
  emitter already had a `Rewind` plus bound-check path for ascending upper
  bounds, but validation rejected that direction and its strict/inclusive
  comparison opcodes were reversed. The emitter now accepts upper-only
  ascending scans and exits on `bound <= current` for `<` or
  `bound < current` for `<=`. A later composite-prefix extension also supports
  descending lower-only ranges by seeking on the prefix and checking the
  suffix lower bound while walking backward (see the M3.4 evidence above).
  Unit checks pin both operators and branch placement. A live memtx/Vinyl
  off/on/off regression with `sql_seq_scan=false` asserts `new_planner` on the
  enabled route and exact `SELECT * ... WHERE id < 2` rows; the focused
  planner-flag suite passes.
  EQP output for full scans and primary-key point lookups now matches legacy
  detail, covered by `eqp.test.lua` and `lua-tables.test.lua`. The corpus run
  also exposed and updated the volatile ANALYZE error expectation; legacy
  opcode-shape checks in `distinct.test.lua` are skipped only in fixed flag-on
  captures, while both EXPLAIN statements still run. This is a successful
  SQL-TAP parity prototype, not feature acceptance. The capture path now also
  supports the normal `sql` and `sql-luatest` runners, pins planner mode per
  child client session, and isolates exact test identities despite test-run's
  substring selector. Off/on/off parity passed for 34/35 included SQL tests
  (1,077 memtx / 1,085 Vinyl queries; 62 enabled routes and 99 route shifts
  per engine) and all 32 included SQL-luatest tests (499 / 447 queries; 16
  enabled routes and 22 route shifts per engine). `sql/iproto.test.lua` is the
  sole omitted SQL entry: planner-snapshot EXPLAIN instrumentation changes
  its asserted `box.stat().EXECUTE` count. Its ordinary reviewed M0 capture
  remains unchanged. Across the three suites, semantic off/on comparison and
  deterministic off/off repeatability passed wherever fixed-mode capture was
  supported. Generated SQL-TAP still needs CnP/LLVM corpus validation and no
  CI parity gate consumes these reports yet.
  Complete fallback
  classification, wider parity/corpus validation, runtime observability, and
  feature acceptance remain open. Scope is explicitly session-local for this
  prototype, not an unresolved instance/session decision.
  **Post-fix reviewed SQL-luatest route review (2026-09-28, source
  `f0eb8bde1bd004fd15ce365b0ee497e98bba6cca`).** A full generated-mode
  off/on/off run over the accepted SQL-luatest scope passed semantic parity
  and exact off-repeat comparisons: 32 memtx / 31 Vinyl tests, 499 / 447
  statements, zero semantic diffs. On each engine, all 22 route transitions
  now reduce to 17 `current_where_c` → `new_planner` adoptions for supported
  scan/point/range forms and five `current_where_c` → `fallback /
  UNSUPPORTED_FILTER` transitions for filters not represented by the current
  descriptor (one `_space.owner` predicate and non-primary/arithmetic filter
  queries in `seq_scan_test.lua`). The latter execute on the legacy path and
  preserve the captured results; the former are the focused feature's
  supported route adoptions. The earlier `seq_scan_test/q14` transition to
  `INVALID_CANDIDATE` is absent after the upper-only range fix. The report is
  `/tmp/upper-range-commit-flag-review/report.json`. This dispositions the
  SQL-luatest route transitions only; reason-precedence review for SQL-TAP,
  the `sql/iproto.test.lua` observer-counter incompatibility, and corpus-wide
  feature acceptance remain open. *parallel: no*.
  **Reviewed SQL-TAP route-transition inventory (2026-09-28, source
  `3d9429c3e9e370da26bf65b2d62ba97d5c8dc741`).** A fresh generated-mode
  fixed-flag off/on/off run passed executed-result parity and off-repeat
  checks: 232 tests / 47,946 queries on memtx and 232 / 37,990 on Vinyl,
  with zero semantic diffs and 24 EXPLAIN-row-only differences on each
  engine. The current report groups the transitions into seven classes (the
  earlier estimate of eight was not backed by a retained report):
  `current_where_c → new_planner` (1,552 / 1,538),
  `fallback / UNSUPPORTED_EXPRESSION → new_planner` (167 / 153),
  `current_where_c → fallback / UNSUPPORTED_FILTER` (2,173 / 2,172),
  `mixed → fallback / UNSUPPORTED_RELATION_COUNT` (2,059 / 60),
  `mixed → fallback / UNSUPPORTED_SUBQUERY` (26 / 25), and two reason-only
  changes to `UNSUPPORTED_FILTER` from `UNSUPPORTED_AGGREGATE` (44 / 44) or
  `UNSUPPORTED_EXPRESSION` (57 / 52). The first five classes have coherent
  structural explanations in representative SQL: supported scan/order
  adoption, a previously rejected but now canonical expression, fail-closed
  residual filters, multi-relation rejection, and subquery rejection. Their
  results remain on the legacy executor when rejected, and strict semantic
  parity passes. The last two classes combine multiple unsupported features;
  choosing filter over aggregate/expression is a fallback-reason precedence
  change, not a result change. `planner_flag_fallback_parity_test.lua` now
  pins that precedence for representative `COUNT(*)` + unsupported-filter and
  SCALAR/BLOB-filter queries: flag-off retains the legacy aggregate or
  expression reason, while flag-on reports the physical producer's
  `UNSUPPORTED_FILTER`; both retain legacy execution and identical rows.
  Exact per-reason counter deltas pass on memtx and Vinyl under generated and
  CnP dispatch. A separate arithmetic-filter case also pins the off
  `current_where_c` versus on `fallback / UNSUPPORTED_FILTER` transition and
  counter behavior. This dispositions the observed reason-only classes at
  representative runtime boundaries, not every query in those corpus counts
  or the universal M3.5 gate. The report is
  `/tmp/sql-tap-flag-review.BVkQf6/report.json`; this is
  generated-mode inventory, not by itself CnP/LLVM corpus acceptance. A full
  CnP fixed-flag off/on/off run now has matching route counts and the same
  transition totals, with 47,946 memtx / 37,990 Vinyl queries, zero semantic
  or off-repeat diffs, and 24 / 23 EXPLAIN-row-only differences. It first
  exposed a CnP-only `FIELD_TYPE_NUMBER` assertion in the column offset-slot
  fast path; the fast path now applies `MEM_Number`, matching its typed and
  exact siblings. The previously failing ordinary CnP `boundary3.test.lua`
  passes on memtx and Vinyl, and the full CnP corpus retry passes. Its report
  is `/tmp/sql-tap-cnp-flag-review.24Sc0V/report.json` at source
  `562ee7a09d870bc704b0d789c31f789ab364e0bf`. LLVM corpus validation is
  recorded below; at this checkpoint M3.7 feature acceptance remained open
  pending M3.5's producer gate and the remaining flag-acceptance criteria.
  The LLVM-mode attempt originally exposed API/build blockers, not a parity
  result. The server now builds with Clang 19 / LLVM 19 / CnP enabled in a
  `/dev/shm` build directory, avoiding the full root filesystem. JIT calls and
  pointer loads now use the explicit-type LLVM C APIs; LLVM 13+ uses the new
  PassBuilder pipeline and links `LLVMPasses`, while older supported LLVM uses
  the legacy pass manager. External opcode declarations retain their actual
  calling convention: `SQL_PRESERVE_NONE` handlers use preserve-none, while
  ordinary handlers such as `ResultRow` retain the C ABI. The local
  `sql_stats_test` luatest passes with `SQL_JIT_ENABLE=1`, including compile,
  execution, and result-row coverage. Full LLVM SQL-TAP fixed-flag off/on/off
  parity now passes from committed source `23fa56eeffd996815255cc6de70902d42f9a8224`:
  47,946 memtx and 37,990 Vinyl queries, zero semantic diffs with LLVM on,
  zero off-repeat semantic diffs, and zero unreviewed route transitions.
  Raw hard diffs are limited to 24/23 EXPLAIN-row-only changes (on vs off)
  and 5/4 EXPLAIN-row-only changes (off repeat) for memtx/Vinyl respectively.
  The report is `/dev/shm/llvm-flag-review.0e507/report.json`; it is temporary
  validation output, not a committed corpus artifact. At that report's source
  M3.7 acceptance remained open pending M3.5's producer gate and the remaining
  feature-flag acceptance criteria. Follow-up fixes `3900b0c62a` and `aad0ae0440` came from
  a cross-version runtime probe: LLVM 16 does not define the PreserveNone
  calling convention, and forcing its numeric value produced a JIT that
  crashed during handler execution. For LLVM before 19, CMake now disables
  PreserveNone in both the server and handler bitcode, keeping JIT calls on
  the C ABI; the pre-19 LLVM + CnP combination is rejected because those
  features require incompatible handler ABIs. Clang 16 / LLVM 16 builds and
  passes the JIT-enabled `sql_stats_test`; Clang 19 / LLVM 16 also passes with
  the explicit cross-version C ABI; and Clang 19 / LLVM 19 rebuilds and passes
  with PreserveNone. The full LLVM corpus audit above remains specifically
  LLVM 19 evidence; no LLVM 16 full-corpus claim is made.

  **M3.7 current-source acceptance (2026-09-29).** The default-off,
  session-local flag now gates every executable new-planner route in the
  bounded production implementation. The 12-case `planner_flag_parity` suite
  passes on current Debug and explicitly verifies fresh-session default-off,
  independent-connection isolation, HASH scan opt-in, typed/composite key
  routes, and off/on/off row parity; `seq_scan_test` and the composite-order
  TAP regression also pass. Reviewed SQL, SQL-TAP, and SQL-luatest selections
  pass off/on/off semantic and off-repeat parity in generated, CnP, and LLVM
  modes with zero unreviewed route transitions. The explicit `iproto` observer
  and native direct-VALUES exclusions remain as documented under M3.5; both
  queries are included in generated-mode coverage. This closes the flag gate
  for the routes that exist today, not general physical candidate selection,
  general secondary-index execution, or the broader operator scope tracked by
  M3.4.
  *parallel: no*.

---

## E1 — Improved Bounded DP Baseline

**Goal:** before any DPhyp / LinDP++ bake-off, prove or disprove the
hypothesis that most plan-quality gain comes from statistics + properties,
not from a new enumerator. This is the cheap experiment that may save
quarters of bake-off work.

**State:** `PROTOTYPE`. E1.1–E1.5 have configurable bounded-DP controls,
per-statement metrics, and a reproducible full reviewed SQL-TAP width
comparison. The wider candidate is repeat-stable but differs in three
EXPLAIN outputs and costs about 2–2.6× aggregate planner time in these runs;
plan quality has not been measured. Offline A/B tooling now supports reviewed
SQL-luatest and normal SQL-suite subsets and reports EXPLAIN-output drift
separately from captured non-EXPLAIN result/diagnostic drift. Full reviewed
adapter-compatible SQL-luatest (32 memtx / 31 Vinyl tests; 499 / 447
statements) and SQL `.test.lua` scope (32 / 33 tests; 1,078 / 1,086 statements)
both passed all strict default/candidate and repeat comparisons with zero
diffs. The two reviewed raw `.test.sql` policy cases are now captured by the
strict SQL-file harness: full SQL coverage is 34 files / 1,090 statements on
memtx and 35 files / 1,098 statements on Vinyl, with exact repeat and
cross-width comparisons. Full reviewed SQL-suite and SQL-luatest captures in
CnP and LLVM also pass exact repeats and cross-width comparisons on both
engines. Full reviewed SQL-TAP captures in CnP and LLVM modes show the same
three cross-width EXPLAIN-only differences and exact within-width repeats;
the generated/CnP/LLVM SQL-TAP diffs are explicitly dispositioned as
compile-time EXPLAIN diagnostics, not executed-result evidence. This confirms
native-mode width sensitivity but does not measure query quality or execution
latency. E1 acceptance and the GATE decision still depend on integrated M3 +
S2 and an accepted evaluation workload with plan-quality and end-to-end latency
data. A standalone `test/sql-baselines/e1_measure.py` consumer and
`E1_WORKLOAD.md` contract now define stage-matched estimate/actual JSONL and
end-to-end execution timing, including zero-cardinality and provenance rules.
Its six unit tests pass; no integrated M3/S2 producer currently supplies these
observations, and the contract intentionally selects no acceptance thresholds.

**Exit criteria:**

- the current bounded DP solver runs with configurable budgets, property-
  aware dominance, and deterministic tie-breaking;
- harness counters (candidates generated / dominated / truncated / retained)
  emitted to M0 corpus;
- A/B comparison against fixed-1/5/10 widths on the corpus.

**Acceptance decision (2026-09-26):** retain the production defaults at
1/5/10. The 2/8/16 configuration remains experimental; the full reviewed
SQL-TAP comparison is evidence of repeatability and width sensitivity, not an
acceptance of wider defaults. Its three cross-width differences are confined
to explicit EXPLAIN captures and the SQL-TAP runs pass, but strict snapshot
parity is false. More importantly, summed planner time is 2.58× (memtx) and
2.52× (Vinyl) for the candidate, while the run measures neither end-to-end
query latency nor plan quality. The full-width SQL-TAP capture is not hosted
CI. Full CnP and LLVM SQL-TAP captures reproduce the same three EXPLAIN-only
cross-width differences and exact within-width repeats; candidate planner time
is roughly 2–2.6× default across modes and engines. Separate full reviewed
SQL-luatest and adapter-compatible SQL `.test.lua`
captures now have zero strict diffs; the two raw `.test.sql` cases also pass
strict SQL-file A/B capture after extending the harness to dispatch their
adapter. Full SQL-suite and SQL-luatest width A/B is now repeat-stable and
strictly identical in both CnP and LLVM modes on both engines. The SQL-TAP
differences have a documented disposition, but strict SQL-TAP cross-width
snapshot parity remains false. Of 24,263 memtx and 22,135 Vinyl
successful planner snapshots, only 6,314 and 6,237 respectively used
`current_where_c`; the rest were fallback or null path classes. This therefore
does not establish the value of wider bounds for an integrated M3/S2 planner.

Before E1 can be accepted or the defaults reconsidered, record all of the
following in a reviewed decision report:

- explain and disposition the three width-sensitive EXPLAIN captures
  (`select6/q96`, `where2/q128`, `whereK/q13`) without treating expected plan
  text differences as semantic regressions or silently excluding them;
- run candidate-vs-default parity over the accepted M0 SQL and SQL-luatest
  scope, on both engines and supported dispatchers, with semantic result
  parity reported separately from plan/diagnostic output;
- evaluate the integrated M3/S2 path on an agreed analytical workload, with
  executed-result correctness and a reviewed plan-quality measure (for
  example, cardinality q-error against actual rows), not EXPLAIN shape alone;
- measure end-to-end query latency as well as planner cost on that workload,
  with repeated runs and a documented acceptance threshold that justifies any
  wider-budget cost.

Until those gates pass and are reviewed, do not change the 1/5/10 defaults or
claim a production plan-quality improvement. The detailed capture, counts,
timings, and reproducible command are in `test/sql-baselines/PLANNER_AB.md`;
the raw run remains local at `/tmp/tarantool-e15-full-corpus-monotonic`.

**Subtasks:**

- [x] **E1.1** `wherePathSolver` beam widths are configurable at process start
  with `SQL_PATH_SOLVER_WIDTH_ONE`, `SQL_PATH_SOLVER_WIDTH_TWO`, and
  `SQL_PATH_SOLVER_WIDTH_MANY`. Defaults preserve `1/5/10`; invalid values
  fall back to defaults and valid values are bounded to `1..64`. Active values
  are exposed in `box.stat.sql()` and covered with a non-default luatest
  server. Time/memory budgets remain open. *parallel: no*.
- [x] **E1.2** Candidate paths are partitioned by relation subset, ORDER BY
  satisfaction, and reverse-scan mask. A global beam cap is retained; when
  full, admission prefers evicting the worst member of a duplicated partition
  before a singleton, falling back to the global worst when all partitions are
  singletons. This is diversity-aware bounded admission, not a guarantee that
  every partition survives. The 112-test SQL suite passes (6 disabled).
  *parallel: no* (same `where.c` solver as E1.1/E1.3).
- [x] **E1.3** Before beam admission, discard a path dominated within the
  same relation-subset/ORDER BY/reverse-scan partition across total cost,
  unsorted cost, and estimated rows. Remove every same-partition path dominated
  by a new candidate; retain incomparable candidates for future loop
  extensions. The global beam cap and E1.2 diversity eviction still apply.
  The 112-test SQL suite passes (6 disabled). *parallel: no*.
- [x] **E1.4** Snapshot envelope v4 (superseded by v5 route-ledger fields)
  and the optional harness manifest now include
  generated/dominated/truncated/retained bounded-path counts alongside legacy
  candidate, elapsed, and fallback counts. Matching process totals are
  available in `box.stat.sql()`. Metrics are diagnostic and non-gating.
  `generated` counts feasible extensions; `dominated` counts path states
  pruned; `truncated` counts candidate/victim paths lost at the beam boundary;
  `retained` sums the beam contents after each join-depth round. *parallel:
  yes* (instrumentation and harness schema are separable from E1.1-E1.3).
- [x] **E1.5 prototype** Reproducible offline A/B capture exists in
  `test/sql-baselines/planner_ab.py` and `PLANNER_AB.md`. On the reviewed
  join/WHERE subset (join, join2, join3, join5, where3), default 1/5/10 vs
  candidate 2/8/16 completed 687 statements per engine/run with zero snapshot
  drift in both repeats and across widths. A repeat after zero-source fallback
  classification at source commit `4d5ca37e` also had zero drift and
  repeat-stable planner structure, with
  294 successful planner snapshots per engine (failed SQL probes are now
  correctly excluded from planner metrics). Widths changed generated,
  retained, and truncated path totals; candidate/fallback counts and path
  classes remained unchanged. `elapsed_us` was zero, so no latency or plan-
  quality gain is established. A full-corpus generated-mode comparison against
  the accepted snapshot-capable anchor `d8fc1e339b` passes coverage equality
  and exact snapshot comparison on both engines. This is not an A/B comparison
  across width settings and does not measure plan quality.
  An expanded 962-query
  exploratory subset is repeat-stable but has one width-sensitive
  `EXPLAIN QUERY PLAN` output difference (`whereK/q13`) on both engines; it is
  not counted as a full snapshot parity pass, though both configurations pass
  the corresponding whereK SQL result assertions. A new reproducible
  `--full-corpus` mode completed default and candidate captures plus repeats
  across all reviewed SQL-TAP tests: 47,946 memtx and 37,990 Vinyl query
  snapshots per capture. Within-width repeats are exact and planner metrics
  are repeat-stable; strict cross-width snapshot parity is false with exactly
  three identical explicit EXPLAIN / EXPLAIN QUERY PLAN differences per
  engine (`select6/q96`, `where2/q128`, `whereK/q13`). All SQL-TAP runs passed;
  no accepted snapshots changed. The last two are plan/estimate diagnostic
  changes, and all three source statements are EXPLAIN captures. Aggregate
  planner metrics changed as widths changed. A rerun with the direct monotonic
  planner timer records nonzero summed `elapsed_us`: memtx 53,882 µs default
  versus 139,211 µs candidate (repeat 55,614 / 141,950 µs); Vinyl 55,871 µs
  versus 140,882 µs (repeat 52,376 / 149,423 µs). These are planner-time sums
  over successful SELECT/WITH snapshots, not end-to-end query latency or a
  plan-quality measurement. The comparison covers the reviewed SQL-TAP
  planner corpus, not the SQL suite. SQL-luatest reviewed-subset capture is
  supported through the normal child-server adapter; the full reviewed subset
  (32 memtx / 31 Vinyl tests, 499 / 447 statements) passed repeats and strict
  cross-width parity with zero diffs. SQL-vs-EXPLAIN classification is
  included; no differences occurred in the full run. Reviewed normal SQL
  `.test.lua` scope (32 / 33 tests, 1,078 / 1,086 statements) passed all
  repeats and cross-width comparisons exactly. The subsequent full SQL-suite
  run includes both reviewed `.test.sql` policy entries: 34 / 35 files and
  1,090 / 1,098 statements, with exact repeats and cross-width matches. The
  child-server adapter still requires Lua files; raw SQL files use the strict
  SQL-file harness. Full SQL-suite and SQL-luatest runs in CnP and LLVM also
  passed every within-width repeat and cross-width comparison, including the
  SQL-only files. All differences under SQL-TAP have been separately
  dispositioned; one represents a compile-only EXPLAIN listing and two are
  EXPLAIN QUERY PLAN diagnostics. No accepted workload has supplied plan
  quality or end-to-end latency evidence, so E1 is not accepted and defaults
  remain unchanged. The A/B harness now
  accepts `--mode generated|cnp|llvm` across SQL-TAP, SQL-luatest, and SQL-suite
  adapters; each mode is held constant through both width configurations and
  repeats and recorded in the report. This is not cross-mode parity evidence.
  Bounded CnP and LLVM runs on five reviewed join/range files, both engines,
  each captured 687 statements with zero strict diffs across default/candidate
  and repeats; generated paths increased from 949,450 to 1,512,106, showing a
  width effect. Full reviewed SQL-TAP runs in both native modes now complete:
  each mode has exact within-width repeats and the same three explicit EXPLAIN
  cross-width differences per engine as generated mode. SQL assertions pass,
  but strict snapshot parity is false; planner-time sums rise roughly 2–2.6×,
  with no query-latency or plan-quality result. Native-mode evidence therefore
  does not accept E1 or justify changing defaults. Exact diffs and reproduction
  instructions are in `test/sql-baselines/PLANNER_AB.md`. The capture/evaluation prototype
  is complete; strict SQL-TAP cross-width parity and
  full-width plan-quality evaluation remain open. The separate E1 workload
  analyzer now rejects paired observations whose actual row counts differ at
  any matching stage, even when query/repeat/stage IDs and provenance match;
  this guards q-error and timing comparisons against changed executed data.
  It also enforces the workload contract's minimum five non-warmup repeats per
  query/configuration. Its focused eight-test suite passes. This strengthens
  analysis validation, but does not provide the still-missing integrated
  measurement producer.
  *parallel: yes*.

---

## GATE — Enumerator Bake-off Decision

**State:** `NOT-STARTED`. Depends on E1.

**Decision input:**

- E1 plan-quality measurements vs fixed-width solver;
- S2 statistics-improvement measurements;
- analytic workload corpus results.

**Possible outcomes:**

1. **Stop.** E1 + S2 closed the analytics gap. Retain bounded DP, declare
   the analytics initiative successful for the current scope.
2. **Continue with DPhyp prototype only.** E1 + S2 helped but a plan-quality
   gap remains on dense / star / snowflake join graphs.
3. **Continue with full bake-off** (DPhyp / LinDP++ / deterministic
   greedy/beam). E1 + S2 helped but the residual gap is unclear in shape.

This gate is intentionally a stopping point. The honest expectation given
Tarantool's current join graph shapes (mostly OLTP-style, low relation
count) is **outcome 1**. The work beyond this point should not be planned
until the gate evaluation runs.

---

## Cross-cutting concerns

### CnP and LLVM MCJIT

Both JIT backends remain in their current state, consuming the VDBE bytecode
that the new planner lowers. **No planner work in this roadmap touches the
JIT path.** Cross-platform expansion of CnP to arm64 happens on a different
machine and is not tracked here.

The dispatcher-parity CI job (M0.6) is the only roadmap item that interacts
with the JITs, and it does so as a black-box correctness check — not as a
modification of either JIT.

### `where.c` retirement

This roadmap does **not** schedule `where.c` removal. Retirement is gated on
all supported query classes showing zero fallback for two consecutive release
cycles, plus a separate written decision. Until that decision, both planners
coexist via the M3.5 fallback path.

### System-space allocation

`_sql_stats_relation`, `_sql_stats_index`, `_sql_stats_column` are
permanent on-disk schema changes. Each requires human review and explicit
allocation of system-space IDs before the corresponding subtask
(`S1.1`, `S2.1`) starts implementation. These are the only one-way doors
in the roadmap.

---

## GitHub epic structure

When converted, this roadmap should produce:

- one top-level epic issue: *"Tarantool SQL Analytics Engine — Statistics +
  Planner Migration"*;
- eight sub-issues, one per milestone (M0, S0, M1, S1, S2, M3, E1, GATE);
- one task issue per subtask checkbox in this file;
- a status label on each sub-issue matching the status legend above.

The Mermaid dependency graph above can be embedded directly in the epic body.
No calendar dates are assigned while architectural approval and integration
gates remain open; use the dependency edges to sequence work.
