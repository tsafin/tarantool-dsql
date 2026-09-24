#!/usr/bin/env python3
"""Capture a machine-readable normal SQL-TAP runner outcome map.

Run one engine at a time. The runner's engine.cfg and suite.ini remain the
authority for supported configurations and disabled tests.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time


RESULT = re.compile(r"^(sql-tap/\S+\.test\.lua)\s+.*?\[\s*([^]]+?)\s*\]")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--builddir", type=Path, required=True)
    parser.add_argument("--engine", choices=("memtx", "vinyl"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--test-timeout", type=int, default=300)
    args = parser.parse_args()
    repo, builddir = args.repo.resolve(), args.builddir.resolve()
    report = {"engine": args.engine, "repo": str(repo),
              "builddir": str(builddir), "results": {}}
    log_path = args.out.with_suffix(".log")
    with tempfile.TemporaryDirectory(prefix="m0-sqltap-runner-") as vardir:
        command = ["python3", str(repo / "test/test-run.py"),
                   "--builddir", str(builddir), "--suite", "sql-tap",
                   "--conf", args.engine, "--force", "--long",
                   "--test-timeout", str(args.test_timeout),
                   "--vardir", vardir, "-j", str(args.jobs)]
        report["command"] = command
        started = time.monotonic()
        with log_path.open("w") as log:
            process = subprocess.Popen(command, cwd=repo,
                                       stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT,
                                       text=True, bufsize=1)
            for line in process.stdout:
                log.write(line)
                match = RESULT.match(line)
                if match:
                    test, status = match.groups()
                    report["results"][test] = status
                    print(test, status, flush=True)
            report["returncode"] = process.wait()
        report["duration_seconds"] = round(time.monotonic() - started, 3)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("runner:", args.out, "log:", log_path,
          "tests:", len(report["results"]),
          "exit:", report["returncode"], flush=True)


if __name__ == "__main__":
    main()
