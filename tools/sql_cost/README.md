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
The expanded probe also forces primary/secondary paths for four equivalent
result sets: selective equality, a 16-row two-sided range, a narrow one-sided
tail, and a broad half-table one-sided range.
It hashes the checked output of each pair and records the unforced broad-range
EXPLAIN plan. Their per-execution microseconds are a common *empirical SQL
operation* unit, not yet a common planner cost formula.

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
python3 -B tools/sql_cost/run_access_cost.py \
  --binary build-jit-clang19-debug/src/tarantool \
  --out-dir /tmp/sql-cost-multirun-run \
  --storage-state multi_run
```

Use fresh output directories. The runner verifies that the binary's embedded
Git revision has the same committed `src/` tree as HEAD, rejects dirty `src/`,
records source and binary hashes, and writes observations, validated report,
and manifest. Keep all three artifacts and record host CPU/storage details.
The default is seven paired repetitions; the analyzer requires at least five.

`memory` measures newly inserted Vinyl data before a dump. `dumped` calls
`box.snapshot()` and waits until both Vinyl indexes have runs. `multi_run`
makes a large dump followed by a much smaller overlapping dump,
then requires at least two physical runs in both Vinyl indexes; this avoids
mistaking scheduled dumps for a stable multi-run topology. Every Vinyl
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

## Matched-source repeat (2026-10-04)

The strict runner completed both modes against the binary built from
`524fa34855`: `/tmp/sql-cost-strict-524fa34855-memory/` and
`/tmp/sql-cost-strict-524fa34855-dumped/`. Each manifest records the binary
hash and probe revision, and each report validates seven paired repetitions.
Median Vinyl/memtx ratios were 2.04×/1.88× for fixed primary points,
4.02×/3.87× for primary scans, 5.21×/5.60× for secondary payload equality,
and 5.81×/9.37× for a 16-row secondary range (memory/dumped respectively).
Both dumped indexes had a run, but the median timed disk-page delta was zero
except for the secondary range (one page per execution). This corroborates
that the workload mostly measures cached SQL execution, not general Vinyl
disk or multi-run LSM cost. Keep the candidate coefficient grid experimental;
no production `WhereLoop` coefficient changes follow from these results.

## Equivalent-path ranking pilot (2026-10-04)

The expanded strict captures at source `1a3b839c72` are
`/tmp/sql-cost-tail-memory-24020e4952/`,
`/tmp/sql-cost-tail-dumped-24020e4952/`, and
`/tmp/sql-cost-tail-multirun-24020e4952/`. Each has seven paired
repetitions, output hashes for equivalent paths, a binary hash, and observed
Vinyl topology. In the two-run state, the median *secondary/primary* SQL-time
ratios are approximately 0.020 (memtx) and 0.013 (Vinyl) for selective
equality, 0.022 and 0.028 for the 16-row two-sided range, 0.016 and 0.027
for the one-sided tail, but **0.47 and 1.52** for
the broad half-table range. The broad secondary path is faster on memtx in
six of seven pairs and slower on Vinyl in all seven. Vinyl's broad secondary
path reads a median three disk pages and makes four disk lookups per execution;
its forced primary counterpart reports zero disk pages in this warm-cache
fixture. The unforced broad plan chooses the secondary index on *both*
engines. This is a reproducible local ranking discrepancy, not a proof of
production-wide regression.
The unforced tail and broad queries use the **same SQL shape** with different
bound values, yet EXPLAIN gives both the same secondary path and the same
~262144-row estimate. On Vinyl the secondary path wins for the 16-row tail
in all seven pairs but loses for the 2048-row broad range in all seven. This
is why a value-independent secondary penalty cannot choose both correctly.

A uniform secondary-range penalty was tested locally at 8, 16, 24, and 40
legacy `LogEst` units. It did not switch the broad Vinyl plan to the primary
path: the planner moved between secondary candidate variants while the
parameterized broad and narrow ranges shared coarse selectivity assumptions.
That experimental code was **not retained**. Next, derive range selectivity
from statistics/bounds and validate engine-specific path prices against a
reviewed JOIN workload; a single global multiplier cannot fix this evidence.

## Empirical common-unit fit (experimental)

`calibrate_access_cost.py` fits a tiny engine-specific model in measured
microseconds per prepared, materialized SQL operation. For this fixed 4096-row
fixture, it uses
`primary_scan_base + primary_output_rate × (output_rows - 16)` and
`secondary_startup + secondary_output_rate × (output_rows - 16)`.
`primary_filtered`/`primary_scan` and
`secondary_payload`/`secondary_broad` determine the two coefficients per
engine. It checks manifest/binary/benchmark hashes before evaluating the
fitted model on *different storage states*:

```sh
python3 -B tools/sql_cost/test_calibrate_access_cost.py
python3 -B tools/sql_cost/calibrate_access_cost.py \
  --train /tmp/sql-cost-tail-memory-24020e4952 \
  --validate /tmp/sql-cost-tail-dumped-24020e4952 \
  --validate /tmp/sql-cost-tail-multirun-24020e4952
```

The memory-state fit estimates per-returned-row secondary work at roughly
approximately 0.7 µs on memtx versus 8–9 µs on Vinyl. It predicts all eight equivalent-path
rankings in each held-out dumped and two-run state, including that Vinyl's
broad secondary path loses to the primary scan. This is a fixture-local
validation, **not** an accepted optimizer cost function: broad secondary
timing was used in fitting, so output-cardinality generalization is untested;
the narrow Vinyl tail magnitude is underpredicted after dumps, despite the
correct ranking;
OS cache is uncontrolled, the work is not a JOIN inner-loop operator, and
`WhereLoop` still lacks reliable bound-specific range cardinalities. The
next A/B must validate new fixture sizes, JOIN order and runtime on the same
binary/statistics before any coefficient is translated to `LogEst`.

`calibrate_access_scale.py` adds a separate size-transfer check. It fits
engine-specific scan-input, primary-output, secondary-startup, and
secondary-output rates from the 2048- and 4096-row memory-state captures, then
tests the untouched 8192-row capture:

```sh
python3 -B tools/sql_cost/test_calibrate_access_scale.py
python3 -B tools/sql_cost/calibrate_access_scale.py \
  --train /tmp/sql-cost-tail-memory-2048 \
  --train /tmp/sql-cost-tail-memory-24020e4952 \
  --validate /tmp/sql-cost-tail-memory-8192
```

It preserves all eight path rankings at 8192 rows. Fitted secondary work is
about 0.75 µs per returned row for memtx and 8.76 µs for Vinyl; the held-out
Vinyl broad ratio is predicted at 1.49 versus 1.61 observed. This tests one
additional cardinality on the same host, not arbitrary selectivities, JOIN inner-loop
reuse, cold I/O, or an accepted production cost function. Training and
validation require the same binary and benchmark revision; only row count is
allowed to differ.

## Integer LogEst translation and offline A/B

`evaluate_logest_ab.py` is the next conservative bridge toward the production
cost domain. It fits the size-transfer model, quantizes each coefficient into
an explicit 0.01 µs work unit, forms integer primary/secondary work totals,
and runs those totals through a Python equivalent of `sqlLogEst()`. It changes
no C code and no planner default:

```sh
python3 -B tools/sql_cost/test_evaluate_logest_ab.py
python3 -B tools/sql_cost/evaluate_logest_ab.py \
  --train /tmp/sql-cost-tail-memory-2048 \
  --train /tmp/sql-cost-tail-memory-24020e4952 \
  --validate /tmp/sql-cost-tail-memory-8192 \
  --out /tmp/sql-cost-logest-ab.json
```

On the matching-source captures above, the integer candidate preserves all
8/8 held-out forced-path rankings. The capture records the production
planner's unforced choice only for the identical-shape one-sided `tail` and
`broad` cases, so the direct A/B covers four engine/case pairs: production
matches 3/4, while the candidate matches 4/4. The difference is the broad
Vinyl case: production selects the secondary index, but the primary scan is
observed faster and the integer candidate selects it (candidate LogEst 212
versus 217).

This is still **offline evidence only**. The candidate is evaluated with the
known output cardinality (16 or half the fixture), whereas the production
planner assigns the narrow and broad bound the same coarse ~262144-row
estimate. The fitted startup also contains SQL execution/materialization
overhead, and the arbitrary common work unit has not been calibrated for
composition across JOIN depths. The next gate is a same-binary A/B using
planner-estimated cardinalities and repeated inner-loop probes on a reviewed
JOIN workload. Until that passes, the report must not be read as approval of
these coefficients or an engine-specific production formula.
