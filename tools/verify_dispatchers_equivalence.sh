#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERIFY_SCRIPT="$SCRIPT_DIR/verify_dispatchers_equivalence.lua"

if [ $# -ge 1 ]; then
    TARANTOOL_BIN="$1"
else
    TARANTOOL_BIN="$PROJECT_ROOT/build/src/tarantool"
fi

if [ ! -x "$TARANTOOL_BIN" ]; then
    echo "tarantool binary not found or not executable: $TARANTOOL_BIN" >&2
    exit 1
fi

if [ ! -f "$VERIFY_SCRIPT" ]; then
    echo "verification script not found: $VERIFY_SCRIPT" >&2
    exit 1
fi

if [ -n "${RESULTS_DIR:-}" ]; then
    mkdir -p "$RESULTS_DIR"
else
    RESULTS_DIR="$(mktemp -d /tmp/vdbe_dispatch_verify.XXXXXX)"
fi
mkdir -p "$RESULTS_DIR/old" "$RESULTS_DIR/generated"

run_mode() {
    local mode="$1"
    local out_file="$RESULTS_DIR/$mode.json"
    local tmp_root="$RESULTS_DIR/$mode"

    (
        cd "$PROJECT_ROOT/build"
        VDBE_DISPATCHER="$mode" \
        VDBE_EQUIV_TMPDIR="$tmp_root" \
        "$TARANTOOL_BIN" "$VERIFY_SCRIPT" > "$out_file"
    )
}

run_mode old
run_mode generated

python3 - "$RESULTS_DIR/old.json" "$RESULTS_DIR/generated.json" <<'PY'
import json
import sys
from pathlib import Path

old_path = Path(sys.argv[1])
gen_path = Path(sys.argv[2])

old = json.loads(old_path.read_text())
gen = json.loads(gen_path.read_text())

if old["steps"] == gen["steps"]:
    print("Dispatcher equivalence verified")
    print(f"Results directory: {old_path.parent}")
    sys.exit(0)

print("Dispatcher equivalence mismatch", file=sys.stderr)
for idx, (old_step, gen_step) in enumerate(zip(old["steps"], gen["steps"])):
    if old_step != gen_step:
        print(f"First mismatch at step {idx + 1}", file=sys.stderr)
        print(f"OLD: {json.dumps(old_step, sort_keys=True)}", file=sys.stderr)
        print(f"GEN: {json.dumps(gen_step, sort_keys=True)}", file=sys.stderr)
        break
else:
    if len(old["steps"]) != len(gen["steps"]):
        print("Step count mismatch", file=sys.stderr)
        print(f"old steps: {len(old['steps'])}", file=sys.stderr)
        print(f"generated steps: {len(gen['steps'])}", file=sys.stderr)
    else:
        print("JSON differs outside step list", file=sys.stderr)

print(f"Results directory: {old_path.parent}", file=sys.stderr)
sys.exit(1)
PY
