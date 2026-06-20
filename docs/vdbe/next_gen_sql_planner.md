# Next-Generation SQL Planner

## Purpose

This note reviews the current SQL planner/optimizer architecture in Tarantool,
compares it with several established alternatives, and proposes a staged path
toward a stronger planner that still fits Tarantool's execution model and
storage engines.

The goal is not to copy PostgreSQL or ORCA wholesale. The goal is to identify
which ideas are worth borrowing, in which order, and where Tarantool's single
node VDBE executor, `memtx`, and `vinyl` require a different design.

For the concrete migration order from current `where.c`/VDBE code generation
to PostgreSQL-like planner operators, see
[`planner_vm_migration.md`](planner_vm_migration.md).

For the current SQL/VDBE semantic baseline that bounds parity work, see
[`current_sql_feature_matrix.md`](current_sql_feature_matrix.md).

For the statistics collection, persistence, budget, and rollout design, see
[`statistics_implementation_plan.md`](statistics_implementation_plan.md).

A push-based execution engine alternative was considered and rejected for the
transactional product. VDBE remains the only executor; CnP and LLVM MCJIT
remain its JIT backends.

## Sources

Primary sources consulted for this note:

- Tarantool current planner code:
  - `src/box/sql/where.c`
  - `src/box/sql/whereInt.h`
  - `src/box/sql/wherecode.c`
  - `src/box/sql/whereexpr.c`
  - `src/box/sql/select.c`
  - `src/box/sql.c`
- PostgreSQL planner/optimizer documentation:
  - https://www.postgresql.org/docs/17/planner-optimizer.html
  - https://www.postgresql.org/docs/13/geqo-pg-intro.html
- DPhyp:
  - Guido Moerkotte, Thomas Neumann, "Dynamic Programming Strikes Back",
    SIGMOD 2008
  - accessible PDF used here:
    https://15721.courses.cs.cmu.edu/spring2019/papers/23-optimizer2/p539-moerkotte.pdf
- LinDP++:
  - Bernhard Radke, Thomas Neumann,
    "LinDP++: Generalizing linearized DP to crossproducts and non-inner joins",
    BTW 2019
  - summary page used here:
    https://portal.fis.tum.de/en/publications/lindp-generalizing-linearized-dp-to-crossproducts-and-non-inner-j/
- ORCA / GPORCA:
  - Mohamed A. Soliman et al.,
    "Orca: A Modular Query Optimizer Architecture for Big Data", SIGMOD 2014
  - accessible PDF used here:
    https://15799.courses.cs.cmu.edu/spring2025/papers/21-orca/p337-soliman.pdf
  - GPORCA repository README:
    https://github.com/greenplum-db/gporca-archive
- Volcano / memo background:
  - Apache Calcite VolcanoPlanner overview:
    https://calcite.apache.org/javadocAggregate/org/apache/calcite/plan/volcano/package-summary.html

## Executive Summary

The current Tarantool SQL planner is much closer to SQLite's `where.c`
architecture than to PostgreSQL or ORCA:

- it combines access-path enumeration, join ordering, and VDBE-oriented physical
  planning into one tightly-coupled subsystem;
- it performs a bounded dynamic-programming search over `WhereLoop` candidates;
- it only optimizes nested-loop style join execution, because the executor has
  no hash join or merge join nodes;
- it folds ORDER BY satisfaction into join search directly, rather than using a
  general physical-property framework;
- and, most importantly, it still lacks a real statistics subsystem.

Today the biggest structural weakness is not the search algorithm alone. It is
the combination of:

- weak statistics,
- planner/codegen coupling,
- and the absence of a clean optimizer IR above VDBE.

Therefore the recommended direction is:

1. make current planning observable and replayable;
2. define a normalized statistics contract and build its collection and
   persistence as a separate infrastructure project;
3. separate logical planning from VDBE lowering, with a minimal property model
   and lightweight subset/property memo;
4. benchmark improved bounded DP, DPhyp, and simpler heuristic search before
   selecting a replacement join enumerator;
5. add richer physical operators only when measurements justify them;
6. evolve toward ORCA/Cascades ideas incrementally rather than treating a full
   memo optimizer as either the immediate answer or an irrelevant future.

This ordering is intended to produce measurable improvements without committing
to a search algorithm before Tarantool has the statistics, properties, replay
tools, and representative workloads needed to evaluate it.

The critical compatibility idea is that PostgreSQL-like operators should first
exist as planner objects, not runtime VM operators. A selected physical plan can
be lowered into existing VDBE opcodes. New VDBE opcodes or runtime executor
nodes should appear only after the planner has physical algorithms, such as
hash join or hash aggregate, that current bytecode cannot express efficiently.

## Current Tarantool Planner Architecture

### What exists now

The current planner core lives in `src/box/sql/where.c` and related files.

Its shape is:

- `WhereTerm`: extracted WHERE predicates and derived virtual predicates.
- `WhereLoop`: one candidate access algorithm for one FROM item.
- `WherePath`: one partial join order made from `WhereLoop` candidates.
- `WhereLevel`: the chosen implementation for each nested loop level.

The search algorithm in `wherePathSolver()` is a bounded bottom-up dynamic
program over partial paths:

- for one table, it keeps only the single best path;
- for two-way joins, it keeps 5 best paths;
- for 3+ joins, it keeps 10 best paths.

This is already a pragmatic DP/beam hybrid, not exhaustive Selinger-style DP.

The planner also directly reasons about:

- ORDER BY / GROUP BY satisfaction via `wherePathSatisfiesOrderBy()`;
- skip-scan;
- auto-index creation;
- covering vs non-covering index penalties;
- LEFT/CROSS join legality;
- and some engine-specific heuristics.

### Strong points

- Small and simple.
- Low planning latency.
- Good fit for nested-loop-only execution.
- Tight integration with index order and sorter avoidance.
- Easy to keep deterministic.

### Structural limitations

#### 1. Planning and code generation are too entangled

The planner is still effectively a VDBE-oriented loop builder. There is no
clean optimizer IR that represents:

- logical relations,
- join predicates as a graph/hypergraph,
- candidate physical properties,
- or engine-specific access methods behind a stable interface.

This makes major optimizer changes expensive because every improvement has to be
threaded through VDBE-centric structures immediately.

#### 2. Statistics are extremely weak

This is the most important current limitation.

Observed facts in the current code:

- `OP_LoadAnalysis` is still effectively a no-op.
- `index_field_tuple_est()` in `src/box/sql.c` returns
  hardcoded `default_tuple_est[]` values.
- `whereRangeScanEst()` still uses fallback heuristics such as:
  - one range bound -> roughly `1/4`
  - closed range -> roughly `1/64`
- comments in `where.c` still refer to `_sql_stat4`, but the real stat4-like
  machinery is not present in the current planner path.

This means better join enumeration alone cannot reliably produce better plans,
because cardinality estimates are still too coarse.

#### 3. Only one join execution family exists

Unlike PostgreSQL, Tarantool does not currently expose a full physical join
space with:

- nested loop,
- hash join,
- merge join,
- bitmap scans,
- materialize,
- and general property enforcers.

The current planner is primarily choosing:

- scan/index access,
- nested loop order,
- and sort avoidance.

That matters when evaluating whether ORCA or PostgreSQL-like path enumeration is
worth the complexity.

#### 4. ORDER BY handling is specialized, not general

`wherePathSatisfiesOrderBy()` is effective, but it is a specialized mechanism.
There is no general physical property system equivalent to PostgreSQL pathkeys
or Cascades traits/enforcers.

#### 5. Join search is bounded by heuristics instead of query structure

The current solver caps the number of kept paths by join count, not by graph
shape, predicate connectivity, or search budget. This is cheap, but it is also
blunt:

- sparse connected join graphs are not exploited as aggressively as DPhyp could;
- large but structured joins have no better fallback than bounded path pruning;
- and non-inner join legality is handled ad hoc rather than by a dedicated join
  graph/constraint layer.

### Current engine-specific assumptions

The current cost model already leaks engine assumptions:

- full-scan and index penalties are biased toward Tarantool's primary-key and
  tuple layout behavior;
- comments in `where.c` explicitly say that some current tuple-size logic does
  not make sense for Vinyl secondary indexes;
- `SELECT count(*)` already has a memtx-specific fast path in `select.c`.

This is a sign that any next-generation optimizer must have an explicit engine
cost interface rather than burying engine assumptions inside `where.c`.

## What PostgreSQL Does Well

From the official PostgreSQL planner docs:

- for smaller join problems it performs a near-exhaustive join-order search;
- it considers nested loop, merge, and hash joins;
- it prefers joining relations that have join clauses;
- when join count exceeds `geqo_threshold`, it switches to GEQO, a genetic
  heuristic search over join sequences.

The important architectural ideas to borrow are not GEQO first. They are:

### 1. Separate logical and physical planning objects

PostgreSQL uses structures like `RelOptInfo` and `Path` to separate:

- base-relation properties,
- join-relation properties,
- and alternative physical access/join implementations.

This is better than tying everything directly to loop code generation.

### 2. Build alternatives before choosing one

PostgreSQL builds multiple access paths per relation/join and only then picks
the cheapest path. Tarantool partially does this via `WhereLoop`, but only in
the narrow nested-loop/access-path sense.

### 3. Represent ordering as a property

PostgreSQL's pathkeys are a cleaner abstraction than current
`nOBSat/revMask/isOrdered` plumbing.

### 4. Switch search strategy by join count

The GEQO split is pragmatic:

- exact/nearly exact when small,
- heuristic when large.

That general idea applies to Tarantool too, but GEQO itself is not the best
first answer here.

### What should not be copied literally

- PostgreSQL's current exact join search is still tightly tied to the broader
  `RelOptInfo/Path` framework and a richer physical operator space.
- GEQO is useful, but Tarantool should compare its current bounded search,
  DPhyp, and deterministic heuristics on representative workloads before
  selecting an intermediate algorithm.
- PostgreSQL is disk-oriented enough that some of its cost constants and path
  tradeoffs are not appropriate for `memtx`.

## DPhyp

### What it is

DPhyp is a dynamic-programming join enumeration algorithm over a hypergraph.
It enumerates connected-subgraph / connected-complement pairs rather than
blindly trying relation subsets.

Key points from "Dynamic Programming Strikes Back":

- it improves over older DP approaches that spend too much time testing
  impossible or disconnected combinations;
- it models complex join predicates as hyperedges;
- it enumerates only connected subgraphs and connected complements;
- it avoids cross products unless explicitly represented in the graph;
- it generalizes well to non-inner join legality by adding derived hyperedges.

### Why it is attractive for Tarantool

This is the strongest exact-search candidate for Tarantool's next step because:

- it targets join order, which is the weakest structural part of current
  planning once statistics are fixed;
- it fits a single-node optimizer well;
- it does not require a full memo/rule engine;
- it works naturally with predicate connectivity;
- and it scales much better than naive exhaustive bushy DP on connected joins.

### Limits

- DPhyp is a join enumerator, not a full optimizer architecture.
- DPhyp still has exponential worst-case behavior.
- Building correct hyperedges and legality constraints is a substantial part of
  the work, especially for outer, semi, anti, and lateral joins.
- It still needs good cardinality estimates.
- It does not give Tarantool a memo, rules, or a general property system.
- It is best for exact optimization of relatively small and medium join graphs,
  not as the only strategy for very large joins.

### Decision status

DPhyp is a candidate, not the selected implementation. A prototype must be
compared against an improved current bounded DP and a simple deterministic
greedy/beam strategy on chain, star, snowflake, clique, disconnected, and
non-inner legality graphs. The selected algorithm may differ by graph shape and
planning budget.

## LinDP and LinDP++

### What they are

Linearized DP uses a good left-deep plan to linearize the search space for a
subsequent DP phase. The point is to get plan quality close to exact DP while
stretching to much larger join counts.

LinDP++ extends the idea to:

- crossproducts when useful,
- and non-inner joins.

From the LinDP++ paper summary:

- graph-based DP is very good for connected inner joins up to a few dozen
  relations;
- linearized DP works well up to around a hundred relations or more;
- plain linearized DP is weak on crossproducts and non-inner joins;
- LinDP++ generalizes it to arbitrary queries by augmenting the query graph.

### Why it matters for Tarantool

LinDP++ is a credible medium/large-query research candidate. It must not be
committed as the fallback until a prototype demonstrates better plan quality
per unit of planning time than a simpler deterministic beam/greedy fallback.

### Limits

- It still depends on reasonable statistics.
- It is more complex than current bounded DP.
- It is still only join-order search, not full ORCA-style optimization.

## ORCA / GPORCA

### What it is

ORCA is a Cascades-style optimizer with:

- a memo of equivalent expressions;
- transformation rules for logical exploration;
- implementation rules that produce physical operators;
- property enforcement;
- metadata/statistics caching;
- and branch-and-bound style search scheduling.

From the ORCA paper and GPORCA README:

- the Memo stores groups of equivalent expressions compactly;
- optimization runs in phases such as exploration, implementation, and
  optimization;
- required physical properties are enforced by enforcers, e.g. Sort;
- metadata is cached on the optimizer side;
- the architecture is intentionally modular and portable.

### What is attractive

ORCA solves problems that Tarantool does not currently solve well:

- broad logical rewriting;
- multiple physical implementations per logical operator;
- property-driven optimization;
- memoized reuse of equivalent subproblems;
- clean metadata boundary;
- and long-term extensibility.

### Why ORCA is not the first step

For Tarantool today, ORCA is probably too large as an initial replacement.

Reasons:

1. The current executor does not yet expose enough physical diversity.
   Without hash join, merge join, bitmap paths, distribution, exchange, etc.,
   a full memo/rule engine has lower immediate leverage.

2. The statistics layer is still too weak.
   ORCA depends on much richer metadata and statistics than Tarantool currently
   has.

3. Planning latency matters.
   Tarantool is often used in latency-sensitive transactional deployments where
   planning overhead is visible, especially for one-shot statements.

4. The implementation cost is high.
   Building a stable memo optimizer is a multi-phase effort, not a planner
   patch.

### What should be borrowed now

Borrow concepts, not the whole system:

- optimizer minidumps and deterministic replay;
- a lightweight subset/property memo;
- memo-friendly IR boundaries;
- explicit properties;
- engine-abstracted metadata/statistics APIs;
- and separate logical exploration from physical implementation.

## Applicability To memtx

`memtx` is in-memory, latency-sensitive, and CPU/cache dominated.

Implications:

### Good fits

- fast exact join search on small/medium joins
- precise point/range costing
- aggressive exploitation of ordered indexes
- parameter-sensitive planning decisions
- low-overhead planning with strong prepared-statement reuse

### Less urgent

- a huge distribution-property framework
- deep I/O-oriented costing dimensions
- overly expensive optimizer phases for one-shot OLTP queries

### What should be different from PostgreSQL

- planning budget must be tighter;
- cheap physical index counts can be used more aggressively, but the API must
  distinguish physical entries, committed rows, and snapshot-visible estimates;
- nested loop with index lookup remains a strong default;
- a hash join implementation would help analytics, but it is not a prerequisite
  for improving join order search first.

## Applicability To vinyl

`vinyl` changes the optimization problem materially.

It is not enough to reuse `memtx` costing with a few constants changed.

### Why Vinyl is different

- LSM/tree layout means point and range reads have read amplification;
- bloom filters, ranges, runs, pages, and compaction state affect cost;
- covering vs non-covering access behaves differently than in `memtx`;
- tuple counts and tuple sizes are not enough to cost secondary-index access;
- physical cost variance is higher.

### What Vinyl needs from the planner

The enumerator must not inspect live Vinyl structures in its inner loop.
Before optimization, the engine produces an immutable compact planning
snapshot. A pure cost function consumes that snapshot and a normalized access
request containing:

- point, equality-prefix, range, or full-scan shape;
- expected qualifying rows and repeated probe count;
- covering/non-covering status and projected width;
- order requirement and limit/top-N demand.

At minimum, the snapshot and cost callback should estimate:

- expected number of ranges/slices touched;
- expected point-probe amplification;
- bloom false-positive cost;
- secondary-index-to-primary fetch penalty;
- cache hit expectations if available;
- and whether a plan is likely sequential or fragmented.

### Consequence

For Vinyl, better search alone is even less useful than for `memtx`.
Statistics and engine-aware costing are mandatory.

## Recommended Architecture

## High-level direction

Adopt a PostgreSQL-like phase split. Treat DPhyp, LinDP++, improved bounded DP,
and simpler heuristic search as candidates to evaluate rather than a
predetermined algorithm stack.

Do **not** start with a full ORCA port.

### Proposed planner pipeline

1. **SQL AST / resolved tree**
   - current parser and name resolution output

2. **Logical query IR**
   - scans, filters, projections, joins, aggregates, sort/limit
   - explicit join graph / hypergraph
   - explicit legality constraints for outer/semi/anti joins

3. **Metadata + statistics API**
   - relation cardinality
   - NDV / null fraction / MCV / histograms
   - joint NDV / multivariate MCV / functional dependencies
   - confidence, staleness, and cardinality semantics
   - key uniqueness
   - prefix distinctness
   - engine-specific physical costs

4. **Base access path enumeration**
   - table scan
   - pk scan
   - secondary index range
   - covering/non-covering variants
   - maybe future index intersection / bitmap-style path

5. **Property reasoning and lightweight memo**
   - required order
   - uniqueness
   - rewindability/materialization requirement
   - subset/property alternatives and dominance
   - no distribution dimension initially

6. **Join search**
   - selected by benchmark and search budget
   - candidates include improved bounded DP, DPhyp, and deterministic heuristic
   - LinDP++ remains optional until justified by measurements

7. **Physical plan**
   - still mostly nested loops at first
   - later can grow hash join / merge join nodes

8. **VDBE lowering**
   - final chosen physical plan becomes VDBE

## Minimum Planner Contracts

These are semantic contracts, not final C layouts. They are deliberately small
enough to prototype in statement-lifetime arenas.

```c
struct SqlRelSet {
	/* Initial design deliberately inherits the current 64-relation ceiling. */
	uint64_t bits;
};

struct SqlPredicate {
	struct SqlRelSet referenced_rels;
	enum SqlPredicateKind kind;
	struct Expr *expr;
	double selectivity;
	double confidence;
};

struct SqlJoinEdge {
	struct SqlRelSet left_required;
	struct SqlRelSet right_required;
	uint32_t *predicate_ids;
	enum SqlJoinType join_type;
	/* Explicit legality/dependency constraints, not implicit loop-builder state. */
	struct SqlRelSet required_before;
};

struct SqlPlanProperties {
	struct SqlOrderKey *order;
	uint32_t order_count;
	struct SqlUniqueKey *unique_keys;
	bool rewindable;
};

struct SqlPlanCost {
	double startup;
	double total;
	double rows;
	double row_width;
	double cpu;
	double memory;
	double engine_access;
	double confidence;
};

struct SqlAccessPath {
	uint32_t relation_id;
	enum SqlAccessKind kind;
	struct SqlNormalizedAccessRequest request;
	struct SqlPlanProperties produced;
	struct SqlPlanCost cost;
};

struct SqlMemoKey {
	struct SqlRelSet relations;
	struct SqlPlanProperties required;
};
```

The lightweight memo maps `SqlMemoKey` to non-dominated alternatives. A plan
dominates another plan only when it is no worse in cost and satisfies at least
the same required properties. The memo also records exploration state, elapsed
budget, candidate count, and estimation confidence.

Property keys must be canonical and comparable without walking expression
trees: relation/column identifiers, collation, direction, and NULL ordering are
interned in statement-lifetime metadata. Costs use calibrated abstract CPU,
memory, and engine-access units plus estimated rows; startup and total costs
remain separate so `LIMIT` can prefer low-startup plans. Confidence is not
added to cost directly. It controls sensitivity analysis, fallback visibility,
and whether close alternatives should be retained.

The normalized statistics API returns planner-facing summaries, not raw
sketches. Selectivity estimation follows an explicit precedence:

1. exact constraints, uniqueness, and known keys;
2. multivariate MCV entries;
3. functional dependencies and joint NDV;
4. single-column MCV/histograms with correlation adjustment;
5. independence fallback, marked with low confidence.

The statistics infrastructure owns sampling, sketches, merge, persistence, and
refresh. The optimizer owns only this normalized contract and its interpretation.

Initial multivariate groups are selected explicitly from:

- multi-part primary/unique/secondary keys;
- frequently co-occurring predicate columns observed by workload telemetry;
- administrator-declared groups.

Key definitions provide exact functional dependencies only when SQL NULL and
uniqueness semantics make them valid. Sampled dependencies carry strength and
confidence and never silently become constraints.

Primary and unique indexes make exact functional dependencies an early,
high-value Tarantool capability: when uniqueness and NULL semantics permit,
the key determines every stored column without statistical discovery. The
estimator should exploit these catalog-derived dependencies before implementing
sampled dependency discovery.

### Relation-set ceiling

The first implementation deliberately retains the current `Bitmask` limit of
64 FROM items. This keeps graph, memo-key, hashing, and compatibility adapters
simple. Supporting wider joins requires a separate redesign of relation sets,
memo keys, legality constraints, and current planner interfaces. It should be
triggered by a measured workload, not speculative extensibility.

## Cost Model And LogEst Transition

The current planner represents rows and scalar cost in `LogEst`, approximately
`10*log2(x)`. This is compact and efficient for the current solver, but it is
not a sufficiently explicit interface for engine-aware costs and uncertainty.

The new planner uses the linear abstract cost components in `SqlPlanCost`.

Definitions:

- one CPU unit is calibrated to a simple predicate/tuple-processing baseline;
- engine-access units are converted to the same abstract scale by engine cost
  snapshots;
- memory cost represents expected bytes retained and materialization pressure,
  not elapsed time by itself;
- `startup` is work before the first row;
- `total` is startup plus expected work to consume all rows;
- `LIMIT` costing interpolates between startup and total using expected demand.

The first formulas are calibrated from controlled microbenchmarks:

- memtx point/range/full scan;
- Vinyl point/range/full scan under representative amplification/cache states;
- tuple field decode and comparison;
- sort/materialization;
- repeated nested-loop probes.

Compatibility adapters convert `LogEst` cardinality into linear rows at the
boundary. They do not pretend old scalar cost and new total cost share numeric
units. Dual-planner comparison evaluates selected-plan execution and records
both native cost explanations separately.

Confidence does not directly multiply cost. Low confidence causes:

- sensitivity evaluation using low/high cardinality bounds;
- retention of close alternatives;
- visible fallback or conservative selection;
- explicit diagnostics in replay.

Before production use, calibration coefficients are versioned and replayed
with optimizer snapshots so plan changes caused by recalibration are
reproducible.

## Join search policy

There is no committed algorithm split yet. The initial work is an enumerator
evaluation harness with common inputs, properties, memo, budgets, and cost
model.

### Algorithms to compare

- improved current bounded DP;
- DPhyp;
- deterministic greedy/beam fallback;
- LinDP++ only after the first three establish a measurable gap it can address.

### Graphs and properties to measure

- chain, star, snowflake, clique/dense, disconnected;
- inner, outer, semi, anti, and lateral legality constraints;
- equi joins, non-equi joins, OR-connected predicates, and predicates
  referencing more than two relations;
- no-order and interesting-order alternatives;
- accurate, stale, correlated, and deliberately poor statistics.

Current Tarantool does not support SQL `LATERAL`; correlated subqueries are the
current semantic source of dependency constraints. The harness may still use
synthetic lateral-style dependency graphs to test the graph model, but they are
not SQL parity cases.

### Budget

Switching must be budget-based, not controlled only by relation count. Record:

- connected subsets/hyperedges enumerated;
- physical candidates costed;
- memo bytes;
- elapsed planning time;
- best-plan improvement over the incumbent;
- estimation confidence.

The production policy is chosen from benchmark results. Any fallback must be
deterministic, preserve legality strictly, and be visible in `EXPLAIN` and
planner counters.

### Enumerator harness contract

The harness consumes a standalone replay object:

- relations and base access paths;
- normalized predicates and residual expressions;
- join/dependency/legality edges;
- required and interesting properties;
- normalized statistics and engine costs;
- search and memory budgets.

Each enumerator returns:

- selected physical plan and native cost explanation;
- planning elapsed time and peak bytes;
- generated, dominated, truncated, and retained candidate counts;
- budget/fallback reason;
- deterministic plan fingerprint.

The initial corpus contains checked-in replay objects generated from specific
SQL queries plus synthetic graphs. Run each cold and warm enough times to
report median and p95; reject comparisons with unstable fingerprints or
excessive variance. A replacement must improve targeted execution outcomes
without violating preparation/memory gates. Lower internal estimated cost is
not sufficient.

### Improved bounded-DP baseline

The improved-current candidate is:

- current-compatible expansion and legality;
- configurable candidate/time/memory budgets instead of fixed `1/5/10` width;
- candidates partitioned by relation subset and relevant properties;
- property-aware dominance before beam truncation;
- deterministic tie-breaking and full harness counters.

This candidate establishes whether statistics, properties, and principled
pruning provide most of the benefit without a new graph enumerator.

### Hypergraph construction scope

The first graph builder extracts safe conjunctive connectivity and dependency
facts. It does not attempt arbitrary Boolean theorem proving:

- simple equi and non-equi predicates connect referenced relations;
- predicates referencing more than two relations become hyperedges;
- OR-connected/mixed Boolean predicates remain residual when safe
  decomposition is unavailable;
- outer/correlated dependencies become explicit legality constraints;
- residual predicates are costed conservatively and evaluated at the earliest
  legal node.

Replay records which predicates were extracted and which remained residual, so
the harness can measure plan-quality loss from incomplete graph construction.

## Why not use ORCA as the primary architecture immediately

Because Tarantool is not yet at the point where memo cost is paid back.

Memo/Cascades becomes justified when all of these are true:

1. multiple physical join algorithms exist;
2. query rewrites are numerous and valuable;
3. physical properties matter beyond simple ordering;
4. statistics are rich enough to compare many alternatives meaningfully;
5. optimization time is acceptable or can be amortized heavily.

Today Tarantool is not there yet.

## Required foundations before replacing the search algorithm

### 1. Real statistics

This is mandatory.

Need a new SQL statistics subsystem with:

- table row count
- per-column null fraction
- per-column NDV
- MCV lists
- equi-depth histograms
- correlation with physical/index order where useful
- prefix NDV for indexes
- joint NDV and multivariate MCV for selected correlated columns
- uniqueness, functional dependencies, and correlation summaries
- confidence, staleness, provenance, and named cardinality semantics

For `memtx`:

- physical index cardinality can remain cheap;
- committed or snapshot-visible relation cardinality must be named separately;
- NDV/MCV/histograms can be sampling-based;
- prefix NDV for primary and secondary indexes should be collected.

For `vinyl`:

- same logical stats, plus physical read-cost telemetry;
- planner should not confuse logical selectivity with read cost.

### 2. Separate access-path costing from join search

Current `where.c` mixes both too much.

Introduce:

- `RelInfo` or equivalent for base relation facts
- `AccessPath` objects for base scans
- `JoinEdge` / `JoinHyperEdge`
- `JoinPlan` candidates

### 3. Join legality graph

Represent:

- inner joins
- left/right/full outer joins
- semi/anti joins
- lateral dependencies

This can be done in a PostgreSQL-like `SpecialJoinInfo` style or a hyperedge
constraint layer. The key is to stop encoding legality only implicitly inside
the current loop builder.

### 4. Property model

Initial properties should be minimal:

- output ordering
- uniqueness
- maybe covering / late materialization eligibility

Do **not** start with a huge trait lattice.

This model is required before evaluating a replacement join enumerator. Without
it, an enumerator may choose a cheaper unordered partial plan that forces a
more expensive final sort and regress existing ORDER BY behavior.

## Implementation phases

The implementation phases, their dependencies, status tracking, and worktree
parallelism live in [`roadmap.md`](roadmap.md). At a glance, the sequence is:

1. **M0** — parity corpus + baseline harness;
2. **M1** — planner observability and replay;
3. **S0 → S1 → S2** — statistics audit, then relation/index stats, then
   column stats + sketches;
4. **M3** — single-table physical IR + VDBE lowering;
5. **E1** — improved bounded DP baseline (replaces fixed 1/5/10 widths with
   configurable budgets and property-aware dominance);
6. **GATE** — decide whether an enumerator bake-off
   (DPhyp / LinDP++ / deterministic greedy) is needed at all, based on
   E1 and S2 measurements.

The architectural commitments above (no enumerator before stats, no memo
before physical alternatives, no ORCA before workload evidence) drive the
roadmap ordering.

## Concrete recommendation

### Recommended medium-term target

Build a **PostgreSQL-inspired but not PostgreSQL-cloned** optimizer:

- separate logical IR
- normalized statistics contract backed by a separate statistics project
- access-path objects
- join legality constraints
- minimal properties and lightweight memo
- benchmark-selected join enumeration and fallback policy

This gives Tarantool most of the practical gains it needs without paying the
full ORCA complexity tax too early.

### Recommended long-term target

Move toward **selected ORCA ideas**, not an immediate GPORCA clone:

- memo groups if and when physical alternatives multiply
- branch-and-bound search
- richer property enforcement
- metadata cache and minidump-style optimizer testing

### What not to do

- Do not swap in a new join enumerator before adding real statistics.
- Do not import a full memo optimizer while the executor still has a narrow
  physical space.
- Do not keep engine costing buried inside one planner file.

## Short answer to the key questions

### "How to do something similar to PostgreSQL planner?"

Borrow these ideas:

- separate relation/access/join objects;
- separate logical planning from code generation;
- represent ordering as a property;
- use exact search for small joins and heuristic fallback for large joins.

Do **not** copy GEQO first and do **not** copy PostgreSQL's cost constants.

### "DP, DPhyp, LinDP, GPORCA?"

- **DPhyp**: strong exact-enumerator candidate that must be benchmarked.
- **LinDP++**: research candidate for medium/large queries, not a committed
  fallback.
- **PostgreSQL DP/GEQO**: useful reference architecture; compare it rather than
  rejecting its algorithm set by assumption.
- **GPORCA**: best long-term reference for modular optimizer architecture, but
  its replay, memo, and property ideas are useful before a full rule engine.

### "Is it applicable to memtx?"

Yes, strongly, but with tight planning budgets and CPU/cache-aware costing.

### "How should it be different for vinyl?"

Vinyl needs a distinct engine cost model that understands LSM read
amplification, bloom filters, ranges/runs, and secondary-to-primary lookup
penalties. Better join search without that will underperform.

## Follow-up: Statistics Sketches

This is a separate statistics-infrastructure track, not an implementation
phase owned by the optimizer. The optimizer consumes normalized summaries.

## Why sketches matter

If Tarantool grows a serious statistics subsystem, sketches should be part of
the design from the beginning.

The reason is not that the planner should consume raw sketches directly. The
reason is that sketches are:

- compact;
- mergeable;
- friendly to incremental maintenance;
- and naturally suited to distributed aggregation across shards.

That makes them a better storage and transport substrate than trying to keep
all planner stats as exact lists or full raw samples.

## Recommended statistics layers

The recommended design is three-layered.

### Layer 1: exact cheap metadata

These values should remain exact whenever the engine already exposes the
required semantics cheaply. Counts must state whether they are physical,
committed, or snapshot-visible:

- relation cardinality
- primary and secondary index cardinality
- nullability
- uniqueness
- key-part structure
- sort order / collation
- engine type and physical access capabilities

### Layer 2: planner-facing normalized stats

These are the objects the planner should read directly:

- per-column NDV
- null fraction
- average width
- MCV lists
- equi-depth or hybrid histograms
- prefix NDV for index keys
- selected multi-column stats
- functional dependency or correlation summaries where useful

This layer should be explicit and easy to interpret. It should not require the
planner to know sketch internals.

### Layer 3: internal mergeable sketch substrate

This layer is what collection, maintenance, and distributed merge should use.

Recommended sketch families:

- NDV:
  - HyperLogLog or a similar mergeable cardinality sketch
- quantiles / range boundaries:
  - KLL or t-digest style sketches
- heavy hitters / MCV candidates:
  - SpaceSaving or Count-Min + heap style structure
- optional correlation / joint sketches:
  - only if multi-column stats become important enough

## What should use sketches directly

Good direct sketch uses:

- global NDV from per-shard NDV
- quantile merging across shards
- candidate heavy hitter detection
- incremental stats refresh

Bad direct sketch uses:

- making the optimizer interpret raw sketch structures during costing
- replacing histograms and MCVs entirely with opaque approximate objects

The planner should still mostly see:

- NDV
- histogram buckets
- MCV entries with frequencies
- prefix selectivity summaries

Sketches should produce those planner-facing summaries.

## Statistics maintenance policy

For Tarantool, a practical policy is:

- exact counts stay live where cheap
- sketches/histograms are refreshed by `ANALYZE`
- optional background incremental refresh may update them later
- stale-stat quality should be recorded explicitly

Each statistic should carry:

- collection timestamp
- sampling rate
- error/quality estimate where available
- engine/shard provenance

This becomes important once planning decisions start switching between exact,
bounded, and heuristic search strategies.

because search quality should depend partly on statistics confidence.

## Follow-up: Column Engine

This is a separate multi-year storage/execution track. It constrains interfaces
but is not part of the core planner MVP.

## Where a column engine fits

A future column engine should be treated as a new physical access backend, not
as a replacement for the logical optimizer.

The logical planner should reason about:

- relation
- predicate
- join
- projection
- aggregation

independently from whether the relation is physically:

- row-store `memtx`
- row-store `vinyl`
- column-store
- remote/sharded

That means the prerequisite is still the same:

- separate logical IR
- separate access-path enumeration
- engine-specific cost interface

## What a column engine adds to the physical plan space

Once a column engine exists, new physical alternatives appear:

- column scan
- column projection scan
- vectorized filter
- vectorized aggregation
- late materialization
- row reconstruction
- maybe zone-map or min/max pruning

Those are not just “different scan costs”. They are different operator
families. This is one of the main reasons a cleaner optimizer IR is needed.

## Planner consequences

The cost model must then account for:

- bytes read per referenced column
- decompression cost
- per-batch vector execution cost
- row reconstruction cost
- benefit of late materialization
- ordering and grouping properties available from column segments

For point lookups and OLTP:

- row-store paths remain the default

For scans / wide analytics:

- column-store paths may dominate

For mixed workloads:

- hybrid plans become plausible

Example:

- filter and aggregate on a columnar path
- reconstruct rows only for the surviving top-N or final projection

## When a column engine changes optimizer architecture

A column engine does not force ORCA immediately.
But it does increase the value of:

- property tracking
- memoized subproblem reuse
- and richer physical alternatives

So if Tarantool adds a real column engine, the case for an ORCA-like
architecture becomes stronger than it is today.

## Follow-up: Distributed Planning Over vshard

This is a separate distributed-SQL track after the local planner, statistics,
and physical-plan contracts are stable. It is not part of the core planner MVP.

## Why this should be layered

Tarantool already has sharding support in `vshard`, but it is mainly a Lua-side
execution/routing model, not yet a native SQL distributed planner.

The correct optimizer architecture is to layer distributed planning above a
strong local planner, not to entangle distribution into local `where.c`-style
logic.

The split should be:

- local planner:
  - access paths
  - local join order
  - local ordering/grouping choices
- distributed planner:
  - shard pruning
  - pushdown legality
  - remote fragment planning
  - data movement
  - global gather/merge/aggregate

## Stage 1: shard-aware planning without repartition

The first distributed SQL stage should only require metadata such as:

- shard key
- colocation group
- distribution policy
- route-to-one-shard detectability
- route-to-subset detectability

This already enables:

- single-shard pushdown
- subset-shard pushdown
- scatter-gather with coordinator finalization
- partial aggregate pushdown
- local top-N pushdown

## Stage 2: distributed statistics

This is where sketches become especially useful.

Per-shard stats should be stored locally, then merged into global summaries
through mergeable sketches.

Examples:

- per-shard HLL -> global NDV
- per-shard KLL/t-digest -> global quantiles
- per-shard heavy hitters -> merged MCV candidates

The global planner can then reason about:

- shard pruning effectiveness
- skew
- whether partial aggregation is beneficial
- whether one or a few shards dominate cost

## Stage 3: distributed physical operators

Eventually the distributed planner needs explicit physical nodes like:

- `RemoteScan`
- `RemoteFilter`
- `RemoteProject`
- `RemotePartialAgg`
- `Gather`
- `GatherMerge`
- `RepartitionExchange`
- `BroadcastExchange`

Only at this point does the optimizer start to resemble systems such as
Greenplum, Citus, or distributed Volcano/Cascades variants.

## How this differs from current vshard Lua mode

Current application-managed vshard routing leaves important planning metadata
outside SQL:

- colocation guarantees
- join locality
- remote cardinality estimates
- serialization cost
- shard fanout cost
- resharding stability

A native distributed SQL planner must own this metadata explicitly.

That means:

- system metadata for table distribution
- shard-aware stats
- explicit remote plan fragments
- and coordinator-side costing

## MessagePack Implications

## Current row-oriented MsgPack impact

Tarantool's internal row format is fundamentally MsgPack tuple oriented.
That influences both statistics and operator design.

For statistics, row-oriented MsgPack means:

- extracting one column often still involves field-map or left-to-right field
  navigation unless offsets are cached;
- sampling for `ANALYZE` is more expensive than if values were already stored in
  column vectors;
- multi-column prefix stats for indexes are relatively natural, because index
  keys are already ordered by key parts;
- full-table per-column profiling costs more CPU due to decode and navigation.

For operators, row-oriented MsgPack means:

- tuple reconstruction is cheap because the row already exists as one object;
- point lookups and OLTP-friendly row access are natural;
- projection-heavy scans pay repeated field extraction and decode costs;
- vectorized execution is awkward unless batches or decoded side buffers are
  introduced.

## Statistics under row-oriented MsgPack

With row-oriented MsgPack, planner stats should avoid repeatedly decoding full
rows whenever possible.

Practical implications:

- use index-ordered sampling where it directly gives prefix distributions
- keep stats collection operators close to tuple field maps and key defs
- prefer approximate/mergeable sketch updates over materializing large decoded
  samples
- cache extracted scalar representations during stats collection batches

There is no fundamental blocker here, but stats collection cost is higher than
in a columnar layout.

## If MessagePack were column-oriented instead of tuple-oriented

If internal MsgPack were organized by columns rather than tuples, several
optimizer-relevant things would change.

### Statistics would get easier

- per-column histograms and NDV collection become much cheaper
- quantile sketches become straightforward to build in scan order
- MCV and null-fraction collection becomes almost trivial
- predicate selectivity estimation can be refreshed more often

### Some operators would get cheaper

- projection-heavy scans
- vectorized filters
- vectorized aggregates
- top-N over a few columns

### Some operators would get harder

- point lookup returning full rows
- transactional row replacement
- mixed-row reconstruction
- update/delete paths that want full tuple semantics

So a column-oriented MsgPack layout would improve planner stats and analytic
execution, but it would also pull the storage/execution design away from
current OLTP-friendly tuple semantics.

## Best compromise for Tarantool

The best compromise is probably not “replace tuple MsgPack with column MsgPack”
globally.

A better path is:

1. keep tuple-oriented MsgPack as the canonical row-store format for `memtx`
   and `vinyl`;
2. build planner stats through sketches/histograms using row samples and index
   samples;
3. if a column engine is introduced, let it own a columnar physical format
   separately;
4. let the optimizer choose between row-store and column-store access paths.

This preserves OLTP strengths while still giving the optimizer a route toward
cheap stats and analytic execution.

## Practical takeaway

Sketches, a column engine, and distributed planning all fit the same general
architecture if the planner is refactored correctly:

- local logical IR
- engine-specific cost and stats APIs
- planner-facing normalized stats
- sketches as mergeable stats substrate
- optional columnar physical backend
- optional distributed fragment planner above the local optimizer

That is another reason not to keep extending the current monolithic
`where.c`-style architecture indefinitely.
