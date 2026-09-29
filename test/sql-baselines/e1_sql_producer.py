#!/usr/bin/env python3
"""Capture a stage-matched live SQL statistics-provider comparison.

This is an S1.7/S1.9 pilot producer, not the reviewed analytical workload or
production ANALYZE path. It executes the TEST_BUILD live snapshot-adapter
fixture in sql_stats_test.lua and records planner estimates and actual SELECT
output cardinalities for six prepared single-table predicates.
"""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
FIXTURE_MATERIAL = (
    "CREATE TABLE sql_stats_adapter_t (id INT PRIMARY KEY, a INT);\n"
    "CREATE INDEX sql_stats_adapter_ix ON sql_stats_adapter_t (a);\n"
    "INSERT INTO sql_stats_adapter_t VALUES "
    "(1, 1), (2, 1), (3, 1), (4, 2), (5, 2), (6, 2), (7, 3), (8, 3);"
).encode()


def git_output(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path,
                        default=ROOT / "build-jit-clang19-debug")
    parser.add_argument("--out", type=Path, required=True,
                        help="new JSONL output path (must not already exist)")
    parser.add_argument("--report", type=Path,
                        help="analyzer report path (default: OUT.report.json)")
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    tarantool = build_dir / "src/tarantool"
    module = build_dir / "test/box/sql_stats_snapshot_test.so"
    luatest = ROOT / "test-run/lib/luatest/bin/luatest"
    for path in (tarantool, module, luatest):
        if not path.is_file():
            parser.error(f"required TEST_BUILD artifact is missing: {path}")
    output = args.out.resolve()
    report = (args.report or output.with_suffix(".report.json")).resolve()
    if output.exists() or report.exists():
        parser.error("output and report paths must not already exist")

    # Refuse to label a dirty SQL/test implementation with only HEAD's commit.
    changed = subprocess.run(
        ["git", "diff", "--quiet", "HEAD", "--", "src/box/sql",
         "test/sql-luatest/sql_stats_test.lua",
         "test/sql-baselines/e1_sql_producer.py",
         "test/sql-baselines/e1_measure.py"], cwd=ROOT, check=False)
    if changed.returncode != 0:
        parser.error("commit SQL, fixture, producer, and analyzer changes "
                     "before capture")

    source_commit = git_output("rev-parse", "HEAD")
    version = subprocess.check_output([str(tarantool), "--version"],
                                      text=True)
    if f"-g{source_commit[:10]}" not in version:
        parser.error(
            "TEST_BUILD binary revision does not match HEAD; reconfigure and "
            "rebuild the selected build directory before capture")
    binary_sha256 = hashlib.sha256(tarantool.read_bytes()).hexdigest()
    data_sha256 = hashlib.sha256(FIXTURE_MATERIAL).hexdigest()
    output.parent.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env.update({
        "TEST_RUN_DIR": str(ROOT / "test-run"),
        "SOURCEDIR": str(ROOT),
        "BUILDDIR": str(build_dir),
        "LUA_PATH": ";".join((
            str(ROOT / "test-run/lib/luatest/?.lua"),
            str(ROOT / "test-run/lib/luatest/?/init.lua"),
            str(ROOT / "test-run/lib/?.lua"), "")),
        "LUA_CPATH": ";".join((str(build_dir / "test/box/?.so"),
                                str(build_dir / "?.so"), "")),
        "E1_SQL_OUTPUT": str(output),
        "E1_WORKLOAD_ID": "sql-stats-live-adapter-v1",
        "E1_SOURCE_COMMIT": source_commit,
        "E1_BINARY_SHA256": binary_sha256,
        "E1_DATA_SHA256": data_sha256,
    })

    with tempfile.TemporaryDirectory(prefix="e1-sql-producer-") as vardir:
        env["VARDIR"] = vardir
        command = [str(tarantool), str(luatest), "-c", "--no-clean",
                   "--verbose", "--pattern", "test_snapshot_estimate_adapter$",
                   "test/sql-luatest/sql_stats_test.lua", "--output", "tap"]
        result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        parser.error(f"live SQL producer failed with exit code {result.returncode}")
    if not output.is_file() or output.stat().st_size == 0:
        parser.error("live SQL test completed without producing observations")

    analyzer = [sys.executable, "-B", str(ROOT / "test/sql-baselines/e1_measure.py"),
                str(output), "--baseline", "no-stats", "--candidate",
                "live-stats", "--allow-statistics-change", "--out", str(report)]
    subprocess.run(analyzer, cwd=ROOT, check=True)
    print(f"observations: {output}")
    print(f"report: {report}")


if __name__ == "__main__":
    main()
