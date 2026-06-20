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

The first milestone is an audit: identify reusable scaffolding and obsolete
SQLite-derived assumptions. The implementation must add active grammar,
collection, persistence, loading, and planner consumption.

## Planner-Facing Contract

The planner reads an immutable `SqlStatsSnapshot` built once per prepare:

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

The snapshot is compact, immutable, reference-counted at prepared-statement
lifetime, and bounded by configuration. Planner inner loops only read it.

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
| S0 audit | inventory disabled/scaffold code and current estimates; documented reuse/delete decisions | NOT-STARTED |
| S1 relation/index basics | cardinality semantics, width, index-prefix facts; versioned persistence; snapshot API; budgets; current-planner compatibility adapter into `where.c` | NOT-STARTED |
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
