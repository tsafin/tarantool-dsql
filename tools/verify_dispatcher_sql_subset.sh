#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
TARANTOOL_BIN="${TARANTOOL_BIN:-$BUILD_DIR/src/tarantool}"

if [ ! -x "$TARANTOOL_BIN" ]; then
    echo "tarantool binary not found or not executable: $TARANTOOL_BIN" >&2
    exit 1
fi

TESTS=(
    sql-tap/select1.test.lua
    sql-tap/check.test.lua
    sql-tap/trigger1.test.lua
    sql-tap/limit.test.lua
    sql-tap/join.test.lua
    sql-tap/join2.test.lua
    sql-tap/join3.test.lua
    sql-tap/orderby1.test.lua
    sql-tap/orderby2.test.lua
    sql-tap/distinct.test.lua
    sql-tap/subselect.test.lua
    sql-tap/where2.test.lua
    sql-tap/where3.test.lua
)

run_mode() {
    local mode="$1"
    local vardir="/tmp/t-${mode}-sql-subset"
    local log_file="${RESULTS_DIR:-/tmp}/vdbe_sql_subset_${mode}.log"

    rm -rf "$vardir"
    echo "[sql-subset] $mode"
    (
        cd "$PROJECT_ROOT"
        env VDBE_DISPATCHER="$mode" \
            ./test/test-run.py \
            --builddir "$BUILD_DIR" \
            --executable "$TARANTOOL_BIN" \
            --vardir "$vardir" \
            --force \
            "${TESTS[@]}"
    ) >"$log_file" 2>&1
    echo "  ok"
}

run_mode old
run_mode generated

cat <<EOF
Representative SQL TAP subset passed in both dispatcher modes.

Covered tests:
  - sql-tap/select1.test.lua
  - sql-tap/check.test.lua
  - sql-tap/trigger1.test.lua
  - sql-tap/limit.test.lua
  - sql-tap/join.test.lua
  - sql-tap/join2.test.lua
  - sql-tap/join3.test.lua
  - sql-tap/orderby1.test.lua
  - sql-tap/orderby2.test.lua
  - sql-tap/distinct.test.lua
  - sql-tap/subselect.test.lua
  - sql-tap/where2.test.lua
  - sql-tap/where3.test.lua
EOF
