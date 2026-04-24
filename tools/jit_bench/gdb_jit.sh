#!/usr/bin/env bash
# gdb_jit.sh — Run a Tarantool Lua script under GDB with full JIT debug info.
#
# MCJIT frames are visible to gdb by name (GDB JIT registration listener
# is always on).  CnP frames are unwound correctly (.eh_frame CFI always
# registered).  No extra environment variables needed.
#
# Usage:
#   cd <builddir>
#   bash /path/to/tools/jit_bench/gdb_jit.sh [OPTIONS] [-- LUA_SCRIPT [ARGS...]]
#
# Options:
#   -d, --dispatcher <name>  VDBE_DISPATCHER value (generated|cnp|old)
#                            default: cnp
#   -j, --jit                Enable MCJIT (SQL_JIT_ENABLE=1)
#   -b, --bench              Use the built-in benchmark workload
#   -w, --workload <name>    BENCH_ONLY_WORKLOAD for --bench
#   -C, --case <name>        BENCH_ONLY_CASE for --bench
#   -n, --runs <count>       BENCH_RUNS for --bench
#   -c, --cmd <gdb-cmd>      Extra GDB -ex command (repeatable)
#   -B, --batch              Run GDB in batch mode (non-interactive)
#   -h, --help               Show this help
#
# Examples:
#   # Interactive session with CnP, break at first compile:
#   bash tools/jit_bench/gdb_jit.sh -d cnp -- /tmp/my_workload.lua
#
#   # Batch: log every compile + exec, run benchmark, print summary:
#   bash tools/jit_bench/gdb_jit.sh --batch --bench -d cnp
#
#   # MCJIT interactive:
#   bash tools/jit_bench/gdb_jit.sh -d generated --jit -- /tmp/my_workload.lua

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GDB_SCRIPT="$SCRIPT_DIR/vdbe_jit.gdb"
TARANTOOL="${TARANTOOL:-./src/tarantool}"

DISPATCHER="cnp"
JIT_ENABLE="0"
USE_BENCH=0
BATCH=0
LUA_SCRIPT=""
BENCH_WORKLOAD=""
BENCH_CASE=""
BENCH_RUNS=""
EXTRA_CMDS=()

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
        -c|--cmd)        EXTRA_CMDS+=("-ex" "$2"); shift 2;;
        -B|--batch)      BATCH=1; shift;;
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

if ! command -v gdb &>/dev/null; then
    echo "error: gdb not found in PATH" >&2
    exit 1
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

# Clean stale state
rm -f ./*.snap ./*.xlog

echo "[gdb_jit] Tarantool : $TARANTOOL"
echo "[gdb_jit] Script    : $LUA_SCRIPT"
echo "[gdb_jit] Dispatcher: $DISPATCHER  JIT=$JIT_ENABLE"
if [[ -n "$BENCH_WORKLOAD" || -n "$BENCH_CASE" || -n "$BENCH_RUNS" ]]; then
    echo "[gdb_jit] Benchmark : workload=${BENCH_WORKLOAD:-all} case=${BENCH_CASE:-all} runs=${BENCH_RUNS:-default}"
fi
echo "[gdb_jit] GDB script: $GDB_SCRIPT"
echo ""

# Build gdb command
GDB_ARGS=(
    -ex "set pagination off"
    -ex "source $GDB_SCRIPT"
)

# Load user extra commands before tarantool-gdb.py so that commands
# blocks (sql-break-compile etc.) are registered before the Python
# fiber unwinder is installed.  The fiber unwinder modifies frame
# context and must not be present when the commands blocks are set up.
GDB_ARGS+=("${EXTRA_CMDS[@]}")

# Tarantool's Python gdb extension (if present) — loaded last so it
# does not interfere with the GDB commands blocks above.
TNTGDB="$REPO_ROOT/tools/tarantool-gdb.py"
if [[ -f "$TNTGDB" ]]; then
    GDB_ARGS+=(-ex "source $TNTGDB")
fi

if [[ "$BATCH" -eq 1 ]]; then
    GDB_ARGS+=(-batch -ex "run")
fi

VDBE_DISPATCHER="$DISPATCHER" \
SQL_JIT_ENABLE="$JIT_ENABLE" \
BENCH_ONLY_WORKLOAD="$BENCH_WORKLOAD" \
BENCH_ONLY_CASE="$BENCH_CASE" \
BENCH_RUNS="$BENCH_RUNS" \
gdb "${GDB_ARGS[@]}" \
    --args "$TARANTOOL" "$LUA_SCRIPT" "$@"
