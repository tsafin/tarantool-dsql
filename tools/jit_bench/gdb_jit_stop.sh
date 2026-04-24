#!/usr/bin/env bash
# gdb_jit_stop.sh — Run the self-stop JIT demo under GDB and show the
# generated function, backtrace, and disassembly for CnP or MCJIT.
#
# Usage:
#   cd <builddir>
#   bash /path/to/tools/jit_bench/gdb_jit_stop.sh [OPTIONS]
#
# Options:
#   -d, --dispatcher <name>  VDBE_DISPATCHER value (generated|cnp|old)
#                            default: cnp
#   -j, --jit                Enable MCJIT (SQL_JIT_ENABLE=1)
#   -s, --script <path>      Lua self-stop script
#                            default: tools/jit_bench/sql_jit_stop_demo.lua
#   -c, --cmd <gdb-cmd>      Extra GDB -ex command (repeatable)
#   -B, --batch              Run GDB in batch mode and exit
#   -h, --help               Show this help
#
# Examples:
#   # CnP: stop after warmup, show generated frame and disassembly, stay in GDB
#   bash tools/jit_bench/gdb_jit_stop.sh -d cnp
#
#   # MCJIT: automated batch run
#   bash tools/jit_bench/gdb_jit_stop.sh -d generated --jit --batch

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GDB_SCRIPT="$SCRIPT_DIR/vdbe_jit.gdb"
STOP_SCRIPT="$SCRIPT_DIR/sql_jit_stop_demo.lua"
TARANTOOL="${TARANTOOL:-./src/tarantool}"

DISPATCHER="cnp"
JIT_ENABLE="0"
BATCH=0
EXTRA_CMDS=()

usage() {
    sed -n '/^# Usage/,/^$/p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--dispatcher) DISPATCHER="$2"; shift 2;;
        -j|--jit)        JIT_ENABLE="1"; shift;;
        -s|--script)     STOP_SCRIPT="$2"; shift 2;;
        -c|--cmd)        EXTRA_CMDS+=("-ex" "$2"); shift 2;;
        -B|--batch)      BATCH=1; shift;;
        -h|--help)       usage 0;;
        *)               echo "Unknown option: $1"; usage 1;;
    esac
done

if [[ ! -x "$TARANTOOL" ]]; then
    echo "error: tarantool not found at '$TARANTOOL'" >&2
    echo "       Run from the build directory or set TARANTOOL=path/to/tarantool" >&2
    exit 1
fi
TARANTOOL="$(cd "$(dirname "$TARANTOOL")" && pwd)/$(basename "$TARANTOOL")"

if [[ ! -f "$STOP_SCRIPT" ]]; then
    echo "error: script not found: $STOP_SCRIPT" >&2
    exit 1
fi

if ! command -v gdb &>/dev/null; then
    echo "error: gdb not found in PATH" >&2
    exit 1
fi

WORKDIR="${GDB_JIT_STOP_WORKDIR:-$(mktemp -d /tmp/jit-gdb-stop.XXXXXX)}"
rm -f "$WORKDIR"/*.snap "$WORKDIR"/*.xlog 2>/dev/null || true

echo "[gdb_jit_stop] Tarantool : $TARANTOOL"
echo "[gdb_jit_stop] Script    : $STOP_SCRIPT"
echo "[gdb_jit_stop] Dispatcher: $DISPATCHER  JIT=$JIT_ENABLE"
echo "[gdb_jit_stop] GDB script: $GDB_SCRIPT"
echo "[gdb_jit_stop] Workdir   : $WORKDIR"
echo ""

GDB_ARGS=(
    -ex "set pagination off"
    -ex "source $GDB_SCRIPT"
    -ex "run $STOP_SCRIPT"
)

if [[ "$JIT_ENABLE" == "1" ]]; then
    GDB_ARGS+=(
        -ex 'set $p = sql_get()->pVdbe'
        -ex 'jit-info $p'
        -ex 'set $jit = (void*)$p->jit_func'
        -ex 'info symbol $jit'
        -ex 'break *$jit'
        -ex 'signal 0'
        -ex 'bt'
        -ex 'jit-find $pc'
        -ex 'x/32i $jit'
    )
else
    GDB_ARGS+=(
        -ex 'sql-vdbes'
        -ex 'set $p = sql_get()->pVdbe'
        -ex 'cnp-info $p'
        -ex 'set $code = (void*)$p->cnp_code'
        -ex 'info symbol $code'
        -ex 'break *$code'
        -ex 'signal 0'
        -ex 'bt'
        -ex 'cnp-find $pc'
        -ex 'x/32i $code'
    )
fi

GDB_ARGS+=("${EXTRA_CMDS[@]}")

if [[ "$BATCH" -eq 1 ]]; then
    GDB_ARGS=(-batch "${GDB_ARGS[@]}")
fi

(
    cd "$WORKDIR"
    VDBE_DISPATCHER="$DISPATCHER" \
    SQL_JIT_ENABLE="$JIT_ENABLE" \
    gdb "${GDB_ARGS[@]}" \
        --args "$TARANTOOL"
)
