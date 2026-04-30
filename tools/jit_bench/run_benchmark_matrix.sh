#!/usr/bin/env bash
# Run the SQL JIT benchmark matrix for interpreter, LLVM MCJIT, and CnP.
#
# Usage:
#   cd /path/to/build-jit-relwithdebinfo
#   bash /path/to/tools/jit_bench/run_benchmark_matrix.sh
#
# Output: JSON results files in $OUTDIR (default: current directory)
# Results are named: bench_<label>.json
#
# Each config/workload pair runs in a separate Tarantool process and removes
# *.snap/*.xlog before launch. This avoids cross-workload runtime state from
# earlier workloads skewing the later scan-heavy cases.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_SCRIPT="$SCRIPT_DIR/sql_llvm_mcjit_benchmark.lua"

TARANTOOL="${TARANTOOL:-./src/tarantool}"
OUTDIR="${OUTDIR:-.}"
RUNS="${BENCH_RUNS:-3}"
ONLY_WORKLOAD="${BENCH_ONLY_WORKLOAD:-}"
MATRIX_WORKLOADS="${BENCH_MATRIX_WORKLOADS:-}"
TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/sql-bench-matrix.XXXXXX")
trap 'rm -rf "$TMPDIR"' EXIT

declare -a WORKLOADS
if [[ -n "$ONLY_WORKLOAD" ]]; then
    WORKLOADS=("$ONLY_WORKLOAD")
elif [[ -n "$MATRIX_WORKLOADS" ]]; then
    # BENCH_MATRIX_WORKLOADS is a space-separated list.
    read -r -a WORKLOADS <<<"$MATRIX_WORKLOADS"
else
    WORKLOADS=(
        tiny_const
        hot_expr
        point_lookup
        bitwise_mix
        agg_scan
        builtin_scan
    )
fi

# Iteration counts: environment variables take precedence, then defaults.
# automatic_execute defaults match prepared_execute because the auto stmt cache
# (added in M4) means MCJIT/CnP no longer re-compile every iteration.
export BENCH_PREPARE_ITERS_TINY=${BENCH_PREPARE_ITERS_TINY:-500}
export BENCH_PREPARE_ITERS_HOT=${BENCH_PREPARE_ITERS_HOT:-500}
export BENCH_PREPARE_ITERS_POINT=${BENCH_PREPARE_ITERS_POINT:-250}
export BENCH_EXEC_ITERS_TINY=${BENCH_EXEC_ITERS_TINY:-200000}
export BENCH_EXEC_ITERS_HOT=${BENCH_EXEC_ITERS_HOT:-200000}
export BENCH_EXEC_ITERS_POINT=${BENCH_EXEC_ITERS_POINT:-100000}
export BENCH_AUTO_ITERS_TINY=${BENCH_AUTO_ITERS_TINY:-200000}
export BENCH_AUTO_ITERS_HOT=${BENCH_AUTO_ITERS_HOT:-200000}
export BENCH_AUTO_ITERS_POINT=${BENCH_AUTO_ITERS_POINT:-100000}

export BENCH_RUNS="$RUNS"

run_workload() {
    local label="$1"
    local dispatcher="$2"
    local jit_enable="$3"
    local workload="$4"
    local outfile="$TMPDIR/bench_${label}_${workload}.json"

    echo "[run_matrix] --- workload $workload ---"
    rm -f ./*.snap ./*.xlog

    VDBE_DISPATCHER="$dispatcher" \
    SQL_JIT_ENABLE="$jit_enable" \
    BENCH_LABEL="$label" \
    BENCH_ONLY_WORKLOAD="$workload" \
    "$TARANTOOL" "$BENCH_SCRIPT" > "$outfile"
}

merge_results() {
    local label="$1"
    local outfile="$OUTDIR/bench_${label}.json"
    shift

    python3 - "$outfile" "$@" <<'PY'
import json
import sys

outfile = sys.argv[1]
paths = sys.argv[2:]

if not paths:
    raise SystemExit("no workload result files to merge")

merged = None
workloads = []
for path in paths:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if merged is None:
        merged = dict(data)
        merged["workloads"] = []
    else:
        if data.get("metadata") != merged.get("metadata"):
            raise SystemExit(f"metadata mismatch while merging {path}")
    workloads.extend(data.get("workloads", []))

merged["workloads"] = workloads
with open(outfile, "w", encoding="utf-8") as f:
    json.dump(merged, f, separators=(",", ":"))
    f.write("\n")
PY

    echo "[run_matrix] Results written to $outfile"
}

run_bench() {
    local label="$1"
    local dispatcher="$2"
    local jit_enable="$3"
    local workload
    local workload_files=()

    echo "[run_matrix] === $label (VDBE_DISPATCHER=$dispatcher SQL_JIT_ENABLE=$jit_enable) ==="
    for workload in "${WORKLOADS[@]}"; do
        run_workload "$label" "$dispatcher" "$jit_enable" "$workload"
        workload_files+=("$TMPDIR/bench_${label}_${workload}.json")
    done
    merge_results "$label" "${workload_files[@]}"
}

echo "[run_matrix] Build: $(pwd)"
echo "[run_matrix] Tarantool: $TARANTOOL"
echo "[run_matrix] Runs per case: $RUNS"
echo "[run_matrix] Workloads: ${WORKLOADS[*]}"
echo ""

run_bench "interpreter"  "generated" "0"
run_bench "mcjit"        "generated" "1"
run_bench "cnp"          "cnp"       "0"

echo ""
echo "[run_matrix] All runs complete. Results:"
ls -lh "$OUTDIR"/bench_*.json
