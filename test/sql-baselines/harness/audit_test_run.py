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


RESULT = re.compile(r"^(?:\[\d+\]\s+)?(sql-tap/\S+)\s+(\S*)\s*"
                    r"\[\s*([^]]+?)\s*\]")


def decode_result(line, all_tests, engine):
    match = RESULT.match(line)
    if not match:
        return None
    raw_test, variant, status = match.groups()
    if raw_test.endswith(">"):
        candidates = [test for test in all_tests
                      if test.startswith(raw_test[:-1])]
        if len(candidates) != 1:
            raise ValueError(f"ambiguous truncated runner name: {raw_test}")
        test = candidates[0]
    else:
        test = raw_test
    key = test if engine != "default" else test + ":" + (variant or "default")
    return key, status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--builddir", type=Path, required=True)
    parser.add_argument("--engine", choices=("memtx", "vinyl", "default"),
                        required=True)
    parser.add_argument("--test", action="append", default=[],
                        help="restrict runner to this test filename")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--test-timeout", type=int, default=300)
    args = parser.parse_args()
    repo, builddir = args.repo.resolve(), args.builddir.resolve()
    all_tests = ["sql-tap/" + path.name
                 for path in (repo / "test/sql-tap").glob("*.test.lua")]
    report = {"engine": args.engine, "repo": str(repo),
              "builddir": str(builddir), "results": {}}
    log_path = args.out.with_suffix(".log")
    with tempfile.TemporaryDirectory(prefix="m0-sqltap-runner-") as vardir:
        command = ["python3", str(repo / "test/test-run.py"),
                   "--builddir", str(builddir), "--suite", "sql-tap",
                   "--force", "--long",
                   "--test-timeout", str(args.test_timeout),
                   "--no-output-timeout", str(args.test_timeout + 20),
                   "--vardir", vardir, "-j", str(args.jobs)]
        if args.engine != "default":
            command.extend(("--conf", args.engine))
        command.extend(args.test)
        report["command"] = command
        started = time.monotonic()
        with log_path.open("w") as log:
            process = subprocess.Popen(command, cwd=repo,
                                       stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT,
                                       text=True, bufsize=1)
            for line in process.stdout:
                log.write(line)
                decoded = decode_result(line, all_tests, args.engine)
                if decoded:
                    test, status = decoded
                    report["results"][test] = status
                    print(test, status, flush=True)
            report["returncode"] = process.wait()
        # Reparse the durable log too, so the report remains complete even if
        # progress printing or terminal output handling changes.
        for line in log_path.read_text().splitlines():
            decoded = decode_result(line, all_tests, args.engine)
            if decoded:
                test, status = decoded
                report["results"][test] = status
        report["duration_seconds"] = round(time.monotonic() - started, 3)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("runner:", args.out, "log:", log_path,
          "tests:", len(report["results"]),
          "exit:", report["returncode"], flush=True)


if __name__ == "__main__":
    main()
