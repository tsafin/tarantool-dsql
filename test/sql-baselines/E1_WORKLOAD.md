# E1/GATE estimate-quality and runtime workload

This is a measurement contract for the post-integration E1/GATE decision. It
does not accept a planner, select width defaults, define production thresholds,
or replace M0 parity. `e1_measure.py` analyzes observations; it does not run SQL
or manufacture planner estimates. The current planner does not yet expose
stage-matched actual cardinalities for the integrated M3/S2 path, so a producer
must wait for those instrumentation interfaces.

## Workload contract

The reviewed workload should contain deterministic analytical queries with
declared result checks and named cardinality stages. Include at least selective
and non-selective predicates, uniform and skewed values, correlated join keys,
multiway joins, range/equality combinations, empty results, and non-empty
results. Preserve the data generator, seed, DDL, statistics state, SQL text,
and query/stage IDs with the workload revision. Exercise both memtx and Vinyl
where the query is supported, and each dispatcher/engine combination under
consideration. A workload is not accepted just because it contains EXPLAIN
queries: actual work must execute.

For every measured query execution, capture one JSON object in JSON Lines
format. `data_sha256` identifies the generated data/DDL fixture and
`statistics_id` identifies the ANALYZE/statistics state. Example:

```json
{"schema_version":1,"workload_id":"analytic-v1","query_id":"join-skew-01","engine":"memtx","dispatcher":"generated","configuration":"default","source_commit":"0123456789abcdef","binary_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","data_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","statistics_id":"stats-v1-seed-17","repeat":1,"warmup":false,"elapsed_us":482,"cardinalities":[{"stage_id":"join-output","estimated_rows":120,"actual_rows":96}]}
```

`actual_rows` and `estimated_rows` must describe the same named plan stage and
the same semantics (for example, rows emitted by a join operator), not a
planner estimate for an intermediate relation compared to the final SQL result
after grouping or LIMIT. The integrated executor/planner instrumentation must
make this correspondence auditable. `elapsed_us` is wall-clock end-to-end
statement execution time, including result production, but excludes setup,
prepare, and warmup; if prepare cost is in scope, record it as a separately
specified measurement rather than silently mixing it into execution time.

For each engine/dispatcher/configuration, retain warmups but exclude them from
summaries. Execute at least five measured repetitions per query/configuration;
randomize or alternate configuration order to reduce temporal bias. Standard
planner A/B comparisons must keep source commit, binary hash, data hash, and
statistics ID identical across paired configurations. A dedicated statistics
provider comparison may intentionally compare different statistics states; use
`--allow-statistics-change` only for that experiment. It still requires source,
binary, data, actual rows, and stage IDs to match, and reports each configuration's
statistics ID. Do not use this option for solver-width or execution-mode A/B.
Do not treat concurrent runs as independent latency repetitions. A report must
include query-level distributions as well as aggregate distributions so a
small number of expensive or badly estimated queries is not hidden by pooling.

## Analyzer behavior

Run locally with:

```sh
python3 -B test/sql-baselines/test_e1_measure.py
python3 -B test/sql-baselines/e1_measure.py observations.jsonl \
  --baseline default --candidate candidate --out report.json
# Only for an explicit no-stats vs stats-provider comparison:
python3 -B test/sql-baselines/e1_measure.py observations.jsonl \
  --baseline no-stats --candidate live-stats --allow-statistics-change \
  --out stats-report.json
```

The analyzer validates schema/provenance, rejects duplicate or unpaired query
repetitions, excludes warmups, and requires a query/configuration to report the
same cardinality-stage inventory and actual row counts on every repetition.
Each paired
baseline/candidate execution must also produce identical actual row counts for
every named stage. This
prevents comparisons across runs whose executed workload changed despite
sharing query and stage IDs. It also requires at least five measured
repetitions per query/configuration, excluding warmups. It reports
median/p95/p99 execution time and
median/p95/max/geometric-mean q-error per workload, engine, dispatcher, and
configuration. It also reports paired candidate/default latency ratios. For
positive estimates and actuals, q-error is
`max(estimate / actual, actual / estimate)`. Two zero cardinalities have q-error
1; exactly one zero is an unbounded error, reported separately and never
discarded into a finite percentile. Summaries include per-query group keys;
acceptance review should inspect their long tails and zero mismatches, not only
the aggregate.

The tool deliberately sets no “good enough” q-error or latency threshold. The
workload, metric thresholds, unacceptable regressions, and trade-off for any
planner-time increase require review before collecting decision-grade results.

## Bounded-DP multiway JOIN execution pilot

`e1_join_run.py` drives `e1_join_workload.lua` in a fresh Tarantool process for
each engine and solver-width triple. It compares the current `(1,5,10)` widths
with `(2,8,16)` on the same deterministic, volatile-ANALYZE fixture. The
workload executes seven SELECTs: hot and rare two-way equijoins, filtered and
range/equality three-way joins, a selective four-way join, an empty three-way
join, and a LEFT JOIN. The fixture asserts expected output row counts. Each
query is prepared once, warmed up three times, then executed five measured
times; query order alternates within each process. Memtx and Vinyl reverse
configuration process order. All timing runs are serial, not concurrent.

```sh
python3 -B test/sql-baselines/test_e1_join_run.py
cmake --build build-jit-clang19-debug --target tarantool -- -j4
cmake --build build-jit-clang19-debug --target sql_stats_snapshot_test -- -j4
python3 -B test/sql-baselines/e1_join_run.py \
  --binary build-jit-clang19-debug/src/tarantool \
  --out /tmp/e1-join-width-run
# Optional exhaustive comparison for eligible 2–4-way INNER JOINs:
python3 -B test/sql-baselines/e1_join_run.py \
  --binary build-jit-clang19-debug/src/tarantool \
  --include-oracle --out /tmp/e1-join-oracle-run
```

The runner requires the binary's embedded commit to match HEAD and the SQL and
producer sources to be clean. `--allow-stale-binary` is only for local debugging
and sets `decision_grade_provenance=false`. The output contains one JSONL per
engine/configuration and `report.json` with per-query latency distributions,
paired ratios, prepare-time observations, plan fingerprints, selected WHERE-path
output estimates, actual row counts, stage-matched q-error, and result
fingerprints. It rejects changed results, repetition gaps, mixed
statistics/fixture/binary provenance, and plan instability within a run. The
fixture code SHA-256 is the data generator/DDL revision identifier; the
statistics ID identifies volatile ANALYZE of that same fixture. Timing includes
result materialization, excludes preparation and warmup. The one-off
`prepare_us` values include parsing and compilation and are **not** a planner-
time distribution.

EXPLAIN QUERY PLAN exposes per-loop estimates, not a semantically matched
estimate for each JOIN output. The TEST_BUILD adapter instead compiles the
same SQL text and reads the selected `WhereInfo.nRowOut` at the top-level
`sqlWhereBegin()` call. The capture is valid only for multi-relation,
ungrouped, unlimited, non-DISTINCT SELECT output: its post-WHERE cardinality
is the SELECT output cardinality. The fixture explicitly checks that grouped,
limited, and single-table SELECTs do not expose this estimate. Every measured
row includes one `join-output` cardinality stage; the strict `e1_measure.py`
analyzer supplies per-query and aggregate q-error under `estimate_quality` in
the report. The selected estimate is not an EXPLAIN per-loop count, and it is
not an executor-observed intermediate JOIN-prefix count. Prefix-stage
estimates and actuals remain uninstrumented.

The optional `exact-oracle` process enables
`SQL_PATH_SOLVER_ORACLE_MAX_RELATIONS=4`; all baseline/candidate processes
explicitly clear that override. Oracle observations include only eligible
INNER JOINs; LEFT JOIN is excluded because `where.c` uses its bounded solver
for that shape. The reporter verifies oracle output and statistics provenance
against the same query under the default beam, and records plan fingerprints,
final-output q-error, prepare observations, and execution latency under
`exact_oracle` / `oracle_estimate_quality`. This comparison is exact only in
the oracle's bounded left-deep search space, not over bushy or alternative
physical join algorithms.

Likewise, this small synthetic pilot does not set or satisfy production latency
acceptance thresholds. A non-decision-grade local instrumentation smoke run is
`/tmp/e1-join-qerror-pilot-20261004-b/report.json`; it finds a hot two-way JOIN
with estimated 22 versus actual 1820 output rows (q-error about 82.7) on both
engines. This is evidence that JOIN selectivity calibration matters, not a
conclusion about the best width. Rerun from a clean, matching build before
using timings or q-error for a width-default decision.

`e1_sql_producer.py` is a reproducible TEST_BUILD pilot of volatile
`ANALYZE table`, not the reviewed M0 analytical corpus. It writes a second,
same-statistics report comparing the legacy route with the enabled M3 route;
its `.planner.jsonl` contains only those two configurations so the strict
provenance validator does not mix in `no-stats`. Reconfigure and build
`tarantool` and
`sql_stats_snapshot_test` from a clean SQL/test source tree, then capture:
The runner refuses dirty SQL/test sources and verifies the binary's embedded
revision matches `HEAD` before recording its source commit and SHA-256.

```sh
cmake -S . -B build-jit-clang19-debug
cmake --build build-jit-clang19-debug --target tarantool -- -j4
cmake --build build-jit-clang19-debug \
  --target sql_stats_snapshot_test -- -j4
python3 -B test/sql-baselines/e1_sql_producer.py \
  --build-dir build-jit-clang19-debug \
  --out /tmp/sql-stats-live.jsonl
```

The pilot measures fourteen prepared single-table predicates five times per
configuration, records a warmup for each query, and pairs the planner's EXPLAIN
estimate with the actual rows from that same SELECT output. Its explicit
`--allow-statistics-change` analysis compares no installed snapshot against a
named SQL `ANALYZE` collection and records both statistics IDs. The pilot spans
three equality values, an empty equality, selective/non-selective ranges, and a
separate skewed eleven-row fixture on both memtx and Vinyl, with a hot value,
tail values, a range, and an empty equality. This exercises both storage
samplers and the volatile literal-equality MCV planner path. The current-source
pilot at `/tmp/e1-mcv-current-20260930.jsonl` reports q-error 1.0 for both hot
and tail equality after ANALYZE on memtx and Vinyl (versus 1.25 and 10 without
statistics). This is focused smoke evidence, not reviewed-corpus q-error
improvement or E1 acceptance; the workload is tiny and no acceptance threshold
has been reviewed.

The three-configuration run at source `8e49433dac` is
`/tmp/e1-m3-current-8e49433dac.jsonl`. The same-statistics M3 comparison is
`/tmp/e1-m3-current-8e49433dac.planner.report.json` (paired input:
`/tmp/e1-m3-current-8e49433dac.planner.jsonl`). Legacy+ANALYZE and
M3+ANALYZE each have median finite q-error 1.0 on both engines; ten memtx and
five Vinyl measured empty-result executions per configuration have unbounded
q-error. Paired median M3/legacy latency ratios are 1.52 on memtx and 1.57 on
Vinyl, with median execution times 8/10.5 µs and 14/22 µs respectively.
These tiny pilot timings are not latency acceptance evidence and do not
justify a planner-width or route-default change.
