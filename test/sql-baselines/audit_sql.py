#!/usr/bin/env python3
"""Record normal-runner and standalone outcomes for every sql suite file."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def run(command, cwd, timeout):
    try:
        completed = subprocess.run([str(c) for c in command], cwd=cwd,
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=timeout)
        return completed.returncode, completed.stdout
    except subprocess.TimeoutExpired:
        return -1, f"timeout after {timeout}s"


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
    runner_repo = args.runner_repo.resolve()
    binary = args.binary.resolve()
    root = args.out_root.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("output root must be empty")
    root.mkdir(parents=True, exist_ok=True)
    tests = sorted((repo / "test/sql").glob("*.test.lua")) + \
            sorted((repo / "test/sql").glob("*.test.sql"))
    rows = []
    for index, path in enumerate(tests, 1):
        stem = path.name.split(".test.")[0]
        test_root = root / stem
        test_root.mkdir()
        normal_cmd = [sys.executable, runner_repo / "test/test-run.py",
                      "--builddir", binary.parent.parent,
                      "--vardir", test_root / "runner-vardir",
                      "--suite", "sql", "-j", "-1", "--force", path.name]
        normal_rc, normal_output = run(normal_cmd, runner_repo, args.timeout)
        normal = {}
        for engine in ("memtx", "vinyl"):
            normal[engine] = any(engine in line and "[ pass ]" in line
                                 for line in normal_output.splitlines())
        row = {"test": f"sql/{path.name}", "normal_rc": normal_rc,
               "normal": normal, "normal_detail": normal_output[-900:],
               "standalone": {}}
        for engine in ("memtx", "vinyl"):
            work, out = test_root / engine / "work", test_root / engine / "out"
            work.mkdir(parents=True)
            out.mkdir()
            env = os.environ.copy()
            env["VDBE_DISPATCHER"] = "generated"
            env["SQL_JIT_ENABLE"] = "0"
            command = [binary, repo / "test/sql-baselines/harness/run.lua",
                       path, f"--engine={engine}", f"--out={out}",
                       f"--work-dir={work}"]
            try:
                completed = subprocess.run([str(c) for c in command],
                                           cwd=binary.parent.parent, env=env,
                                           text=True, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT,
                                           timeout=args.timeout)
                status = completed.returncode == 0
                detail = completed.stdout[-900:]
            except subprocess.TimeoutExpired:
                status, detail = False, f"timeout after {args.timeout}s"
            row["standalone"][engine] = {"accepted": status, "detail": detail}
        rows.append(row)
        print(f"{index}/{len(tests)} {path.name}: runner={normal} "
              f"standalone={{memtx:{row['standalone']['memtx']['accepted']}, "
              f"vinyl:{row['standalone']['vinyl']['accepted']}}}", flush=True)
        args.report.write_text(json.dumps({"tests": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
