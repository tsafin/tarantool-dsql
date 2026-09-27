#!/usr/bin/env python3
"""Triage sql-luatest files against the single-child capture adapter."""

import argparse
import json
from pathlib import Path
import subprocess
import sys

from luatest_capture import unsupported_luatest_source


LONG_RUN = {
    "ghs_119_too_long_mem_values_test.lua",
    "ghs_122_allocations_in_printf_test.lua",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--engine", choices=("memtx", "vinyl"), default="memtx")
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
    for path in sorted((repo / "test/sql-luatest").glob("*_test.lua")):
        source = path.read_text()
        row = {"test": f"sql-luatest/{path.name}"}
        unsupported = unsupported_luatest_source(source)
        if path.name in LONG_RUN:
            row.update(status="not_run", reason="suite.ini marks this test long_run")
        elif unsupported:
            row.update(status="not_run", reason=unsupported)
        elif source.count("server:new(") != 1:
            row.update(status="not_run", reason="multiple luatest child-server constructions")
        elif ":restart(" in source:
            row.update(status="not_run", reason="luatest child server is restarted")
        else:
            command = [sys.executable,
                       str(repo / "test/sql-baselines/luatest_capture.py"),
                       "--repo", str(repo), "--runner-repo", str(args.runner_repo),
                       "--binary", str(args.binary),
                       "--out", str(root / path.stem), "--test", path.name,
                       "--engine", args.engine, "--mode", "generated"]
            try:
                completed = subprocess.run(command, text=True,
                                           stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT,
                                           timeout=args.timeout)
                row["status"] = "captured" if completed.returncode == 0 else "failed"
                row["reason"] = completed.stdout[-2000:]
            except subprocess.TimeoutExpired:
                row.update(status="timeout", reason=f"exceeded {args.timeout}s")
        rows.append(row)
        print(f"{row['status']}: {row['test']}", flush=True)
    report = {"suite": "sql-luatest", "engine": args.engine,
              "mode": "generated", "tests": rows}
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    summary = {}
    for row in rows:
        summary[row["status"]] = summary.get(row["status"], 0) + 1
    print(json.dumps(summary, sort_keys=True))


if __name__ == "__main__":
    main()
