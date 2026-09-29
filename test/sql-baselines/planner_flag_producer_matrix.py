#!/usr/bin/env python3
"""Run focused all-producer ledger fixtures across engines and dispatchers."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


HERE = Path(__file__).resolve().parent
LUATEST_CAPTURE = HERE / "luatest_capture.py"
TESTS = (
    "planner_final_paths_test.lua",
    "planner_insert_select_snapshot_test.lua",
    "planner_flag_fallback_parity_test.lua",
    "planner_composite_prefix_range_test.lua",
    "planner_scalar_filter_test.lua",
)
ENGINES = ("memtx", "vinyl")
MODES = ("generated", "cnp", "llvm")


def invoke_case(repo, binary, out, test, engine, mode):
    stem = test[:-len(".lua")]
    case_dir = out / "cases" / mode / engine / stem
    command = [sys.executable, str(LUATEST_CAPTURE), "--repo", str(repo),
               "--runner-repo", str(repo), "--binary", str(binary),
               "--out", str(case_dir), "--suite", "sql-luatest",
               "--test", test, "--engine", engine, "--mode", mode]
    result = subprocess.run(command, cwd=repo, text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise RuntimeError(f"{mode}/{engine}/{test} failed:\n" +
                           result.stdout[-5000:])
    manifest = case_dir / "manifests/sql-luatest" / \
        f"{stem}.{engine}.json"
    data = json.loads(manifest.read_text())
    if data.get("accepted") is not True or \
            data.get("component_ledger_version") != 1 or \
            data.get("captured_queries", 0) < 1:
        raise RuntimeError(f"incomplete component-ledger capture: {manifest}")
    return {
        "test": test,
        "engine": engine,
        "mode": mode,
        "status": "passed",
        "captured_queries": data["captured_queries"],
        "component_ledger_version": data["component_ledger_version"],
        "cnp_exec_delta": data.get("cnp_exec_delta", 0),
        "llvm_exec_delta": data.get("llvm_exec_delta", 0),
        "manifest": str(manifest),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    repo, binary, out = args.repo.resolve(), args.binary.resolve(), args.out.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError("output must be empty")
    out.mkdir(parents=True, exist_ok=True)
    commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    cases = []
    for mode in MODES:
        for engine in ENGINES:
            for test in TESTS:
                print(f"RUN {mode}/{engine}/{test}", flush=True)
                cases.append(invoke_case(repo, binary, out, test, engine, mode))
    report = {"matrix_version": 1, "source_commit": commit,
              "scope": "focused SELECT producer component-ledger runtime cases",
              "cases": cases}
    path = out / "report.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(path), "cases": len(cases)}, sort_keys=True))


if __name__ == "__main__":
    main()
