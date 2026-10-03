# SQL access-cost calibration probe

This probe measures **observed warm-cache prepared SQL execution**, not a
storage-engine primitive or a calibrated planner coefficient. It uses the same
4096-row SQL fixture on memtx and Vinyl: a primary key, a non-unique secondary
key with 16 rows per value, and a payload column. `INDEXED BY` fixes the
access path; each JSONL row includes the actual `EXPLAIN QUERY PLAN` text and
the checked number of returned rows. Cases are primary-key point lookup,
secondary equality projected as key-only and payload, secondary range, and
primary scan. Preparation and fixture creation are outside the timer; result
materialization is inside it. One execution per case is warmed first, and
engine order alternates between repetitions.

From the repository root, using a **current binary built from the source being
evaluated**:

```sh
python3 -B tools/sql_cost/test_access_cost_report.py
bench_dir=$(mktemp -d /tmp/sql-cost.XXXXXX)
cd "$bench_dir"
SQL_COST_ROWS=4096 SQL_COST_REPEATS=7 SQL_COST_ITERATIONS=300 \
  /absolute/path/to/build/src/tarantool \
  /absolute/path/to/tools/sql_cost/access_cost_bench.lua > observations.jsonl
python3 -B /absolute/path/to/tools/sql_cost/access_cost_report.py \
  observations.jsonl > report.json
```

Retain `git rev-parse HEAD`, `git status --short`, binary `--version`, binary
SHA-256, build configuration, CPU/storage configuration, and the two output
files with any reported measurements. Run on an otherwise idle host and repeat
on a second day/machine before treating small differences as meaningful. For
Vinyl, this run does **not** control LSM level count, compaction, persistence,
or OS cache. A cold/read-amplification experiment needs its own fixture and
cache-state metadata. The `secondary_payload` name deliberately does not claim
that EXPLAIN's “COVERING INDEX” label identifies the number of engine reads.

The report checks seven-way pairing, fixture/version consistency, stable
SQL/plan/result shapes, timing arithmetic, and a minimum of five repeats. It
prints per-access engine medians and paired ratios. These ratios compare full
SQL calls and include executor/return overhead; they cannot be copied into
`WhereLoop.rRun`. The current planner uses shared `LogEst` formulas for both
engines, including a generic secondary-index penalty. An engine-aware cost
change requires a separate same-binary A/B of chosen plans, planning time,
and actual execution time on the reviewed JOIN workload. This probe only
supplies a reproducible starting point for selecting candidate coefficients.

The initial smoke run (`HEAD` 231884c8af, binary version
`1.3.2-19597-g1d854aadfa`, 4096 rows, seven repeats) gave median
Vinyl/memtx ratios of 1.98 for primary point, 3.19 for primary scan, and
5.04–5.80 for secondary accesses. The binary's embedded revision did **not**
match HEAD, so this run is explicitly *not* decision-grade evidence for the
current source. Its observations and report are local at
`/tmp/sql-cost-run.5aHLkX/`; regenerate with a matching clean build before
calibration or any production ranking change.
