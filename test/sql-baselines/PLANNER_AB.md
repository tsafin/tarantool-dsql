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
`--mode generated|cnp|llvm` selects one execution mode for all four width
captures (default: generated). CnP and LLVM require a binary built with the
corresponding support; native participation is checked by the suite adapter.
The report records the requested mode, dispatcher setting, and JIT flag.
Widths are compared only within a mode; the tool makes no cross-mode parity
claim.

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

### Native-mode width comparisons

The same A/B capture flow can run under CnP or LLVM without changing the
SQL-TAP, SQL-luatest, or normal SQL-suite adapters. For example, a bounded
single-test proof is:

```sh
python3 -B test/sql-baselines/planner_ab.py \
  --repo /home/tsafin/tarantool --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/planner-ab-cnp --test join.test.lua --engine memtx \
  --mode cnp --widths 2,8,16
```

Each invocation holds its mode constant across default, candidate, and both
repeats. This enables per-mode width sensitivity and repeatability evaluation;
it is not cross-mode parity or a performance/plan-quality result.

Bounded native proof, 2026-09-26: source `c40594173ba82432cf5293311c329ebe6ec5e56c`,
binary SHA-256
`7f85e7d52b5becd6d813c2ad2f4eeaeb42889efe54ba245e4bbe0f46c0062cd4`.
The five reviewed join/range SQL-TAP files (`join`, `join2`, `join3`, `join5`,
`where3`) were compared at widths 1/5/10 vs 2/8/16 under both CnP and LLVM,
on memtx and Vinyl. Each engine/mode captured 687 statements in all four
configurations; default-repeat, candidate-repeat, and cross-width strict
comparisons passed 687/687. There were 294 planner-metric snapshots per
engine/mode, with structural width effects on 121 queries. Generated paths
increased from 949,450 to 1,512,106. Reports are local at
`/tmp/tarantool-e1-cnp-join-subset/report.json` and
`/tmp/tarantool-e1-llvm-join-subset/report.json`. This is bounded evidence,
not a full-corpus native-mode run, E1 acceptance, or a runtime benchmark.

### Full reviewed SQL-TAP widths in native modes, 2026-09-27

The full reviewed SQL-TAP corpus was captured with CnP and LLVM separately on
both engines, each at default 1/5/10 and candidate 2/8/16 widths with repeats.
Both mode runs used source HEAD
`7a8d328e1ad5680f230a672f4d3b0a9a8fade1ea` and binary SHA-256
`7f85e7d52b5becd6d813c2ad2f4eeaeb42889efe54ba245e4bbe0f46c0062cd4`.

All within-width repeats passed exactly. Cross-width strict snapshot parity
was false in both modes and engines with the same three explicit EXPLAIN
captures already seen in generated mode: `select6/q96`, `where2/q128`, and
`whereK/q13` (47,943/47,946 memtx and 37,987/37,990 Vinyl snapshots matched).
The SQL-TAP assertions passed in both configurations; no non-EXPLAIN result
regression was observed. The full captures and reports are at
`/tmp/tarantool-e1-full-tap-cnp/report.json` and
`/tmp/tarantool-e1-full-tap-llvm/report.json`.

Planner work increased in both native modes. CnP generated paths rose from
1,019,739 to 1,582,459 on memtx and 1,013,614 to 1,576,334 on Vinyl; summed
`elapsed_us` rose from 71,083 to 150,106 µs and 63,381 to 148,547 µs,
respectively. LLVM generated the same path totals; summed planner time rose
from 71,782 to 164,489 µs on memtx and 75,983 to 161,646 µs on Vinyl. These
are planner-time sums, not execution latency. Native-mode full-corpus
repeatability does not improve the candidate's plan quality, which remains
unmeasured; keep the 1/5/10 production defaults pending an integrated workload
decision.

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

## Full reviewed SQL-TAP width comparison, 2026-09-26

The harness also supports `--full-corpus`, which selects every reviewed
SQL-TAP file eligible for each engine and sizes its query bound from the
policy. This is the full reviewed SQL-TAP planner corpus, not all M0 tests:
the SQL and SQL-luatest suites are not run by this harness. Reproduce with:

```sh
python3 -B test/sql-baselines/planner_ab.py \
  --repo /home/tsafin/tarantool \
  --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/tarantool-e15-full-corpus --widths 2,8,16 --full-corpus
```

The run used the existing binary (SHA-256
`5a3fd28adbff7f1bc17f2b6ea18a917efc34cc9e82ef2af0c209eb1eab90c59f`, version
string `Tarantool 1.3.2-18719-gd8fc1e339b`). It completed all four captures
and both repeats: 232 memtx SQL-TAP files / 47,946 statements per capture and
224 Vinyl files / 37,990 statements per capture. Both within-width repeat
comparisons passed, and planner structure was repeat-stable. Cross-width
strict snapshot parity **did not pass**: each engine had exactly the same
three hard differences (47,943/47,946 memtx snapshots and 37,987/37,990 Vinyl
snapshots matched):

| Capture | Default 1/5/10 | Candidate 2/8/16 | Interpretation |
| --- | --- | --- | --- |
| `select6/q96` | `EXPLAIN SELECT` VDBE rows include `Column(10,1)`, null check, then seek through cursor 11 | Bytecode uses `Column(10,0)`, omits that null check, and seeks through cursor 11 before reading `Column(10,1)` from cursor 10 | Different compiled EXPLAIN program; this capture does not execute the represented SELECT |
| `where2/q128` | `EXPLAIN QUERY PLAN`: index constraints `z=? AND y=? AND x<?`, estimate `~4 rows` | `z=? AND y=? AND x>? AND x<?`, estimate `~6 rows` | Different range description and estimate in diagnostic output |
| `whereK/q13` | `EXPLAIN QUERY PLAN`: narrow `b=? AND c>?` branch (`~2 rows`) plus broad `b>?` branch (`~245760 rows`) | Only the broad `b>?` branch (`~245760 rows`) | Width-sensitive OR-plan output; the paired SQL result assertions pass in both configurations |

These are explicit `EXPLAIN`/`EXPLAIN QUERY PLAN` statements. They are hard
snapshot differences, not observed regressions in the paired SQL result
assertions; no accepted snapshots were changed. The A/B therefore establishes
repeat stability and width sensitivity, but does not pass strict cross-width
parity or establish which plan is better. Aggregate planner totals were the
same on both engines: candidate count −7, fallback count 0, generated
+562,720, dominated +39,843, truncated +495,669, retained +27,208. Both
captures reported `elapsed_us=0`; this provides no latency evidence. The
complete local report is `/tmp/tarantool-e15-full-corpus-retry/report.json`.

### Re-run with monotonic planner timing

After the planner timer was changed to a direct monotonic clock, the same
full reviewed SQL-TAP comparison was rerun from source HEAD
`7122eb2474003803c6f9810913a4419b38361e4b`, using binary SHA-256
`0af2e7313949650106df4404eb6de620414c90f5f5e143596e83b2d07310d337`:

```sh
python3 -B test/sql-baselines/planner_ab.py \
  --repo /home/tsafin/tarantool \
  --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/tarantool-e15-full-corpus-monotonic \
  --widths 2,8,16 --full-corpus
```

All four captures again completed at 47,946 memtx and 37,990 Vinyl statements
per run. Both default/candidate repeat comparisons passed with zero hard or
soft snapshot differences, and planner metrics were structurally repeat-
stable. Strict cross-width parity remains false with the same three explicit
EXPLAIN differences per engine: `select6/q96`, `where2/q128`, and
`whereK/q13`; matched counts remain 47,943/47,946 for memtx and 37,987/37,990
for Vinyl. Their details and classifications are unchanged from the prior
full-corpus run above.

The monotonic planner timer produced nonzero summed `elapsed_us` totals across
successful planner snapshots:

| Engine | Planner snapshots/run | Default | Default repeat | Candidate | Candidate repeat |
| --- | ---: | ---: | ---: | ---: | ---: |
| memtx | 24,263 | 53,882 µs | 55,614 µs | 139,211 µs | 141,950 µs |
| Vinyl | 22,135 | 55,871 µs | 52,376 µs | 140,882 µs | 149,423 µs |

Candidate/default first-run summed planner time is 2.58× for memtx and 2.52×
for Vinyl. The timer is now useful for within-workload planning-cost
comparison; these sums are not end-to-end query latency and do not evaluate
plan quality. The full report is
`/tmp/tarantool-e15-full-corpus-monotonic/report.json`.

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

## SQL-luatest A/B subset support

`planner_ab.py --suite sql-luatest` reuses reviewed per-engine generated-mode
query evidence in `corpus.json` and captures selected tests through the normal
SQL-luatest runner and child-server hook. `--test` selects reviewed
`*_test.lua` basenames; `--full-corpus` selects every reviewed SQL-luatest
test for requested engines. `--runner-repo` may point to a source tree with
initialized `test-run` submodules when the implementation worktree lacks
them. Four captures (two repeats and the A/B pair) and strict snapshot diffs
are used as in SQL-TAP mode. Planner-metric totals are unavailable in this
mode because the luatest adapter does not emit planner-metric manifests.

The report partitions cross-width snapshot differences by captured SQL:
`EXPLAIN` / `EXPLAIN QUERY PLAN` output differences are reported separately
from non-EXPLAIN result or diagnostic differences. Unknown/missing SQL is
unclassified and cannot count as semantic-result parity. Strict snapshot
parity still includes every difference; partitioning is diagnostic, not a
waiver. This compares captured results, not all side effects or test
assertions.

A bounded proof used reviewed `sql-luatest/collation_test.lua` on memtx (3
captured statements) with binary `/tmp/tarantool-m1-build/src/tarantool`
(SHA-256 `5a3fd28adbff7f1bc17f2b6ea18a917efc34cc9e82ef2af0c209eb1eab90c59f`):

```sh
python3 -B test/sql-baselines/planner_ab.py \
  --repo /home/tsafin/tarantool --runner-repo /home/tsafin/tarantool \
  --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/e1-luatest-ab-proof3 \
  --suite sql-luatest --test collation_test.lua --engine memtx \
  --widths 2,8,16
```

Default and candidate repeats and cross-width comparison all passed (3/3
snapshots each); no semantic-result or EXPLAIN differences were observed.
This verifies the single-file memtx adapter path only. It is not full M0
SQL-luatest coverage, does not exercise Vinyl or native dispatchers, and does
not satisfy E1 acceptance. The report remains a local artifact.

### Full reviewed SQL-luatest subset, 2026-09-26

The full reviewed SQL-luatest policy subset was then captured on both engines
with all four default/candidate/repeat runs. The run used source HEAD
`ae2ee3ea5a4b57d8b54c8b6577b95df83b9cd713` and binary SHA-256
`7f85e7d52b5becd6d813c2ad2f4eeaeb42889efe54ba245e4bbe0f46c0062cd4`:

```sh
python3 -B test/sql-baselines/planner_ab.py \
  --repo /home/tsafin/tarantool --runner-repo /home/tsafin/tarantool \
  --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/tarantool-e1-full-luatest-fixed \
  --suite sql-luatest --full-corpus --widths 2,8,16
```

All compared captures passed strict snapshot parity, including default-repeat,
candidate-repeat, and cross-width comparisons. Memtx covered 32 reviewed tests
and 499 statements; Vinyl covered 31 tests and 447 statements (one reviewed
test is not eligible for Vinyl). There were no EXPLAIN, non-EXPLAIN, or
unclassified cross-width differences, so captured-result parity was also
clean. This closes the reviewed SQL-luatest comparison slice only; it does
not include the M0 SQL suite, native dispatchers, runtime query latency, or
plan-quality evaluation. The report is local at
`/tmp/tarantool-e1-full-luatest-fixed/report.json`.

### Normal SQL-suite A/B support

`planner_ab.py --suite sql` now selects reviewed tests from the ordinary
`test/sql` `test-run.py` suite and captures their child-server SQL through the
same normal-runner adapter as SQL-luatest. SQL-suite selection uses the corpus
policy's `evidence.<engine>.capture.audit_queries` count, rather than the
SQL-TAP `captured_queries` or SQL-luatest generated-mode evidence contracts.
`--full-corpus` enumerates reviewed adapter-compatible `.test.lua` tests
eligible for the requested engine; raw `.test.sql` policy entries are omitted
because the child-server adapter requires a Lua test file. `--test` is
restricted to reviewed top-level `.test.lua` basenames.
Each of the default, default-repeat, candidate, and candidate-repeat runs is
strictly diffed. The report additionally partitions EXPLAIN output differences
from other result/diagnostic differences, without weakening strict parity.
Planner-metric aggregation is not available for this adapter; SQL-TAP retains
its own metrics-manifest path.

This adapter emits diagnostics only; it does not change E1 acceptance. A focused
proof captured `sql/autoincrement.test.lua` on memtx (3 statements) using the
binary SHA-256 `7f85e7d52b5becd6d813c2ad2f4eeaeb42889efe54ba245e4bbe0f46c0062cd4`:

```sh
python3 -B test/sql-baselines/planner_ab.py \
  --repo /tmp/tarantool-e1-sql-suite --runner-repo /home/tsafin/tarantool \
  --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/e1-sql-single-test-proof \
  --suite sql --test autoincrement.test.lua --engine memtx \
  --widths 2,8,16
```

The default repeat, candidate repeat, and cross-width diff each passed with
three exact snapshots. This checks the SQL-suite adapter path, not the full
reviewed SQL suite or both-engine scope. The report remains local at
`/tmp/e1-sql-single-test-proof/report.json`.

### Full reviewed SQL `.test.lua` subset, 2026-09-26

The full adapter-compatible SQL suite was captured on both engines using
default/candidate widths and repeats. The run used source HEAD
`9aa7837f4d51ea1654bd2f33c990ea8880152196` and binary SHA-256
`7f85e7d52b5becd6d813c2ad2f4eeaeb42889efe54ba245e4bbe0f46c0062cd4`:

```sh
python3 -B test/sql-baselines/planner_ab.py \
  --repo /home/tsafin/tarantool --runner-repo /home/tsafin/tarantool \
  --binary /tmp/tarantool-m1-build/src/tarantool \
  --out /tmp/tarantool-e1-full-sql-suite-fixed \
  --suite sql --full-corpus --widths 2,8,16
```

Memtx covered 32 `.test.lua` files / 1,078 statements; Vinyl covered 33 files /
1,086 statements. Default-repeat, candidate-repeat, and cross-width strict
diffs all passed exactly on both engines. SQL-vs-EXPLAIN classification found
no width-sensitive differences. Two reviewed raw `.test.sql` policy entries
were omitted because the normal child-server adapter currently requires a
Lua test file. The report is local at
`/tmp/tarantool-e1-full-sql-suite-fixed/report.json`; raw `.test.sql` coverage
and hosted-CI execution remain open.
