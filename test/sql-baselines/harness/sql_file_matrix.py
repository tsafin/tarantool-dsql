#!/usr/bin/env python3
"""Reproduce the SQL-only adapter acceptance matrix before corpus inclusion."""

import argparse
import json
import os
from pathlib import Path
import subprocess


TESTS = (
    "gh-4256-do-not-change-order-during-insertion.test.sql",
    "gh-4697-scalar-bool-sort-cmp.test.sql",
)
ENGINES = ("memtx", "vinyl")
MODES = ("generated", "cnp", "llvm")


def run(*args, env=None, cwd=None):
    subprocess.run([str(a) for a in args], check=True, env=env, cwd=cwd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out-root", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    binary = args.binary.resolve()
    root = args.out_root.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("output root must be empty")
    root.mkdir(parents=True, exist_ok=True)
    baseline = repo / "test/sql-baselines"
    rows = []
    for test in TESTS:
        stem = test[:-len(".test.sql")]
        for engine in ENGINES:
            paths = {}
            for mode in (*MODES, "generated-repeat"):
                run_dir = root / stem / engine / mode
                work, out = run_dir / "work", run_dir / "out"
                work.mkdir(parents=True)
                out.mkdir()
                env = os.environ.copy()
                env["VDBE_DISPATCHER"] = "cnp" if mode == "cnp" else "generated"
                env["SQL_JIT_ENABLE"] = "1" if mode == "llvm" else "0"
                run(binary, baseline / "harness/run.lua", repo / "test/sql" / test,
                    f"--engine={engine}", f"--out={out}", f"--work-dir={work}",
                    env=env, cwd=binary.parent.parent)
                run(binary, baseline / "validate.lua", out, cwd=binary.parent.parent)
                manifest_path = out / "manifests/sql" / f"{stem}.{engine}.json"
                manifest = json.loads(manifest_path.read_text())
                if not manifest["accepted"] or not manifest["mode_executed"]:
                    raise ValueError(f"capture rejected: {test} {engine} {mode}")
                paths[mode] = out
                rows.append({"test": test, "engine": engine, "mode": mode,
                             "snapshots": manifest["written_snapshots"],
                             "cnp_exec_delta": manifest["cnp_exec_delta"],
                             "llvm_exec_delta": manifest["llvm_exec_delta"]})
            for mode in ("cnp", "llvm", "generated-repeat"):
                run(binary, baseline / "diff.lua", paths["generated"], paths[mode],
                    "--format=json", cwd=binary.parent.parent)
    print(json.dumps(rows, indent=2))


if __name__ == "__main__":
    main()
