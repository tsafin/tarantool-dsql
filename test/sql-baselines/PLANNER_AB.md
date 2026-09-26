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
