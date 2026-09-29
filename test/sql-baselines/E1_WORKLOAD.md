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

`e1_sql_producer.py` is a reproducible TEST_BUILD pilot of volatile
`ANALYZE table`, not the reviewed M0 analytical corpus. Reconfigure and build
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

The pilot measures six prepared single-table predicates five times per
configuration, records a warmup for each query, and pairs the planner's EXPLAIN
estimate with the actual rows from that same SELECT output. Its explicit
`--allow-statistics-change` analysis compares no installed snapshot against a
named SQL `ANALYZE` collection and records both statistics IDs. The pilot spans
three equality values, an empty equality, and selective/non-selective ranges;
its uniform eight-row fixture and single engine are smoke evidence for the
JSONL producer and stage contract, and a narrow validation of the volatile
collection path. They do not establish skewed MCV quality, corpus q-error
improvement, or E1 acceptance.
