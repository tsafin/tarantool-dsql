#!/usr/bin/env python3
"""
M2 gate test for Copy-and-Patch VDBE dispatcher.

Gate: VDBE_DISPATCHER=cnp box.execute("SELECT 42") returns 42
and sql_cnp_exec_count is non-zero.

Runs structural checks on the generated stencils header, then
executes the Lua gate script via subprocess.

Usage:
    python3 tools/test_cnp_m2.py [--builddir BUILD] [--srcdir SRC]
"""

import argparse
import os
import re
import subprocess
import sys


def check_stencils_header(stencils_h_path):
    """Structural checks on vdbe_cnp_stencils.h."""
    errors = []

    with open(stencils_h_path) as f:
        content = f.read()

    # 1. OP_Init stencil must exist
    if "cnp_bytes_init" not in content:
        errors.append("OP_Init stencil (cnp_bytes_init) not found in stencils.h")

    # 2. CNP_HOLE_SIGNAL must be in the enum
    if "CNP_HOLE_SIGNAL" not in content:
        errors.append("CNP_HOLE_SIGNAL not in enum")

    # 3. OP_ResultRow must reference CNP_HOLE_SIGNAL (not CNP_HOLE_BRANCH)
    # Find the cnp_holes_resultrow array
    m = re.search(
        r"cnp_holes_resultrow\[\] = \{(.*?)\};", content, re.DOTALL
    )
    if m:
        holes_block = m.group(1)
        if "CNP_HOLE_SIGNAL" not in holes_block:
            errors.append(
                "OP_ResultRow holes do not contain CNP_HOLE_SIGNAL"
            )
        # OP_ResultRow should NOT have CNP_HOLE_BRANCH anymore
        if "CNP_HOLE_BRANCH" in holes_block:
            errors.append(
                "OP_ResultRow holes still contain CNP_HOLE_BRANCH (should use CNP_HOLE_SIGNAL)"
            )
    else:
        errors.append("cnp_holes_resultrow not found in stencils.h")

    # 4. OP_Halt must reference CNP_HOLE_BRANCH (for SQL_DONE return)
    m = re.search(
        r"cnp_holes_halt\[\] = \{(.*?)\};", content, re.DOTALL
    )
    if m:
        holes_block = m.group(1)
        if "CNP_HOLE_BRANCH" not in holes_block:
            errors.append("OP_Halt holes do not contain CNP_HOLE_BRANCH")
    else:
        errors.append("cnp_holes_halt not found in stencils.h")

    # 5. OP_Init must reference CNP_HOLE_BRANCH (for jump to P2)
    m = re.search(
        r"cnp_holes_init\[\] = \{(.*?)\};", content, re.DOTALL
    )
    if m:
        holes_block = m.group(1)
        if "CNP_HOLE_BRANCH" not in holes_block:
            errors.append("OP_Init holes do not contain CNP_HOLE_BRANCH")
    else:
        errors.append("cnp_holes_init not found in stencils.h")

    # 6. Must have exactly 7 stencils (M1 set + OP_Init)
    stencil_count = len(re.findall(
        r"static const uint8_t cnp_bytes_\w+\[\]", content
    ))
    if stencil_count != 7:
        errors.append(f"Expected 7 stencils, found {stencil_count}")

    return errors


def run_m2_lua(tarantool_bin, lua_script):
    """Run the Lua gate test with VDBE_DISPATCHER=cnp."""
    env = os.environ.copy()
    env["VDBE_DISPATCHER"] = "cnp"

    # Work in a temp dir to avoid polluting the build root
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        result = subprocess.run(
            [tarantool_bin, lua_script],
            env=env,
            capture_output=True,
            text=True,
            cwd=tmpdir,
        )

    print("--- tarantool stdout ---")
    print(result.stdout)
    if result.stderr:
        print("--- tarantool stderr ---")
        print(result.stderr)

    return result.returncode, result.stdout


def main():
    parser = argparse.ArgumentParser(description="M2 CnP gate test")
    parser.add_argument(
        "--builddir",
        default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        help="Tarantool build directory (default: repo root)",
    )
    parser.add_argument(
        "--srcdir",
        default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        help="Tarantool source root (default: repo root)",
    )
    args = parser.parse_args()

    builddir = args.builddir
    srcdir = args.srcdir

    tarantool_bin = os.path.join(builddir, "src", "tarantool")
    stencils_h = os.path.join(
        srcdir, "src", "box", "sql", "generated", "vdbe_cnp_stencils.h"
    )
    lua_script = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "test_cnp_m2.lua"
    )

    print(f"Tarantool: {tarantool_bin}")
    print(f"Stencils:  {stencils_h}")
    print(f"Lua test:  {lua_script}")
    print()

    # --- Structural checks ---
    print("=== Structural checks ===")
    errors = check_stencils_header(stencils_h)
    if errors:
        for e in errors:
            print(f"  FAIL: {e}")
        print("\nStructural checks FAILED")
        sys.exit(1)
    print("  All structural checks PASSED")
    print()

    # --- Runtime check ---
    print("=== Runtime check (VDBE_DISPATCHER=cnp) ===")
    rc, stdout = run_m2_lua(tarantool_bin, lua_script)
    if rc != 0:
        print(f"\nRuntime check FAILED (exit code {rc})")
        sys.exit(1)
    if "M2 gate: PASS" not in stdout:
        print("\nRuntime check FAILED: 'M2 gate: PASS' not in output")
        sys.exit(1)
    print("  Runtime check PASSED")
    print()
    print("=== M2 GATE: ALL CHECKS PASSED ===")


if __name__ == "__main__":
    main()
