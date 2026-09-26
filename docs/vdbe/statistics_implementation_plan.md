# SQL Statistics Implementation Plan

## Purpose

This document defines the design of the statistics subsystem: the
planner-facing contract, persistence layout, collection job, summary
algorithms, refresh policy, budgets, and engine interface.

For **scheduling, status tracking, and worktree parallelism**, see
[`roadmap.md`](roadmap.md) milestones S0, S1, S2. This document is the
design companion; the roadmap is the schedule.

The statistics subsystem is a separate infrastructure project. The optimizer
consumes normalized, immutable snapshots and does not interpret collection
sketches directly.

## Current State

The current tree has historical/scaffolding elements but no active end-to-end
statistics path:

- `ANALYZE`-oriented SQL TAP tests exist but are disabled;
- current SQL rejects `ANALYZE`;
- neither `_sql_stat1` nor `_sql_stat4` exists in a fresh instance;
- `_sql_stat1`/stat4-oriented historical tests and comments remain;
- stat4-oriented comments/tests remain, but no active `_sql_stat4` source
  implementation exists;
- `OP_LoadAnalysis` is a no-op in both the original and extracted handlers;
- `index_field_tuple_est()` uses `default_tuple_est[]`;
- table cardinality can use current primary-index size, with the cardinality
  semantics caveats described in `next_gen_sql_planner.md`.

S0 completed that audit in [`s0_audit_report.md`](s0_audit_report.md).
The implementation must add active grammar, collection, persistence,
loading, and planner consumption.

## Planner-Facing Contract

The intended planner contract is an immutable `SqlStatsSnapshot` built once
per prepare. The in-memory API prototype is implemented in
`src/box/sql/sql_stats_snapshot.{h,c}`; construction deep-copies and sorts
relation/index data, enforces a byte budget, and uses a reference count for
lifetime management. It stores catalog/schema versions and gives a stale
lookup result when the current schema version differs. The current prototype
is not wired to prepare or the planner yet:

```c
struct SqlStatsSnapshot {
	uint64_t catalog_version;
	uint64_t schema_version;
	uint32_t relation_count;
	uint32_t total_bytes;
	struct SqlRelationStats *relations;
};

struct SqlRelationStats {
	uint32_t space_id;
	double rows;
	double rows_confidence;
	double average_row_width;
	enum SqlCardinalitySemantics cardinality_semantics;
	uint64_t collected_at;
	uint64_t modification_epoch;
	struct SqlColumnStats *columns;
	struct SqlIndexStats *indexes;
	struct SqlMultiColumnStats *groups;
};
```

The intended snapshot is compact, immutable, reference-counted at
prepared-statement lifetime, and bounded by configuration. Planner inner
loops only read it. The prototype API has not yet been wired into prepare or
planner execution.

The S1.4 API prototype currently records `confidence`, named cardinality
semantics, `collected_at`, and `modification_epoch` per relation. It compares
the caller's current schema version at lookup and returns `STALE` on mismatch.
It intentionally does not define age thresholds, confidence decay, refresh
scheduling, or persistent provenance; those policies remain part of S1/S2
integration and the human-reviewed schema.

### Parallel contract boundary

Freeze a small, versioned read-only `SqlStatsSnapshot` API before multiple
statistics tasks integrate. It must define ownership/lifetime, missing and
stale values, cardinality semantics for memtx and Vinyl, confidence, and the
fallback to current estimates. Unit tests may supply a fixed snapshot;
neither M1 nor early M3 needs persisted statistics to start.

S1 can be split into sampler adapters, snapshot reader, collection job, and
schema proposal. The persistent system-space format and IDs are reviewed
before writes; a single integrator joins those pieces and then wires the
`where.c` adapter. S2 sketch algorithms and synthetic distributions can run
against the frozen snapshot API in parallel with S1 persistence, but S2's
payload format and selectivity integration wait for the S1 generation and
staleness rules. This ordering prevents the storage schema and planner
adapter from acquiring competing owners.

The current adapter prototype installs an optional immutable snapshot on the
SQL core and lets legacy index cardinality estimates read relation row counts
and average rows per captured index prefix (index tuple count divided by the
matching prefix NDV, not relation count). This keeps sparse-index prefix
estimates tied to the index population. Missing or schema-stale entries retain
the existing fallback estimates. This is a reader-side seam only: no collector
currently populates the provider, prepared statements do not retain their own
snapshot generation, and no persisted IDs or formats are selected.

## Persistence

Use versioned system spaces rather than extending the opaque `_sql_stat1`
string format:

| Space | Purpose |
| --- | --- |
| `_sql_stats_relation` | cardinality, width, collection metadata |
| `_sql_stats_column` | null fraction, NDV, width, MCV, histogram |
| `_sql_stats_index` | prefix NDV, ordering/correlation, physical summary |
| `_sql_stats_multicolumn` | selected groups, joint NDV, MCV, dependencies |

Large payloads such as MCV/histogram arrays are stored as versioned MsgPack
objects with explicit format versions. Rows are replaced transactionally per
relation collection generation. Readers either see the previous complete
generation or the new complete generation.

Do not make the new planner depend on `_sql_stat1` text parsing. A compatibility
importer may preserve useful old data if the audit finds any.

## Collection

### Initial collection policy

`ANALYZE [table]` performs an explicit bounded collection job:

1. obtain a stable read view;
2. collect exact cheap metadata and index definitions;
3. sample tuples using an engine-provided sampler;
4. decode only selected fields into a column-oriented collection buffer;
5. build normalized summaries;
6. persist one new generation transactionally;
7. publish a new statistics catalog version.

### Sampling

Initial target:

- configurable maximum sampled rows and bytes per relation;
- reservoir/systematic sampling for memtx;
- Vinyl-provided sampling that avoids forcing pathological full LSM reads;
- full scan only for small relations below a configurable threshold;
- deterministic seed option for tests/replay.

MsgPack field extraction is performed once per sampled tuple into a temporary
column-oriented buffer. This avoids rescanning each tuple independently for
every statistic. Selected multi-column groups reuse the decoded columns.

### Multi-column group selection

Collect initially for:

- all primary/unique key prefixes;
- selected secondary-index prefixes;
- explicitly configured column groups.

Workload-driven groups are later work and require bounded telemetry.

## Summary Algorithms

Initial production candidate:

- exact/sample row count and average width;
- null fraction;
- NDV using exact set below a threshold, then HLL;
- bounded MCV using SpaceSaving plus exact verification on the sample;
- equi-depth histogram from sampled ordered values;
- joint NDV for selected groups;
- exact key-derived functional dependencies where SQL NULL semantics permit;
- sampled dependency strength for explicitly selected groups.

Correlation coefficients and more advanced sketches are added only when a
targeted estimation corpus demonstrates value.

### HyperLogLog prototype contract (S2.2)

The in-memory prototype lives at `src/box/sql/sql_stats_hll.{h,c}`. It accepts
caller-encoded byte strings and does not define SQL value encoding: callers
must provide canonical value bytes. `sql_stats_hll_add_tuple()` adds a
composite-key path that encodes arity, one-byte caller type tags, 64-bit
little-endian field lengths, and field bytes before hashing; this prevents
ambiguous field boundaries and type-tag collisions while leaving collation
and SQL canonicalization with the caller. Both add paths use a 64-bit
endian-independent seeded hash and are deterministic. Seed
and precision are sketch identity; merge is register-wise maximum and rejects
either mismatch. Composite sketches can be merged only when callers use the
same type-tag and canonical-value conventions for every field. The API is
intentionally opaque and has no persistent encoding, snapshot ABI, or
system-space dependency.

Precision is supported from 4 through 18. The register array consumes
`2^precision` bytes, excluding allocator overhead. The estimator uses the
standard HLL harmonic estimate and linear-counting correction for small
cardinalities. Its asymptotic relative standard error is approximately
`1.04 / sqrt(2^precision)` (about 1.63% at precision 12); this is a statistical
expectation, not an individual-result guarantee. Integration must preserve
the seed/precision metadata and report confidence independently.

### SpaceSaving prototype contract (S2.3)

The in-memory bounded MCV candidate sketch is exposed by
`src/box/sql/sql_stats_spacesaving.{h,c}`. Capacity is fixed at construction;
each key is an opaque caller-encoded byte string, so SQL callers must encode
type and NULL distinctions unambiguously. A tracked value returns an upper
frequency estimate and an error; its sample frequency is in
`[estimate - error, estimate]`. For a single stream of `N` updates, the
SpaceSaving error for every tracked value is at most `ceil(N / capacity)`.
Keys use bytewise lexical order (shorter prefix first) to resolve equal-count
victim and output-selection ties, making results deterministic.

Merge keeps the destination capacity. For a value tracked on both sides,
upper estimates and errors are added. If it is absent on one side, that side's
minimum counter is added to both estimate and error, conservatively covering
the frequency that may have been omitted there. The bound for a tracked result
is therefore no worse than the sum of the per-input SpaceSaving bounds; merge
is not an exact union and may omit candidates. Merge is atomic on allocation
or overflow failure. The API defines no SQL value encoding, persistence
format, confidence metadata, or system-space ID; those remain integration and
schema-review work.

### Equi-depth histogram prototype contract (S2.4)

The in-memory builder is `src/box/sql/sql_stats_histogram.{h,c}`. Its caller
supplies an already sorted sample of opaque byte strings and a comparator
implementing the SQL type's total ordering, including collation and NULL
rules. This keeps type encoding and comparison policy outside the generic
summary algorithm. The builder selects cumulative quantile boundaries and
deep-copies them under a caller-provided byte budget. It never splits equal
values: when a target quantile falls inside a duplicate run, the boundary
advances to the end of that run, and repeated boundaries collapse. Therefore
the result can contain fewer buckets than requested. Each boundary reports
its cumulative sample count; the final bucket covers the full sample. The API
is opaque and in-memory only, with no persistence encoding or system-space
dependency. Interpolation and conversion of these sample counts into planner
selectivity remain separate estimator work (S2.5).

### Selectivity estimator prototype contract (S2.5)

`src/box/sql/sql_stats_selectivity.{h,c}` consumes normalized single-column
inputs without owning collection or persistence. A proven unique equality and
`IS NULL` fraction precede sampled MCV equality; untracked equality values use
residual NDV with deliberately reduced confidence. Range predicates use
cumulative histogram counts, with midpoint estimates inside a bucket and
strict/inclusive behavior at exact boundaries. Independent conjunctions
multiply selectivities and retain the minimum confidence. This is a narrow
prototype only: beyond that exact-tuple case it does not model dependencies,
general correlation, MCV-aware range mass, or planner fallbacks, and is not
connected to `where.c` or `SqlStatsSnapshot`. A narrow joint-MCV prototype
handles only a fully specified conjunction of non-NULL equality predicates:
when the exact tuple is present it uses that sampled joint frequency; when it
is absent, it falls back to the existing per-column independence product.
The general predicate-conjunction entry point combines repeated constraints
on a column using its caller-supplied SQL comparator. Equivalent equalities
are estimated once; distinct equalities, disjoint bounds, and strict/inclusive
point intervals that exclude their endpoint produce exact zero. Equality
combined with bounds is estimated as that equality if it satisfies every
bound. Repeated bounds in one direction reduce to the strongest bound.
Multiple distinct lower/upper bounds do not use an independence product.
When their intersection is a non-point interval, only an exhaustive joint
sample can estimate it; absent such a sample, the API rejects the input
rather than subtracting independently interpolated histogram CDF estimates.
With one combined predicate per column, mixed constraints remain exact only
when distinct joint tuples exhaust the complete non-NULL sample for all
summarized columns; partial joint MCV samples use the per-column independence
fallback. A predicate constraining only a subset of joint dimensions is exact
when that sample is exhaustive, by summing all matching tuple counts. For a
truncated joint MCV list the sum is only a lower bound; the scalar full-group
joint-NDV estimate cannot infer the missing marginal tail, so independence is
preserved instead of extrapolating from MCV rows. Joint-NDV-aware
partial-tuple estimation, dependency statistics, and policy for
choosing/storing multicolumn groups remain open.

The focused unit probes include a uniform 1,000-distinct-value column, a
skewed column with one value at 90% frequency, correlated/anti-correlated
conjunctions, and a synthetic stale-MCV fixture where a distribution shift
worsens q-error while the caller supplies lower confidence. Equality and
selected range predicates are compared against known selectivities using
q-error. A second drift fixture models an 80%-to-1% change in a joint pair
frequency: the current sample has q-error 1 and the stale sample q-error 80,
with lower confidence supplied by the caller. These are algorithm sanity
checks, not the M0 corpus gate: the estimator has no freshness/decay policy,
representative stale-stat corpus variants and integrated plan-quality
measurement remain open.

## Refresh And Staleness

Initial policy is explicit `ANALYZE`; background refresh is not required for
the first production candidate.

Each relation statistic records:

- collection time and catalog generation;
- sampled rows/bytes and configured limits;
- modification epoch/count since collection where available;
- confidence/error metadata.

The planner degrades confidence as statistics become stale. It does not
synchronously recollect during prepare.

## Memory And Time Budgets

Configuration must bound:

- sampled rows and bytes per relation;
- collection arena bytes;
- persisted bytes per relation and database;
- snapshot bytes per prepared statement;
- MCV entries, histogram buckets, and multicolumn groups;
- collection elapsed time.

When a budget is exceeded, collection emits a lower-quality summary with
explicit confidence rather than failing ordinary query preparation.

## Engine Interface

Storage engines provide:

```c
struct SqlStatsSampleRequest {
	uint64_t max_rows;
	uint64_t max_bytes;
	uint64_t seed;
	uint32_t *field_ids;
};

int
engine_sql_stats_sample(struct space *space,
			 const struct SqlStatsSampleRequest *request,
			 struct SqlStatsSampleSink *sink);
```

The engine controls safe/efficient tuple sampling. The common SQL statistics
layer owns MsgPack decoding, summary construction, persistence, and normalized
planner snapshots.

## Milestones

The active milestones are S0, S1, S2. They map directly onto the same-named
sections in [`roadmap.md`](roadmap.md), which owns scheduling, dependencies,
status, and worktree-parallelism markings. This document owns the design
content for each.

| Milestone | Scope | Status (see roadmap) |
| --- | --- | --- |
| S0 audit | inventory disabled/scaffold code and current estimates; documented reuse/delete decisions | COMPLETE (report at `docs/vdbe/s0_audit_report.md`) |
| S1 relation/index basics | cardinality semantics, width, index-prefix facts; versioned persistence; snapshot API; budgets; current-planner compatibility adapter into `where.c` | PROTOTYPE (snapshot API only) |
| S2 columns | null fraction, NDV (HLL), MCV (SpaceSaving), histograms; memtx/Vinyl sampling; stale/confidence policy; selectivity estimator | NOT-STARTED |

S1 absorbs the work originally drafted as a separate S4
("current-planner integration") — it is the same compatibility adapter into
`where.c`, scheduled together with the rest of the relation/index work so
the S1 exit gate is measurable end-to-end.

**Deferred until after roadmap GATE:**

| Future milestone | Scope | Trigger |
| --- | --- | --- |
| S3 selected multicolumn | key-derived dependencies, joint NDV, bounded configured groups | corpus shows residual q-error driven by multi-column correlation after S2 |

S3 is intentionally deferred. The roadmap's E1 + GATE evaluation may show
that S2 column statistics close the analytics gap; if so, S3 never becomes
necessary. If GATE shows a residual gap traceable to correlated predicates,
S3 becomes its own scheduled milestone with a fresh design pass.

## Validation

Required corpora:

- synthetic uniform, skewed, correlated, and anti-correlated data;
- stale and missing statistics;
- memtx and Vinyl;
- point, range, multi-predicate, and join selectivity;
- planner q-error and selected-plan regression;
- collection CPU, elapsed time, memory, and persisted bytes.

Statistics is successful only when current-planner estimates and end-to-end
plans improve under bounded collection/preparation cost.
