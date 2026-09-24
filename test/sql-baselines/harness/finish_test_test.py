#!/usr/bin/env python3
"""Regression test for SQL-TAP direct-check and premature-exit outcomes."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    harness = Path(__file__).resolve().parent
    for name, accepted in (("direct_tap", True), ("bare_exit", False)):
        with tempfile.TemporaryDirectory(prefix="m0-tap-finish-") as dirname:
            root = Path(dirname)
            work, out = root / "work", root / "out"
            work.mkdir()
            out.mkdir()
            env = dict(os.environ, VDBE_DISPATCHER="generated",
                       SQL_JIT_ENABLE="0")
            command = [str(args.binary.resolve()), str(harness / "run.lua"),
                       str(harness / "fixtures" / (name + ".test.lua")),
                       "--suite=sql-tap", "--engine=memtx",
                       "--out=" + str(out), "--work-dir=" + str(work)]
            result = subprocess.run(command, cwd=work, env=env,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            manifest_path = out / "manifests/sql-tap" / (name + ".memtx.json")
            manifest = json.loads(manifest_path.read_text())
            assert (result.returncode == 0) == accepted, result.stdout
            assert manifest["accepted"] == accepted, result.stdout
            assert manifest["captured_queries"] == 1, result.stdout
            print(name, "accepted" if accepted else "rejected")


if __name__ == "__main__":
    main()
