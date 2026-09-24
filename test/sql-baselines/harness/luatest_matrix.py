#!/usr/bin/env python3
"""Reproduce a sql-luatest single-child capture parity matrix."""

import argparse
import json
from pathlib import Path
import subprocess


def run(*args):
    result = subprocess.run([str(a) for a in args], check=True,
                            text=True, stdout=subprocess.PIPE)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--test", default="gh_6575_assertion_in_modulo_test.lua")
    args = parser.parse_args()
    repo = args.repo.resolve()
    binary = args.binary.resolve()
    root = args.out_root.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("output root must be empty")
    root.mkdir(parents=True, exist_ok=True)
    results = []
    for engine in ("memtx", "vinyl"):
        outputs = {}
        for mode in ("generated", "cnp", "llvm", "generated-repeat"):
            out = root / engine / mode
            actual_mode = "generated" if mode == "generated-repeat" else mode
            run("python3", repo / "test/sql-baselines/luatest_capture.py",
                "--repo", repo, "--runner-repo", args.runner_repo,
                "--binary", binary, "--out", out,
                "--test", args.test, "--engine", engine,
                "--mode", actual_mode)
            outputs[mode] = out
            stem = args.test[:-len(".lua")]
            manifest = json.loads((out / "manifests/sql-luatest" /
                                   f"{stem}.{engine}.json").read_text())
            results.append({"engine": engine, "mode": mode,
                            "snapshots": manifest["written_snapshots"],
                            "cnp_exec_delta": manifest["cnp_exec_delta"],
                            "llvm_exec_delta": manifest["llvm_exec_delta"]})
        for mode in ("cnp", "llvm", "generated-repeat"):
            run(binary, repo / "test/sql-baselines/diff.lua",
                outputs["generated"], outputs[mode], "--format=json")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
