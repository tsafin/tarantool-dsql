#!/usr/bin/env bash
# perf_jit.sh — Profile a Tarantool Lua script with per-opcode JIT attribution.
#
# For CnP: writes /tmp/jit-PID.dump (JITDUMP) and /tmp/perf-PID.map.
# For MCJIT: writes /tmp/jit-PID.dump via LLVM PerfJITEventListener.
# Both: `perf inject --jit` rewrites perf.data to include DWARF source info.
#
# CnP JITDUMP debug entries map each opcode stencil to its mnemonic name
# (e.g. OP_Column, OP_Compare), giving per-opcode attribution in perf report.
#
# Requirements: linux-tools (perf), kernel.perf_event_paranoid <= 1
#
# Usage:
#   cd <builddir>
#   bash /path/to/tools/jit_bench/perf_jit.sh [OPTIONS] [-- LUA_SCRIPT [ARGS...]]
#
# Options:
#   -d, --dispatcher <name>  VDBE_DISPATCHER value (generated|cnp|old)
#                            default: cnp
#   -j, --jit                Enable MCJIT (SQL_JIT_ENABLE=1)
#   -b, --bench              Use the built-in benchmark workload
#   -w, --workload <name>    BENCH_ONLY_WORKLOAD for --bench
#   -C, --case <name>        BENCH_ONLY_CASE for --bench
#   -n, --runs <count>       BENCH_RUNS for --bench
#   -e, --event <event>      perf event(s) (default: cycles)
#   -g, --callgraph          Record call-graph with DWARF (slower)
#   -r, --report-args <str>  Extra args passed to perf report
#   --no-report              Skip perf report (just record + inject)
#   -h, --help               Show this help
#
# Examples:
#   # Profile CnP benchmark, show per-opcode report:
#   bash tools/jit_bench/perf_jit.sh --bench -d cnp
#
#   # Profile MCJIT with call-graph:
#   bash tools/jit_bench/perf_jit.sh --bench -d generated --jit -g
#
#   # Custom workload with cycles+instructions:
#   bash tools/jit_bench/perf_jit.sh -d cnp -e cycles,instructions -- /tmp/bench.lua

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARANTOOL="${TARANTOOL:-./src/tarantool}"

DISPATCHER="cnp"
JIT_ENABLE="0"
USE_BENCH=0
LUA_SCRIPT=""
BENCH_WORKLOAD=""
BENCH_CASE=""
BENCH_RUNS=""
PERF_EVENT="cycles"
CALLGRAPH=0
REPORT_ARGS="--stdio"
DO_REPORT=1

usage() {
    sed -n '/^# Usage/,/^$/p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--dispatcher) DISPATCHER="$2"; shift 2;;
        -j|--jit)        JIT_ENABLE="1"; shift;;
        -b|--bench)      USE_BENCH=1; shift;;
        -w|--workload)   BENCH_WORKLOAD="$2"; shift 2;;
        -C|--case)       BENCH_CASE="$2"; shift 2;;
        -n|--runs)       BENCH_RUNS="$2"; shift 2;;
        -e|--event)      PERF_EVENT="$2"; shift 2;;
        -g|--callgraph)  CALLGRAPH=1; shift;;
        -r|--report-args) REPORT_ARGS="$2"; shift 2;;
        --no-report)     DO_REPORT=0; shift;;
        -h|--help)       usage 0;;
        --)              shift; LUA_SCRIPT="${1:-}"; shift; break;;
        *)               echo "Unknown option: $1"; usage 1;;
    esac
done

if [[ ! -x "$TARANTOOL" ]]; then
    echo "error: tarantool not found at '$TARANTOOL'" >&2
    echo "       Run from the build directory or set TARANTOOL=path/to/tarantool" >&2
    exit 1
fi

if ! command -v perf &>/dev/null; then
    echo "error: perf not found in PATH" >&2
    echo "       Install linux-tools or linux-perf package" >&2
    exit 1
fi

# Check paranoia level (need <= 1 for kernel symbols, <= 2 for user-space)
PARANOIA=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 3)
if [[ "$PARANOIA" -gt 2 ]]; then
    echo "warning: kernel.perf_event_paranoid=$PARANOIA — profiling may fail" >&2
    echo "         Run: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid" >&2
fi

if [[ "$USE_BENCH" -eq 1 ]]; then
    LUA_SCRIPT="$SCRIPT_DIR/sql_llvm_mcjit_benchmark.lua"
fi

if [[ -z "$LUA_SCRIPT" ]]; then
    echo "error: no Lua script specified (use --bench or -- <script.lua>)" >&2
    usage 1
fi

if [[ ! -f "$LUA_SCRIPT" ]]; then
    echo "error: script not found: $LUA_SCRIPT" >&2
    exit 1
fi

rm -f ./*.snap ./*.xlog

echo "[perf_jit] Tarantool : $TARANTOOL"
echo "[perf_jit] Script    : $LUA_SCRIPT"
echo "[perf_jit] Dispatcher: $DISPATCHER  JIT=$JIT_ENABLE"
if [[ -n "$BENCH_WORKLOAD" || -n "$BENCH_CASE" || -n "$BENCH_RUNS" ]]; then
    echo "[perf_jit] Benchmark : workload=${BENCH_WORKLOAD:-all} case=${BENCH_CASE:-all} runs=${BENCH_RUNS:-default}"
fi
echo "[perf_jit] Event     : $PERF_EVENT"
echo ""

# Determine if we should enable MCJIT vs CnP JITDUMP flags
PERF_ENV=()
if [[ "$DISPATCHER" == "cnp" ]]; then
    PERF_ENV+=(
        "SQL_CNP_PERF_MAP=1"
        "SQL_CNP_JITDUMP=1"
    )
    echo "[perf_jit] CnP: writing /tmp/perf-PID.map and /tmp/jit-PID.dump"
fi
if [[ "$JIT_ENABLE" == "1" ]]; then
    PERF_ENV+=("SQL_JIT_PERF_MAP=1")
    echo "[perf_jit] MCJIT: writing /tmp/jit-PID.dump"
fi
PERF_ENV+=(
    "VDBE_DISPATCHER=$DISPATCHER"
    "SQL_JIT_ENABLE=$JIT_ENABLE"
    "BENCH_ONLY_WORKLOAD=$BENCH_WORKLOAD"
    "BENCH_ONLY_CASE=$BENCH_CASE"
    "BENCH_RUNS=$BENCH_RUNS"
)

# Build perf record args
PERF_RECORD_ARGS=(
    record
    -k mono          # required for JITDUMP timestamp correlation
    -e "$PERF_EVENT"
    -o perf.data
)
if [[ "$CALLGRAPH" -eq 1 ]]; then
    PERF_RECORD_ARGS+=(--call-graph dwarf)
fi

echo "[perf_jit] Step 1: perf record ..."
env "${PERF_ENV[@]}" \
    perf "${PERF_RECORD_ARGS[@]}" \
    -- "$TARANTOOL" "$LUA_SCRIPT" "$@"

echo ""
echo "[perf_jit] Step 2: perf inject --jit ..."
perf inject --jit -i perf.data -o perf.jit.data

if [[ "$DO_REPORT" -eq 1 ]]; then
    echo ""
    echo "[perf_jit] Step 3: perf report ..."
    echo "           (JIT functions show by name; CnP frames show opcode mnemonics)"
    echo ""
    # shellcheck disable=SC2086
    PERF_PAGER=cat perf report -i perf.jit.data $REPORT_ARGS
fi

echo ""
echo "[perf_jit] Done.  Raw data: perf.data  Injected: perf.jit.data"
echo "           Re-run report: perf report -i perf.jit.data --stdio"
