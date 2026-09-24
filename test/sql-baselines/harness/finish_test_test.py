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
    cases = (("direct_tap", "memtx", True, 0),
             ("bare_exit", "memtx", False, 0),
             ("child_dml", "vinyl", True, 1),
             ("child_ddl", "vinyl", False, 0))
    for name, engine, accepted, non_ddl_minimum in cases:
        with tempfile.TemporaryDirectory(prefix="m0-tap-finish-") as dirname:
            root = Path(dirname)
            work, out = root / "work", root / "out"
            work.mkdir()
            out.mkdir()
            env = dict(os.environ, VDBE_DISPATCHER="generated",
                       SQL_JIT_ENABLE="0")
            command = [str(args.binary.resolve()), str(harness / "run.lua"),
                       str(harness / "fixtures" / (name + ".test.lua")),
                       "--suite=sql-tap", "--engine=" + engine,
                       "--out=" + str(out), "--work-dir=" + str(work)]
            result = subprocess.run(command, cwd=work, env=env,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            manifest_path = out / "manifests/sql-tap" / \
                (name + "." + engine + ".json")
            manifest = json.loads(manifest_path.read_text())
            assert (result.returncode == 0) == accepted, result.stdout
            assert manifest["accepted"] == accepted, result.stdout
            assert manifest["captured_queries"] >= 1, result.stdout
            assert manifest["non_ddl_engine_mismatches"] >= non_ddl_minimum
            if name == "child_ddl":
                assert manifest["engine_mismatch"], result.stdout
            print(name, "accepted" if accepted else "rejected")


if __name__ == "__main__":
    main()
