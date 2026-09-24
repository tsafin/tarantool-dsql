#!/usr/bin/env python3
"""Audit native parity and recapture stability for SQL normal-runner tests."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def run(command, timeout):
    try:
        result = subprocess.run([str(c) for c in command],
                                text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        return result.returncode == 0, result.stdout[-1200:]
    except subprocess.TimeoutExpired:
        return False, f"exceeded {timeout}s"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--triage-report", type=Path, required=True)
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=45)
    args = parser.parse_args()
    repo, binary = args.repo.resolve(), args.binary.resolve()
    root = args.out_root.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("output root must be empty")
    root.mkdir(parents=True, exist_ok=True)
    triage = json.loads(args.triage_report.read_text())
    candidates = [row for row in triage["tests"]
                  if any(row["engines"][e]["status"] == "captured"
                         for e in ("memtx", "vinyl"))]
    rows = []
    for index, candidate in enumerate(candidates, 1):
        test = candidate["test"].split("/", 1)[1]
        stem = test[:-len(".test.lua")]
        row = {"test": candidate["test"], "engines": {}}
        for engine in ("memtx", "vinyl"):
            if candidate["engines"][engine]["status"] != "captured":
                row["engines"][engine] = {"status": "not_in_triage"}
                continue
            paths = {}
            outcome = {"status": "accepted", "counts": {}}
            for mode in ("generated", "cnp", "llvm", "generated-repeat"):
                actual_mode = "generated" if mode == "generated-repeat" else mode
                out = root / stem / engine / mode
                command = [sys.executable,
                           repo / "test/sql-baselines/luatest_capture.py",
                           "--repo", repo, "--runner-repo", args.runner_repo,
                           "--binary", binary, "--out", out,
                           "--test", test, "--suite", "sql",
                           "--engine", engine, "--mode", actual_mode]
                ok, detail = run(command, args.timeout)
                if not ok:
                    outcome.update(status="failed", failure=mode, reason=detail)
                    break
                paths[mode] = out
                manifest = json.loads((out / "manifests/sql" /
                                       f"{stem}.{engine}.json").read_text())
                outcome["counts"][mode] = manifest["captured_queries"]
            if outcome["status"] == "accepted":
                for mode in ("cnp", "llvm", "generated-repeat"):
                    ok, detail = run([binary, repo / "test/sql-baselines/diff.lua",
                                      paths["generated"], paths[mode],
                                      "--format=json"], args.timeout)
                    if not ok:
                        outcome.update(status="drift", failure=mode, reason=detail)
                        break
            row["engines"][engine] = outcome
        rows.append(row)
        print(f"{index}/{len(candidates)} {test}: "
              f"memtx={row['engines']['memtx']['status']} "
              f"vinyl={row['engines']['vinyl']['status']}", flush=True)
        args.report.write_text(json.dumps({"tests": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
