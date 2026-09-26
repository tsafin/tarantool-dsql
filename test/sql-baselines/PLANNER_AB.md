# Offline bounded-DP width evaluation (E1.5)

Run from any directory, using a binary built with bounded-path metrics:

```sh
python3 -B test/sql-baselines/test_planner_ab.py
python3 -B test/sql-baselines/planner_ab.py \
  --repo /home/tsafin/tarantool \
  --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/planner-ab-capture --widths 2,8,16
```

The output directory must be empty. No hosted CI, network access, baseline
replacement, or database writes outside temporary capture directories are
required. `--test` may repeat, selecting reviewed top-level SQL-TAP basenames;
`--engine` may select memtx or Vinyl. Reviewed evidence bounds the selected
subset to `--max-queries` (default 1000) per engine per run, and the actual
capture count must agree. SQL-TAP captures are validated with the existing
manifest/snapshot validator and compared with the existing diff tool.

The default subset is join, join2, join3, join5, and where3: 687 SQL statements
per engine. It includes multi-relation joins, indexed WHERE queries, outer
joins, aggregates, and subqueries. Each engine runs generated dispatch under
default widths **1/5/10** and candidate widths **2/8/16**, with independent
repeats of both configurations. All three width environment variables are
explicit, so inherited settings cannot contaminate the default run.

`report.json` records the source commit, exact test/configuration scope,
complete snapshot comparison results, repeat planner-structure stability,
path-class distribution, metric totals/deltas, and query-level structural
deltas. A capture/TAP failure records its exact configuration and test before
exiting. A snapshot divergence is reported and returns nonzero. Diagnostic
metric changes do not change corpus acceptance policy.

## Local result, 2026-09-26

Code source `5ee3d4dff6`; final capture source identity `ecada1b8ff` differs
only by the following documentation commit. Binary `/tmp/tarantool-m1-build/src/tarantool` (built from
the instrumentation working tree; its version string names an earlier HEAD).
Both engines completed all four 687-statement captures. Default repeat,
candidate repeat, and A/B had zero snapshot differences. Each engine measured
296 SELECT/WITH planner snapshots; 121 queries had structural metric deltas.
The two engines produced the same aggregate structural counts:

| Metric | Default 1/5/10 | Candidate 2/8/16 | Delta |
| --- | ---: | ---: | ---: |
| candidate_count | 10,553 | 10,553 | 0 |
| fallback_count | 285 | 285 | 0 |
| generated | 949,462 | 1,512,118 | +562,656 |
| dominated | 65,676 | 105,450 | +39,774 |
| truncated | 840,791 | 1,339,443 | +498,652 |
| retained | 42,995 | 67,225 | +24,230 |

Both configuration repeats had identical structural planner metrics. Path
classes were 11 `current_where_c` and 285 `fallback` per engine under each
configuration. Raw local evidence:
`/tmp/tarantool-e15-ab-results-final/report.json`. More retained
and generated paths demonstrate a width effect, not improved plan quality.
Elapsed-time counters were zero in this local sample; no latency conclusion
is justified. Measurements describe compile-time EXPLAIN snapshots, not
native dispatcher execution or runtime query performance.

The same four-run A/B was repeated after the capture-harness and zero-source
fallback updates at source commit `4d5ca37e5dbcf29332fa0aa1171b40ae7f7d35d6`.
All 687 statements
per engine/run again passed default-repeat, candidate-repeat, and cross-width
snapshot comparison (zero hard or soft drift). Each engine measured 294
successful planner snapshots; the two engines had identical metrics:

| Metric | Default 1/5/10 | Candidate 2/8/16 | Delta |
| --- | ---: | ---: | ---: |
| candidate_count | 10,539 | 10,539 | 0 |
| fallback_count | 283 | 283 | 0 |
| generated | 949,450 | 1,512,106 | +562,656 |
| dominated | 65,670 | 105,444 | +39,774 |
| truncated | 840,791 | 1,339,443 | +498,652 |
| retained | 42,989 | 67,219 | +24,230 |

Both repeats remained structurally stable. The 294 path classes per engine
were 11 `current_where_c` and 283 `fallback`. The binary SHA-256 was
`f8635866e03c5162df9bd24c9c4d73df12b7106e7942b10abb7f1fbcf7921ab4`;
full report: `/tmp/tarantool-e15-4d5ca/report.json`. This updates the
bounded-subset evidence, not the full-corpus or plan-quality gate.

An expanded exploratory selection added join4, join6, where5, whereF,
whereI, whereK, whereA, and whereC (962 statements per engine). All four
captures completed; 434 SELECT/WITH planner snapshots were measured per
engine. Repeat comparisons were clean, but default-vs-candidate comparison
reported one difference per engine: `whereK/q13`, an `EXPLAIN QUERY PLAN`
result whose OR branches are ordered differently at widths 1/5/10 and 2/8/16.
The harness
classifies this expected-plan-output change as a hard mismatch, so the
expanded run is not a parity pass. It is not evidence of a changed underlying
SQL result: both width configurations completed the `whereK` SQL-TAP file,
whose paired SELECT assertion returned its expected rows. The difference is
confined to the `EXPLAIN QUERY PLAN` output captured for comparison.
The candidate run also observed seven fewer path candidates over this broader
sample, while generated, dominated, truncated, and retained totals moved as
expected with the wider beam. No other compared output differed. Evidence is
at `/tmp/tarantool-e15-more.waRTns/capture/report.json` (local, uncommitted).

An exploratory **1/1/1** run changed where3 EXPLAIN QUERY PLAN join-order
expectations (five TAP failures, including expected tB/tC/tA/tD versus
tA/tB/tC/tD). This is a plan-output assertion difference, not an observed
semantic result-row regression. The current harness rejects a failed TAP run
before writing accepted snapshots, so that configuration cannot establish a
separate SQL-result parity gate through this corpus file.

where4 was intentionally omitted: its default capture encounters an unrelated
EXPLAIN pointer-normalization error (`snapshot.lua:161`, void pointer lacks
`match`). Neither tests nor parser/normalizer code were changed to hide it.
This is a bounded diagnostic evaluation, not a full-corpus or hosted-CI pass.
