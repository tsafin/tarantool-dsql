#!/usr/bin/env python3
"""Audit opt-in normal-runner capture of sql/*.test.lua on both engines."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=45)
    args = parser.parse_args()
    repo = args.repo.resolve()
    root = args.out_root.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("output root must be empty")
    root.mkdir(parents=True, exist_ok=True)
    rows = []
    tests = sorted((repo / "test/sql").glob("*.test.lua"))
    for index, path in enumerate(tests, 1):
        stem = path.name[:-len(".test.lua")]
        row = {"test": f"sql/{path.name}", "engines": {}}
        for engine in ("memtx", "vinyl"):
            command = [sys.executable,
                       repo / "test/sql-baselines/luatest_capture.py",
                       "--repo", repo, "--runner-repo", args.runner_repo,
                       "--binary", args.binary, "--out", root / stem / engine,
                       "--test", path.name, "--suite", "sql",
                       "--engine", engine, "--mode", "generated"]
            try:
                result = subprocess.run([str(c) for c in command], text=True,
                                        stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT,
                                        timeout=args.timeout)
                if result.returncode == 0:
                    manifest = json.loads((root / stem / engine / "manifests/sql" /
                                           f"{stem}.{engine}.json").read_text())
                    row["engines"][engine] = {
                        "status": "captured",
                        "queries": manifest["captured_queries"],
                    }
                else:
                    row["engines"][engine] = {
                        "status": "failed", "reason": result.stdout[-1400:],
                    }
            except subprocess.TimeoutExpired:
                row["engines"][engine] = {
                    "status": "timeout", "reason": f"exceeded {args.timeout}s",
                }
        rows.append(row)
        print(f"{index}/{len(tests)} {path.name}: "
              f"memtx={row['engines']['memtx']['status']} "
              f"vinyl={row['engines']['vinyl']['status']}", flush=True)
        args.report.write_text(json.dumps({"tests": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
