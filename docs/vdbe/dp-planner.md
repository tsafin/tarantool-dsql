# Bounded DP Planner and Cost Model

This document describes the **current SQL WHERE planner** in
[`where.c`](../../src/box/sql/where.c) and its private structures in
[`whereInt.h`](../../src/box/sql/whereInt.h), as implemented on 2026-10-03.
It is an implementation specification, not a proposal for DPhyp or the new
single-table physical-plan producer. The planner enumerates access loops,
constructs left-deep nested-loop join orders with bounded **dynamic
programming (DP)**, and emits the chosen loop order to the existing VDBE code
generator.

## What “dynamic programming” means here

Dynamic programming solves a larger optimization problem by building on
solutions to smaller subproblems. Here the larger problem is: **choose one
access method for each FROM relation, and choose their outer-to-inner join
order, minimizing estimated work while respecting dependencies and requested
ordering**. A smaller subproblem is a partial join plan covering a subset of
relations. Its stored result is not just one cost: it includes estimated
output rows and ordering properties, because those change the cost of adding
the next relation.

The search is **bottom-up, not top-down**. It begins with an empty path, then
builds paths containing one relation, two relations, and so on until every
relation is present. At depth *k*, it extends each retained *k*-relation
path with every legal access loop for a relation not yet used. It never starts
with the full join and recursively asks for the best smaller join.

For a three-relation query, the search has this shape (some arrows may be
illegal because a loop needs another relation to run first):

```mermaid
flowchart LR
    E["Depth 0: empty path"] --> A["Depth 1: A"]
    E --> B["Depth 1: B"]
    E --> C["Depth 1: C"]
    A --> AB["Depth 2: A → B"]
    B --> BA["Depth 2: B → A"]
    A --> AC["Depth 2: A → C"]
    AB --> ABC["Depth 3: A → B → C"]
    BA --> BAC["Depth 3: B → A → C"]
    AC --> ACB["Depth 3: A → C → B"]
```

`A → B` and `B → A` cover the same subset, but are different left-deep
execution orders. For example, if A has 1,000 rows and B has five, placing B
outside a keyed lookup into A may require only five lookups; placing A outside
may require 1,000. The model prices both *when their prefixes survive* and
can pick the cheaper complete path. Each partial path carries its accumulated
cost and row estimate forward, avoiding a fresh cost calculation for its
entire prefix every time another relation is appended.

**Does it examine every plan? No.** It tries every available WhereLoop against
every *retained* prefix at the next depth, but it does not retain every prefix.
It first removes dominated states and then applies a global beam-width cap.
If a promising prefix is discarded at depth *k*, none of its length-*k + 1*
or longer descendants is explored. The selected plan is therefore the least
estimated-cost plan **among retained complete paths**, not necessarily the
least-cost plan in the entire legal search space. The one-/two-/many-relation
beam defaults are 1/5/10, not `2^N` states or `N!` join orders.

**Where is the memo?** There is no persistent hash table keyed by every
relation subset, and no exhaustive per-subset memo. `wherePathSolver()`
uses two bounded arrays of `WherePath` records: `aFrom` is the retained
frontier at depth *k*; `aTo` collects depth *k + 1*. Each record stores the
prefix's relation mask, loop sequence, costs, row estimate, and order/reverse
properties. These records are the temporary, memo-like reuse of subproblem
results. Within a depth, paths with the same `(relation mask, satisfied order,
reverse-scan mask)` form a comparison partition; a path is dropped only if
another is no worse in total cost, unsorted cost, **and** output rows. Several
incomparable paths for one partition may survive. After the round, the arrays
swap roles; older-depth states are reused/overwritten, not kept as a complete
history. The global beam can also evict a partition entirely.

This is best described as **beam-limited, bottom-up join-order DP**: it reuses
partial solutions like dynamic programming, but sacrifices exhaustive search
and optimality guarantees for bounded planning work and memory.

## Scope and terminology

- A **relation** is one `FROM` item. Each item has a bit in a `Bitmask`.
- A **WhereLoop** is one possible access method for one relation: for example,
  a primary scan, a secondary-index equality lookup, a range scan, or an
  automatic-index lookup. Several loops may exist for the same relation.
- A **WherePath** is an ordered sequence of WhereLoops with no repeated
  relation. Its sequence is the outer-to-inner order of a nested-loop plan.
- A **beam** is the bounded set of partial WherePaths retained after one
  join-depth round. The beam is global, not one beam per relation subset.
- **DP** stands for dynamic programming; the concrete bottom-up and bounded
  behavior is described above.
- **LogEst** represents roughly `10 × log2(value)`. Ordinary addition of two
  LogEst values represents multiplication of their underlying linear values;
  `sqlLogEstAdd(x, y)` represents addition of those linear values. Values are
  rounded estimates, not measured runtime or calibrated time units.

The current WHERE DP and the newer M3 single-table physical producer are
different planner paths. The latter has limited cost-ranked secondary access
selection in `sql_physical_plan.c`; it does not replace this join-order DP.
The E1 decision about wider beam defaults and corpus-level plan quality is
also separate from whether the present DP cost model functions.

## Join coverage, size limit, and fallback behavior

The WHERE planner uses this DP to choose the outer-to-inner order for a
multi-relation `FROM` list. Its physical join operator is always a nested
loop: a `WhereLoop` specifies how to access the next relation, not a choice
between nested-loop, hash, and merge join algorithms. This distinction also
means that a supported SQL join is not necessarily freely reorderable.

| SQL construct | DP treatment |
| --- | --- |
| `INNER JOIN` and comma join | Supported; orders may be rearranged when predicate dependencies permit. |
| `CROSS JOIN` | Supported, but a prerequisite mask preserves the relevant left-to-right boundary. |
| `LEFT [OUTER] JOIN` | Supported with NULL-extension semantics; prerequisite masks prevent illegal movement of its right side ahead of the required left side. |
| `NATURAL JOIN` and `USING` | Supported; join/column resolution occurs before the WHERE planner sees the resulting predicates. Their underlying join type still determines ordering legality. |
| `RIGHT JOIN` and `FULL OUTER JOIN` | Rejected as unsupported before this DP runs. |
| Explicit `SEMI JOIN` or `ANTI JOIN` syntax | Unsupported. `EXISTS`, `NOT EXISTS`, and `IN` can express related semantics through subquery/set machinery; they are not additional physical join families in this DP. |

`sqlWhereBegin()` accepts at most `BMS = sizeof(Bitmask) * 8` `FROM` entries
in one planning invocation. The normal `u64` Bitmask makes this **64
relations**, or at most 63 binary join edges in a single flat join tree.
The limit applies to that invocation, not to a sum across separately planned
query blocks. Subquery flattening can change which entries end up in one
invocation. A build that overrides `SQL_BITMASK_TYPE` changes the limit with
the bitmask width. Exceeding the limit raises the SQL parser-limit error; it
does not invoke a different optimizer.

For every multi-relation invocation, `sqlWhereBegin()` builds WhereLoop
candidates and runs `wherePathSolver()`; a one-relation shortcut may bypass
it. There is **no relation-count threshold that switches from DP to a greedy,
genetic, or other faster join-order algorithm**. Instead, the DP is bounded
at *every* depth: by default it retains at most five paths for two relations
and ten paths for three through 64 relations. Thus 64 is a capacity limit,
not a promise to enumerate all legal orders or find a global optimum.

```mermaid
flowchart LR
    SQL[Resolved SELECT and WHERE] --> WC[WhereClause / WhereTerm analysis]
    WC --> LB[WhereLoopBuilder]
    ST[Statement-pinned statistics snapshot] --> LB
    LB --> L[WhereInfo.pLoops: access candidates]
    L --> DP[wherePathSolver: bounded DP]
    DP --> W[WhereInfo.a: chosen WhereLevel order]
    W --> V[VDBE nested-loop code generation]
    DP --> O[Planner metrics and selected-path diagnostics]
```

## Main data structures

| Structure | Relevant fields | Purpose |
| --- | --- | --- |
| `WhereTerm` | `pExpr`, `eOperator`, `truthProb`, `prereqRight`, `prereqAll`, `leftCursor` | One analyzed predicate or subterm. Dependency masks say which other relations must already be available before using it as an index constraint. |
| `WhereClause` | `a`, `nTerm`, `pOuter`, `pWInfo` | Container for analyzed terms, including nested OR/AND clauses. |
| `WhereLoopBuilder` | `pWC`, `pWInfo`, `pNew`, `pOrSet` | Mutable template and context used to generate and insert access candidates. `pOrSet` is the small special-purpose OR-cost accumulator. |
| `WhereLoop` | `iTab`, `maskSelf`, `prereq`, `index_def`, `wsFlags`, `aLTerm`, `nEq`, `rSetup`, `rRun`, `nOut`, `iSortIdx` | One access alternative. `rSetup` is one-time setup cost, `rRun` is per-invocation cost, and `nOut` is rows produced per invocation, all in LogEst units. |
| `WherePath` | `maskLoop`, `aLoop`, `nRow`, `rUnsorted`, `rCost`, `isOrdered`, `revLoop` | A partial or complete join plan. `aLoop` is the ordered access sequence; `maskLoop` is its relation subset; the remaining fields are accumulated cardinality, costs, and order properties. |
| `WhereInfo` | `pLoops`, `pTabList`, `pOrderBy`, `nLevel`, `a`, `nRowOut`, `nOBSat` | Whole WHERE-planning invocation. `pLoops` owns candidate loops; `a` receives the winning order as `WhereLevel` records for code generation. |
| `WhereLevel` | `pWLoop`, `iFrom`, VDBE cursor/jump fields | One chosen executable loop level. `WhereInfo.a[0]` is outermost. |

`WhereLoop` candidates are held in a linked list at `WhereInfo.pLoops`.
`whereLoopInsert()` discards or replaces dominated access alternatives before
the path solver starts, provided their relation, sort property, prerequisites,
setup/run costs, and output estimates make the comparison valid. This is a
separate pruning stage from WherePath beam admission. OR branches use
`WhereOrSet`, which retains at most `N_OR_COST = 3` prerequisite/run/output
entries before constructing an OR access alternative.
`WhereMaskSet` maps sparse VDBE cursor numbers onto dense relation bits; the
join size is limited by the width of those dependency masks (normally 64).
`isOrdered` is the satisfied ORDER BY prefix length, or `-1` while unknown;
`revLoop` tracks which loop traversals must run backward.

The solver allocates two arrays of `WherePath` entries, `aFrom` and `aTo`,
plus contiguous storage for each entry's `aLoop` pointers. It swaps the two
arrays after every depth. An optional `aSortCost` array caches sorting costs
by number of ORDER BY terms already satisfied. Peak solver-frontier storage
is proportional to `2 × beam_width × join_depth`, plus the sort cache and
the separately allocated WhereLoop list.

## Candidate generation and statistics

`whereLoopAddAll()` generates alternatives for each relation. The B-tree
builder considers full scans, index equality and range constraints, and
eligible automatic indexes. A loop's `prereq` includes dependencies from
join terms: a lookup using `inner.key = outer.key` cannot be placed before
the outer relation. The planner also retains alternatives with different
ordering properties when neither dominates the other.

For ordinary index loops, `whereLoopAddBtreeIndex()` estimates rows visited
and then computes `rRun`. The current model uses:

1. Index/relation population and average rows per index-key prefix from the
   VDBE's immutable, schema-validated statistics snapshot when available.
   `index_field_tuple_est_with_snapshot(index, 0)` estimates the population;
   higher prefix lengths estimate matching rows. Missing or stale entries
   retain the legacy default estimates.
2. For a supported literal equality on the leading part of a non-unique
   index, the pinned SpaceSaving MCV interval midpoint can replace the
   prefix average. The supported literal types are integer/unsigned,
   Boolean, and binary-collated string. The estimate has a one-row floor.
   Bind parameters, computed constants, and unsupported types keep the
   existing estimate. An explicit `likelihood()` truth probability takes
   precedence over this MCV path.
3. Range and residual predicates use existing reduction heuristics;
   histogram-backed range selectivity and join-correlation estimation are
   not yet integrated into this cost model.

The B-tree cost is heuristic. In the code's approximate linear interpretation,
a full scan is proportional to rows visited, with a penalty for a secondary
index scan. A constrained index lookup pays a seek term plus row-visitation
cost; a non-covering lookup also pays main-table fetch cost. `IN` multiplies
the estimated number of seeks. Automatic indexes carry an `rSetup` cost for
building the transient index. The code converts these terms to LogEst and
combines them with `sqlLogEstAdd()`. These costs are relative scores, not
durations in microseconds. The precise constants and branches are in
`whereLoopAddBtree()` and `whereLoopAddBtreeIndex()`; they are intentionally
subject to calibration rather than a stable external API.

## DP state transition

`wherePathSolver()` receives the complete candidate list and a requested
output-row estimate for sorting. It starts with one empty path. The seed's
`nRow` is `min(pParse->nQueryLoop, LogEst(28))`: this accounts for the number
of times the WHERE subprogram may be invoked from an outer context and caps
the automatic-index payback horizon. For each join depth it tries every
retained path with every WhereLoop.

A candidate extension is legal only if:

- every bit in `loop.prereq` is already in `path.maskLoop`;
- `loop.maskSelf` is not yet in `path.maskLoop`;
- an automatic-index loop is not used when the outer path is estimated to run
  fewer than two times.

Join predicates and outer-join restrictions feed the prerequisite masks, so
the solver cannot fix a dependency violation by assigning a low cost to an
otherwise illegal order. Each legal extension adds exactly one new relation.

The central recurrence, in the code's LogEst arithmetic, is:

```text
new_subset      = old.maskLoop | loop.maskSelf
new_rows        = old.nRow + loop.nOut
new_unsorted    = logAdd(loop.rSetup,
                         loop.rRun + old.nRow)
new_unsorted    = logAdd(new_unsorted, old.rUnsorted)
new_cost        = logAdd(new_unsorted, sort_cost)
                  if the required ordering is not fully satisfied;
                  otherwise new_unsorted
```

Here `logAdd` means `sqlLogEstAdd`. Thus `loop.rRun + old.nRow` models running
the inner access once per estimated outer row; `new_rows` models the product
of outer and per-invocation output cardinality. `rSetup` is charged once for
that loop, and prior work remains in `old.rUnsorted`. The planner then checks
ORDER BY satisfaction (including possible reverse index traversal), assigns
`isOrdered`/`revLoop`, and adds sorting cost when needed. Sorting estimates
follow roughly `3 × N × log(N)`, scaled for an already-sorted prefix and
limited by eligible LIMIT information.

The search loop can be summarized without its VDBE-specific bookkeeping:

```text
frontier = { empty_path }
for depth = 1 .. relation_count:
    next = {}
    for path in frontier:
        for loop in WhereInfo.pLoops:
            if dependencies_are_met(path, loop) and relation_is_unused(path, loop):
                candidate = extend_and_price(path, loop)
                admit_by_partition_dominance_and_global_beam(next, candidate)
    frontier = next
winner = first_minimum(frontier, path.rCost)
copy winner.aLoop into WhereInfo.a
```

```mermaid
flowchart TD
    A[Retained paths at depth k] --> B[Try every WhereLoop]
    B --> C{Prerequisites met and relation unused?}
    C -- no --> B
    C -- yes --> D[Compute rows, run/setup cost, ordering, sort cost]
    D --> E{Dominated in same subset/property partition?}
    E -- yes --> X[Discard; count dominated]
    E -- no --> F[Remove states dominated by candidate]
    F --> G{Beam has room?}
    G -- yes --> H[Retain candidate]
    G -- no --> I[Choose eviction victim]
    I --> J{Candidate beats victim?}
    J -- no --> Y[Discard; count truncated]
    J -- yes --> H
    H --> K[Next depth; swap aFrom and aTo]
```

## Dominance and bounded admission

Two paths are comparable for path-level dominance only when they have the
same relation-subset mask, same number of satisfied ORDER BY terms, and same
reverse-scan mask. Inside that partition, a path dominates another when its
`rCost`, `rUnsorted`, **and** `nRow` are all no greater. An incomparable path
can be worth retaining even if it currently costs more: it may have a smaller
intermediate result or unsorted cost that pays off after a future loop.

The beam width is global for each depth. Defaults are **1** for one relation,
**5** for two, and **10** for three or more. Process-start environment values
`SQL_PATH_SOLVER_WIDTH_ONE`, `_TWO`, and `_MANY` may override these with
integers in `1..64`; malformed or out-of-range values use the defaults. They
are read lazily once per process, not changed per SQL session.

When a full beam receives a non-dominated candidate, admission first prefers
evicting the worst member of a partition with at least two retained paths.
If every path is a partition singleton, it considers the globally worst path.
"Worst" compares `rCost`, then `nRow`. The new candidate replaces the victim
only when it wins that comparison; otherwise the new candidate is truncated.
This rule encourages subset/property diversity but does **not** promise that
every partition survives. At the final depth, the solver chooses the
retained path with the lowest `rCost`; an equal-cost tie keeps the earlier
retained path. Enumeration/insertion order therefore remains relevant to
ties; there is no independent canonical-fingerprint tie-break for joins.

The beam prevents the factorial number of join orders from being retained.
Each round still scans every retained prefix against the candidate-loop list,
and the full-beam duplicate-partition victim search compares retained pairs.
With beam width `B`, `L` candidate loops, and `J` relation levels, the solver
uses `O(B × J)` frontier pointers and has a coarse worst-case search bound of
`O(J × B × L × B²)` before accounting for ordering checks. The bound is an
implementation upper bound, not a claim of exhaustive DP optimality.

## ORDER BY, shortcut, and output

When an ORDER BY clause is present, `sqlWhereBegin()` calls the solver twice.
The first call uses `nRowEst = 0` and ignores sorting to estimate output rows;
the second uses that estimate and includes order/sort costs. The second
selection supplies the executable plan. A one-relation shortcut may bypass
the general WhereLoop enumeration and solver; multi-relation joins use it.

After the last round, each winning `aLoop[i]` is copied to
`WhereInfo.a[i].pWLoop`, with the matching FROM position and cursor. The
order of `WhereInfo.a` is the outer-to-inner VDBE loop nesting; the chosen
`nRow` becomes `WhereInfo.nRowOut`. The solver itself does not execute a
query. The WHERE code generator opens cursors, emits seeks/iterations and
predicate tests, and closes the loops using this chosen plan.

Per-statement instrumentation counts paths **generated**, **dominated**,
**truncated**, and **retained**. `EXPLAIN (planner = 'snapshot')` can expose
those metrics. The final-path replay-candidate capture is narrower than the
DP: it records only an ordinary one-relation final beam, not joins or OR
sub-solvers. Its fingerprints and selected-path metadata must not be treated
as a complete multi-relation DP trace.

## Observed behavior and remaining limits

[`sql_stats_test.lua`](../../test/sql-luatest/sql_stats_test.lua) contains live
volatile-ANALYZE regressions on both memtx
and Vinyl. With 100 rows joined to five rows, a no-statistics plan starts at
the first table; after ANALYZE the DP starts at the five-row table. A second
same-size, skewed fixture changes from an approximately ten-row index lookup
on the first table to an MCV-backed approximately one-row lookup on the
second. Both keep the same SQL result. A schema change that stales the
snapshot restores the default plan. These tests demonstrate that statistics
alter WhereLoop costs **and** the bounded DP's chosen join order.

| Two-relation fixture | Without fresh statistics | After volatile ANALYZE | Result |
| --- | --- | --- | --- |
| 100-row `a` joined to five-row `b` on primary key | Scan `a` first; default scan estimate ~1,048,576 rows | Scan `b` first; estimate ~5 rows | IDs 1–5 in both plans |
| Equally sized tables, `a.v = 1` matches 90 rows, `b.v = 1` matches one | Start with `a`'s index; default equality estimate ~10 rows | Start with `b`'s index; MCV estimate ~1 row | ID 1 in both plans |

They do not establish globally optimal plans, calibrated I/O/CPU units,
correlated-join estimates, histogram range estimates, end-to-end speedup, or
E1 acceptance. In particular, persistent statistics formats remain behind
their separate human sign-off gate; the demonstrated collection path is the
volatile TEST_BUILD ANALYZE implementation. The current production beam
defaults remain 1/5/10 until the workload-level quality and latency gates in
[`roadmap.md`](roadmap.md) are reviewed.

## Cost-function status: memtx versus Vinyl

**Cardinality inputs are partly data- and engine-specific; access costs are
not yet calibrated per engine.** Volatile `ANALYZE` samples the selected
memtx or Vinyl data and publishes statement-pinned relation/index summaries.
The current WHERE planner consumes relation population, index-prefix
averages, and supported leading-part literal-equality MCV estimates. These
inputs can change a WhereLoop's `nOut` and `rRun`, and the resulting DP join
order, on either engine. Missing or stale statistics revert to defaults.
This is a working statistics-aware *relative* cost model, not a measured
execution-time predictor. Persistence remains behind the separate S1 schema
approval gate.

| Surface | Implemented now | Missing for engine-aware costing |
| --- | --- | --- |
| Legacy WHERE / join DP | One LogEst setup/run formula for both engines; relation and index statistics may differ by engine. Full scans, seeks, secondary scans, base-tuple fetches, and automatic indexes have heuristic penalties. | Independently calibrated memtx/Vinyl CPU and access units, cache/read-amplification state, and measured repeated nested-loop probe cost. |
| Vinyl secondary access | Uses the same index-visit and base-fetch formula as memtx. `where.c` notes that its tuple-size approximation does not describe Vinyl secondary-index entries reliably. | LSM run/level and cache effects, bloom-filter behavior, and secondary-to-primary lookup amplification. |
| M3 single-table physical producer | Carries linear startup/total/rows/width/confidence fields, but the live SELECT adapter seeds total cost from row count and derives narrow point/prefix/range costs from that value. It does not replace the join DP. | A common-unit engine cost provider and calibrated path-specific access formulas before these costs can be compared across engines or integrated into a new join DP. |
| Selectivity | Snapshot cardinality, prefix NDV-derived averages, and narrow MCV equality are live; range and residual selectivity still use legacy heuristics. | Histogram range and multivariate/join-correlation estimates wired into the planner, with stale/confidence policy and stage-matched q-error validation. |

The `sql_index_tuple_size()` input is obtained from `space_bsize()` and
`index_size()`, so storage can affect a value used by the common formula; it
is **not** a separate Vinyl cost function. The memtx-specific simple
`COUNT(*)` fast path in `select.c` is an execution optimization, not a
memtx-specific join cost model. The proposed linear CPU/memory/engine-access
interface and its calibration workload are described in
[`next_gen_sql_planner.md`](next_gen_sql_planner.md#cost-model-and-logest-transition).

## Comparison with join-enumeration research

The comparison is about **planning time, search space, and plan quality under
the same estimated costs**. An algorithm's exactness means optimality *within
its supported plan space and cost model*; it does not imply minimum measured
query latency when cardinalities or engine costs are wrong. Numbers reported
by different papers are not a head-to-head Tarantool benchmark.

| Method | Search and practical strength | Fit and caveat here |
| --- | --- | --- |
| Current beam-limited, left-deep DP | Small bounded frontier; predictable work for up to 64 FROM entries. | Already executes nested loops and honors current prerequisite masks. Global beam truncation can lose the cheapest estimated legal plan. |
| [DPccp / DPhyp](https://15721.courses.cs.cmu.edu/spring2019/papers/23-optimizer2/p539-moerkotte.pdf) | Exact connected-subgraph/complement enumeration; DPhyp represents complex predicates and non-inner-join constraints with hyperedges. It avoids much unproductive subset testing on sparse graphs. | Good *reference enumerator*, but bushy alternatives need executable physical operators and a complete legality model. Dense graphs still have exponential search cost; merely swapping it into `where.c` does not produce hash/merge joins. |
| [Adaptive optimization for very large joins](https://db.in.tum.de/~radke/papers/hugejoins.pdf) | Exact search where tractable, then search-space reduction/near-optimal methods for large instances. Graph structure matters as well as relation count. | A budget- and graph-aware transition is a stronger candidate than a table-count-only switch, but its quality must be measured against this code's cost model and workload. |
| [DPomega join-and-sort optimization (2025)](https://link.springer.com/article/10.1007/s00778-025-00906-y) | Jointly optimizes join and interesting sort orders under the paper's conditions; reports efficient exact search for its evaluated problem. | Relevant to our ORDER BY path properties, but not evidence that replacing the current left-deep solver would be faster here. |
| [DPconv (SIGMOD 2025)](https://15799.courses.cs.cmu.edu/spring2025/papers/07-joins1/stoian-sigmod2025.pdf) | Fast subset convolution gives a practical exact `Cmax` variant; the authors report up to 29x speedup over DPsub on 24-relation clique queries for that objective. | Its practical result optimizes maximum intermediate cardinality, **not** our nested-loop `rRun`/`rSetup` objective. The paper explicitly says its convolution framework does not cover the cited nested-loop cost function; sparse JOB-like graphs see less benefit. Research comparator, not a drop-in implementation. |
| [PostgreSQL GEQO threshold](https://www.postgresql.org/docs/current/runtime-config-query.html) | A genetic heuristic limits planning time above a configurable FROM-item threshold; PostgreSQL documents the possible plan-quality loss. | We have no equivalent algorithm switch. A fixed relation-count threshold alone would ignore graph sparsity, and GEQO is not an assumed winner for this workload. |

There is therefore no defensible universal ranking of these methods by
"speed." DPconv's reported speedup is for a different objective and graph
class; an exact enumerator can improve estimated plan cost while using more
planning time; an approximate one can plan quickly but miss a good order.
The local decision must compare both planning and execution distributions on
the same queries, statistics generation, and engine.

## Nearest decision-oriented work

The following is a **proposed sequence**, not a claim that E1 or an engine
cost model is complete. The E1 measurement contract is
[`E1_WORKLOAD.md`](../../test/sql-baselines/E1_WORKLOAD.md); acceptance
thresholds and the reviewed workload still require a recorded decision.

1. **Establish the same-stats baseline.** Extend the reviewed workload with
   connected sparse, star, chain, cyclic/dense, LEFT/CROSS-constrained, skewed,
   range, and ORDER BY join cases on both engines. Capture plan identity,
   planning time, execution time, generated/dominated/truncated path counts,
   and stage-matched estimated/actual cardinalities. Compare current 1/5/10
   widths with the experimental 2/8/16 widths under the *same* statistics
   generation. Do not treat EXPLAIN drift alone as a quality result.
2. **Calibrate access costs independently of join enumeration.** Benchmark
   memtx and Vinyl point, range, full, and secondary-to-primary accesses,
   repeated nested-loop probes, cache states, and representative Vinyl
   read-amplification states. Define common abstract units with explicit
   engine parameters; preserve the old LogEst scores during A/B so no numeric
   unit is silently mixed. Validate predicted *relative path rankings* and
   execution-time regressions separately for each engine.
3. **Audit selectivity and legality.** Wire or explicitly defer histogram
   ranges and multivariate/join estimates; report q-error, including empty
   and stale cases. Independently model join graph connectivity, CROSS/LEFT
   dependencies, and physical-plan properties so an experimental enumerator
   cannot admit a semantically illegal order.
4. **Prototype an exact small-join comparator.** For a bounded, supported
   inner-join subset, retain the existing left-deep/nested-loop physical
   space and cost inputs while replacing global-beam truncation with an
   exhaustive per-subset/property memo. Use it as an *oracle for the estimated
   objective*, not as proof of runtime optimality. Add connected-subgraph
   pruning where legal; evaluate DPhyp-style hypergraph enumeration only
   after the legality representation and any desired bushy execution are
   specified. Record candidate count, peak memory, and planning time.
5. **Decide the production policy from paired evidence.** Compare beam width,
   exact small-join search, and a budgeted fallback by graph class and engine.
   Keep current behavior if lower estimated cost does not improve measured
   latency or if planning overhead/regressions exceed reviewed limits. A
   graph-/budget-aware transition is a candidate, not an already selected
   algorithm. Do not change production widths or claim E1 acceptance before
   the workload and thresholds are reviewed.

Tracks 1 (workload/instrumentation) and 2 (engine-cost calibration) can be
developed independently; the legality audit in 3 can also run in parallel.
The oracle in 4 needs the legality contract but can initially reuse the
current costs; recalibration is needed before interpreting engine-level
quality or changing production policy. The decision in 5 requires all
measurement tracks. None of these steps requires approving the draft
persistence format: the experimental comparison can use statement-pinned
volatile statistics.

```mermaid
flowchart LR
    A[Same-stats workload and measurements] --> D[Paired decision]
    B[memtx/Vinyl access-cost calibration] --> D
    C[Join legality and selectivity audit] --> O[Small-join exact oracle]
    A --> O
    O --> D
```
