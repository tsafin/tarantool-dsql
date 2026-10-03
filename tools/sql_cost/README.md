# SQL access-cost calibration probe

This probe measures **prepared SQL execution**, not a storage-engine primitive
or a production planner coefficient. It uses a 4096-row SQL fixture on memtx
and Vinyl: a primary key, a non-unique secondary key with 16 rows per value,
and a payload column. `INDEXED BY` fixes the path, while the observation keeps
the actual `EXPLAIN QUERY PLAN` and checked output row count. Preparation and
fixture creation are outside the timer; result materialization is inside it.
Engine order alternates between repetitions.

`primary_point` and the secondary equality cases reuse one key per timed
batch. `primary_cycling` and `secondary_cycling` change the parameter every
execution, approximating repeated probes on the inner side of a nested-loop
JOIN. These are still *single-table* SQL calls, not a measured JOIN operator.

## Run

From the repository root, with a matching binary and no uncommitted `src/`
changes:

```sh
python3 -B tools/sql_cost/test_access_cost_report.py
python3 -B tools/sql_cost/run_access_cost.py \
  --binary build-jit-clang19-debug/src/tarantool \
  --out-dir /tmp/sql-cost-memory-run \
  --storage-state memory
python3 -B tools/sql_cost/run_access_cost.py \
  --binary build-jit-clang19-debug/src/tarantool \
  --out-dir /tmp/sql-cost-dumped-run \
  --storage-state dumped
```

Use fresh output directories. The runner verifies that the binary's embedded
Git revision has the same committed `src/` tree as HEAD, rejects dirty `src/`,
records source and binary hashes, and writes observations, validated report,
and manifest. Keep all three artifacts and record host CPU/storage details.
The default is seven paired repetitions; the analyzer requires at least five.

`memory` measures newly inserted Vinyl data before a dump. `dumped` calls
`box.snapshot()` and waits until both Vinyl indexes have runs. Every Vinyl
observation includes per-index run count and deltas for disk pages, disk
lookups, cache lookups/gets, and memory gets. A single warmup execution precedes
each timed batch. The OS page cache and Vinyl cache are **not reset**; the
JSONL deliberately labels cache state `uncontrolled`, even for the fixed-key
cases. `secondary_payload` does not claim that EXPLAIN's “COVERING INDEX” label
identifies how many storage reads occurred.

The report checks pairing, fixture/version/source/binary/run provenance,
stable SQL/plan/result shapes, elapsed-time arithmetic, and a minimum of five
repeats. It prints medians and per-execution Vinyl read counters. Ratios include
SQL executor and result-return overhead. They cannot be copied directly into
`WhereLoop.rRun`, which uses `LogEst` units and combines multiple terms.

## Initial candidate coefficient grid (not accepted)

An exploratory local run with 4096 rows, seven repeats, and a binary reporting
revision `e688db7ff9` produced the following median Vinyl/memtx ratios. It was
launched directly while the shared source tree was changing, without the strict
runner's provenance check; its `binary_sha256` field is a diagnostic placeholder.
These numbers are a **candidate A/B grid only**, not decision-grade calibration.

| Access shape | Memory | Dumped | Rounded candidate multiplier | `LogEst` delta for A/B |
| --- | ---: | ---: | ---: | ---: |
| Primary point / cycling | 2.1 / 2.1 | 1.8 / 2.0 | 2× | +10 |
| Primary full scan | 3.3 | 2.8 | 3× | +16 |
| Secondary equality, key/payload/cycling | 4.4–5.7 | 4.6–5.7 | 5× | +23 |
| Secondary range, 16 rows | 5.5 | 8.6 | 6× | +26 |

Local diagnostic artifacts are `/tmp/sql-cost-memory.CAHYXE/` and
`/tmp/sql-cost-dumped.ZhdIS2/`. The dumped secondary range recorded a median
one disk page per SQL execution; most other dumped cases recorded zero, so one
cannot treat the rounded factors as stable I/O prices. In particular, a
single-run LSM does not characterize read amplification from multiple levels.
Repeat with a clean matching source/binary, multiple fixture sizes and LSM
topologies, and a second host before narrowing coefficients. Then compare
candidate vs baseline **on the same binary and statistics** for chosen JOIN
plans, planning time, and execution time. Do not change production ranking
based on this probe alone.
