#!/usr/bin/env bash
# Run the SQL JIT benchmark matrix for interpreter, LLVM MCJIT, and CnP.
#
# Usage:
#   cd /path/to/build-jit-relwithdebinfo
#   bash /path/to/tools/jit_bench/run_benchmark_matrix.sh
#
# Output: JSON results files in $OUTDIR (default: current directory)
# Results are named: bench_<label>.json

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_SCRIPT="$SCRIPT_DIR/sql_llvm_mcjit_benchmark.lua"

TARANTOOL="${TARANTOOL:-./src/tarantool}"
OUTDIR="${OUTDIR:-.}"
RUNS="${BENCH_RUNS:-3}"

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

run_bench() {
    local label="$1"
    local dispatcher="$2"
    local jit_enable="$3"
    local outfile="$OUTDIR/bench_${label}.json"

    echo "[run_matrix] === $label (VDBE_DISPATCHER=$dispatcher SQL_JIT_ENABLE=$jit_enable) ==="
    rm -f ./*.snap ./*.xlog

    VDBE_DISPATCHER="$dispatcher" \
    SQL_JIT_ENABLE="$jit_enable" \
    BENCH_LABEL="$label" \
    "$TARANTOOL" "$BENCH_SCRIPT" > "$outfile"

    echo "[run_matrix] Results written to $outfile"
}

echo "[run_matrix] Build: $(pwd)"
echo "[run_matrix] Tarantool: $TARANTOOL"
echo "[run_matrix] Runs per case: $RUNS"
echo ""

run_bench "interpreter"  "generated" "0"
run_bench "mcjit"        "generated" "1"
run_bench "cnp"          "cnp"       "0"

echo ""
echo "[run_matrix] All runs complete. Results:"
ls -lh "$OUTDIR"/bench_*.json
