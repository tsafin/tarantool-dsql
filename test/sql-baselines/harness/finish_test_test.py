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

    for mode in ("generated", "cnp", "llvm"):
        with tempfile.TemporaryDirectory(prefix="m0-mode-proof-") as dirname:
            root = Path(dirname)
            work, out = root / "work", root / "out"
            work.mkdir()
            out.mkdir()
            env = dict(os.environ,
                       VDBE_DISPATCHER="cnp" if mode == "cnp" else "generated",
                       SQL_JIT_ENABLE="1" if mode == "llvm" else "0")
            command = [str(args.binary.resolve()), str(harness / "run.lua"),
                       str(harness / "fixtures/mode_proof.test.lua"),
                       "--suite=sql-tap", "--engine=memtx",
                       "--out=" + str(out), "--work-dir=" + str(work)]
            result = subprocess.run(command, cwd=work, env=env,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            manifest = json.loads((out / "manifests/sql-tap/"
                                   "mode_proof.memtx.json").read_text())
            assert result.returncode == 0, result.stdout
            assert manifest["accepted"], result.stdout
            assert manifest["executed_query_indices"] == [1, 2, 3], manifest
            expected = [] if mode == "generated" else [1, 2, 3]
            assert manifest["eligible_queries"] == len(expected), manifest
            assert manifest["eligible_query_indices"] == expected, manifest
            assert manifest["native_participation_queries"] == (0 if mode == "generated" else 3), manifest
            assert manifest["native_participation_query_indices"] == expected, manifest
            if mode == "llvm":
                assert 4 in manifest["native_compile_success_query_indices"], manifest
                assert 4 not in manifest["eligible_query_indices"], manifest
            assert manifest["mode_miss_queries"] == [], manifest
            print("mode_proof", mode, "accepted")


if __name__ == "__main__":
    main()
