#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ $# -ge 1 ]; then
    TARANTOOL_BIN="$1"
else
    TARANTOOL_BIN="$BUILD_DIR/src/tarantool"
fi

if [ ! -x "$TARANTOOL_BIN" ]; then
    echo "tarantool binary not found or not executable: $TARANTOOL_BIN" >&2
    exit 1
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "build directory not found: $BUILD_DIR" >&2
    exit 1
fi

if [ -n "${RESULTS_DIR:-}" ]; then
    mkdir -p "$RESULTS_DIR"
else
    RESULTS_DIR="$(mktemp -d /tmp/vdbe_dispatch_baseline.XXXXXX)"
fi

run_in_build() {
    (
        cd "$BUILD_DIR"
        rm -f ./*.snap ./*.xlog
        "$@"
    )
}

run_case() {
    local name="$1"
    shift
    local log_file="$RESULTS_DIR/$name.log"

    echo "[baseline] $name"
    if "$@" >"$log_file" 2>&1; then
        echo "  ok"
    else
        echo "  failed (see $log_file)" >&2
        tail -n 40 "$log_file" >&2 || true
        exit 1
    fi
}

run_case simple-old \
    run_in_build env VDBE_DISPATCHER=old "$TARANTOOL_BIN" ../tools/verify_dispatchers_simple.lua

run_case simple-generated \
    run_in_build env VDBE_DISPATCHER=generated "$TARANTOOL_BIN" ../tools/verify_dispatchers_simple.lua

run_case phase58-old \
    run_in_build env VDBE_DISPATCHER=old "$TARANTOOL_BIN" ../test_phase58.lua

run_case phase58-generated \
    run_in_build env VDBE_DISPATCHER=generated "$TARANTOOL_BIN" ../test_phase58.lua

run_case equivalence \
    bash "$SCRIPT_DIR/verify_dispatchers_equivalence.sh" "$TARANTOOL_BIN"

parallel_log="$RESULTS_DIR/parallel-placeholder.log"
echo "[baseline] parallel-placeholder"
if run_in_build env VDBE_DISPATCHER=parallel "$TARANTOOL_BIN" ../tools/verify_dispatchers_simple.lua \
    >"$parallel_log" 2>&1; then
    echo "  unexpected success (see $parallel_log)" >&2
    exit 1
fi
echo "  expected failure"

cat <<EOF
Baseline interpreter verification passed.
Results directory: $RESULTS_DIR

Covered:
  - tools/verify_dispatchers_simple.lua in old/generated
  - test_phase58.lua in old/generated
  - tools/verify_dispatchers_equivalence.sh
  - VDBE_DISPATCHER=parallel placeholder check (expected failure)
EOF
