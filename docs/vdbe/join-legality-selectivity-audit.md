# Join legality and selectivity audit for the exact-search comparator

This audit fixes the *first comparator's* scope. It compares the existing
bounded path solver against an exact enumeration over the **same generated
`WhereLoop` candidates and the same left-deep nested-loop cost model**. It
does not claim to enumerate every SQL-equivalent plan or every physical join
algorithm. The regression fixture is
[`dp_join_legality.test.lua`](../../test/sql-tap/dp_join_legality.test.lua).

## Legal extensions

`whereLoopAddAll()` assigns a bit to each `FROM` item and attaches a
`prereq` mask to each candidate. An index lookup driven by an expression
from another item includes that item's bit (`WhereTerm.prereqRight`). For a
right-hand item of `LEFT` or `CROSS JOIN`, the mask additionally includes
all preceding `FROM` items. The dependency is inherited by the immediately
following item through `priorJointype`; the implementation's precise rule
is `((jointype | priorJointype) & (JT_LEFT | JT_CROSS)) != 0`. The solver
appends a loop only when `prereq` is already a subset of the path's
`maskLoop`, and only if the loop's own bit is not already present.

Consequences for the comparator:

| Shape | First comparator | Reason |
| --- | --- | --- |
| Top-level, flat INNER JOIN (including comma join), 2–small-N items | Include | All executable left-deep orders are eligible, subject to individual candidate prerequisites. Cyclic and disconnected graphs are fixtures, not special exclusions. |
| CROSS JOIN | Defer | SQL result is inner-like, but this implementation deliberately makes it an order barrier. Treating it as free inner reordering would compare different legal spaces. |
| LEFT OUTER JOIN | Defer | The preserved side must precede the nullable side; ON/WHERE pushdown and NULL extension need separate proof. |
| NATURAL/USING | Defer as comparator syntax | They are rewritten into predicates and output-column rules. The fixture checks current SQL semantics, but comparator integration should start with explicit `ON` predicates. |
| RIGHT/FULL OUTER, explicit SEMI/ANTI | Exclude | They are not supported as join nodes by current SQL. `EXISTS`/`IN` subqueries are a different planning context. |
| Nested SELECT, OR sub-solver, DML SELECT producer | Defer | Each may have a separate `WhereInfo`, flattening boundary, or specialised search. One flat query block is the controlled first comparison. |

The comparator must consume existing `WhereLoop` candidates and evaluate
the same `prereq` test as `wherePathSolver()`; a join-graph edge alone is
*not* an adequate legality oracle. A relation with no connecting predicate
may still be appended (Cartesian product). Conversely, an indexed lookup
may require its source relation even in an otherwise commutative INNER JOIN.
The current path solver also rejects automatic-index paths when the outer
prefix is estimated below two rows; matching only the bitmask test would
silently enlarge the comparison space. ORDER BY prefix and reverse-scan
properties, plus setup/run/output estimates, are part of a distinct path
state; comparing only one cheapest path per relation subset is unsound.

`whereLoopInsert()` has already pruned some access candidates before either
solver sees them. Thus “exact” means exact over the retained candidate set
and legal left-deep plans under the existing cost formula, **not** an
optimality certificate for SQL generally. If the comparator eventually
supports outer joins, its legality cases should be added *after* a separate
NULL-extension and predicate-placement audit, not by clearing the barrier.

## Selectivity evidence and limit

The existing volatile-ANALYZE tests
[`test_dp_join_cost_uses_pinned_relation_rows`](../../test/sql-luatest/sql_stats_test.lua)
and `test_dp_join_cost_uses_pinned_mcv` show both memtx and Vinyl using
relation population and leading-key literal-equality MCV estimates to
change join order. These test **choice sensitivity**, not intermediate-join
cardinality accuracy. In `whereLoopAddBtreeIndex()`, prefix averages or
supported MCV values estimate indexed lookup output; range estimates and
residual predicate reductions remain heuristic. `wherePathSolver()` adds
`pWLoop->nOut` to prefix `nRow` in LogEst units, effectively multiplying
per-invocation row estimates across nested-loop levels. It does not consult
cross-relation correlation statistics.

The fixture gives `dpx` and `dpy` identical one-column `id` and `k`
marginals. Joining each with `dpa` on **both** columns yields respectively
three and zero rows, yet `EXPLAIN QUERY PLAN` reports the same per-loop
estimates for both without stats. This is a concrete counterexample to
treating a lower *estimated* exact-search cost as evidence of lower runtime.
The fixture does not measure full intermediate cardinality error after
ANALYZE; that still needs per-stage actual-row instrumentation on both
engines. For a nonzero estimate and actual count, report q-error as
`max(estimate / actual, actual / estimate)`. Empty actual results require
an explicit separate bucket (or a documented smoothing convention), not
silent omission from the q-error report.

## Acceptance work for this track

1. Preserve the regression fixture's inner-cycle, disconnected, CROSS,
   LEFT, NATURAL/USING, and correlated/empty cases on both engines.
2. Compare beam and exact search only within the scoped flat INNER cases;
   assert identical candidate inputs and legal-extension rules before
   drawing optimizer-quality conclusions.
3. Add stage-matched actual/estimated intermediate rows to the workload
   capture. Report q-error by join depth, graph class, engine, and empty
   result status. Current EQP exposes per-loop estimates, not enough to
   prove the full intermediate-row gate.
4. Expand legality only with a separate semantic fixture and explicit
   dependency model for each deferred join shape.

The fixture passed under `test/test-run.py --suite sql-tap --builddir
build-clang19 dp_join_legality` on both memtx and Vinyl on 2026-10-04.
The workload-level intermediate-cardinality report is not yet proven by
this audit.
