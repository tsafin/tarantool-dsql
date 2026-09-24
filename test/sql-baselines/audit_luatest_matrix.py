#!/usr/bin/env python3
"""Audit six-mode parity for candidates that passed generated/memtx triage."""

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
        return result.returncode == 0, result.stdout[-1600:]
    except subprocess.TimeoutExpired:
        return False, f"exceeded {timeout}s"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--triage-report", type=Path, required=True)
    parser.add_argument("--triage-out", type=Path, required=True)
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    repo = args.repo.resolve()
    binary = args.binary.resolve()
    root = args.out_root.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("output root must be empty")
    root.mkdir(parents=True, exist_ok=True)
    triage = json.loads(args.triage_report.read_text())
    candidates = [row["test"].split("/", 1)[1] for row in triage["tests"]
                  if row["status"] == "captured"]
    rows = []
    for index, test in enumerate(candidates, 1):
        stem = test[:-len(".lua")]
        outputs = {("memtx", "generated"): args.triage_out.resolve() / stem}
        row = {"test": f"sql-luatest/{test}", "status": "accepted",
               "engines": ["memtx", "vinyl"], "counts": {}}
        for engine in ("memtx", "vinyl"):
            for mode in ("generated", "cnp", "llvm", "generated-repeat"):
                if engine == "memtx" and mode == "generated":
                    continue
                actual_mode = "generated" if mode == "generated-repeat" else mode
                out = root / stem / engine / mode
                command = [sys.executable,
                           repo / "test/sql-baselines/luatest_capture.py",
                           "--repo", repo, "--runner-repo", args.runner_repo,
                           "--binary", binary, "--out", out,
                           "--test", test, "--engine", engine,
                           "--mode", actual_mode]
                ok, detail = run(command, args.timeout)
                if not ok:
                    row.update(status="failed", failure=f"{engine}/{mode}",
                               reason=detail)
                    break
                outputs[(engine, mode)] = out
                manifest = json.loads((out / "manifests/sql-luatest" /
                                       f"{stem}.{engine}.json").read_text())
                row["counts"][f"{engine}/{mode}"] = manifest["captured_queries"]
            if row["status"] != "accepted":
                break
            for mode in ("cnp", "llvm", "generated-repeat"):
                command = [binary, repo / "test/sql-baselines/diff.lua",
                           outputs[(engine, "generated")], outputs[(engine, mode)],
                           "--format=json"]
                ok, detail = run(command, args.timeout)
                if not ok:
                    row.update(status="drift", failure=f"{engine}/{mode}",
                               reason=detail)
                    break
            if row["status"] != "accepted":
                break
        rows.append(row)
        print(f"{index}/{len(candidates)} {row['status']}: {row['test']} "
              f"{row.get('failure', '')}", flush=True)
        args.report.write_text(json.dumps({"tests": rows}, indent=2) + "\n")
    summary = {}
    for row in rows:
        summary[row["status"]] = summary.get(row["status"], 0) + 1
    print(json.dumps(summary, sort_keys=True))


if __name__ == "__main__":
    main()
