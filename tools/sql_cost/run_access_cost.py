#!/usr/bin/env python3
"""Run the access-cost probe with source/binary provenance checks."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import uuid

from access_cost_report import summarize


ROOT = Path(__file__).resolve().parents[2]
BENCH = Path(__file__).resolve().with_name("access_cost_bench.lua")


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def binary_source_commit(version):
    match = re.search(r"-g([0-9a-f]{10,40})(?:\b|$)", version)
    if match is None:
        raise ValueError("binary version has no embedded Git revision")
    return git("rev-parse", "--verify", match.group(1) + "^{commit}")


def source_is_equivalent(binary_commit):
    # Tool/docs commits after the build are fine; compiled source must be the
    # same. This deliberately covers all src/ files, not merely where.c.
    dirty = subprocess.run(["git", "status", "--porcelain", "--", "src"],
                           cwd=ROOT, text=True, capture_output=True,
                           check=True).stdout.strip()
    if dirty:
        raise ValueError("compiled source tree has uncommitted changes")
    changed = subprocess.run(["git", "diff", "--quiet", binary_commit,
                              "HEAD", "--", "src"], cwd=ROOT, check=False)
    if changed.returncode != 0:
        raise ValueError("compiled source differs from binary revision")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True,
                        help="new directory for observations, report, manifest")
    parser.add_argument("--rows", type=int, default=4096)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--iterations", type=int, default=300)
    parser.add_argument("--storage-state", choices=("memory", "dumped",
                                                    "multi_run"),
                        default="memory")
    args = parser.parse_args(argv)
    if args.rows < 128 or args.rows > 1000000 or args.rows % 16 or \
            args.repeats < 5 or args.iterations < 1:
        parser.error("rows must be 128..1000000 and divisible by 16; "
                     "repeats >= 5; iterations >= 1")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error("binary does not exist")
    out_dir = args.out_dir.resolve()
    if out_dir.exists():
        parser.error("output directory already exists")
    version = subprocess.check_output([str(binary), "--version"], text=True)
    try:
        source_commit = binary_source_commit(version)
        source_is_equivalent(source_commit)
    except (ValueError, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))
    binary_hash = sha256(binary)
    run_id = uuid.uuid4().hex
    env = os.environ.copy()
    env.update({
        "SQL_COST_ROWS": str(args.rows),
        "SQL_COST_REPEATS": str(args.repeats),
        "SQL_COST_ITERATIONS": str(args.iterations),
        "SQL_COST_STORAGE_STATE": args.storage_state,
        "SQL_COST_SOURCE_COMMIT": source_commit,
        "SQL_COST_BINARY_SHA256": binary_hash,
        "SQL_COST_RUN_ID": run_id,
    })
    with tempfile.TemporaryDirectory(prefix="sql-cost-bench-") as vardir:
        result = subprocess.run([str(binary), str(BENCH)], cwd=vardir,
                                env=env, text=True, capture_output=True,
                                check=False)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        parser.error(f"benchmark failed with exit code {result.returncode}")
    out_dir.mkdir(parents=True)
    observations = out_dir / "observations.jsonl"
    observations.write_text(result.stdout)
    try:
        report = summarize(observations)
    except ValueError as exc:
        parser.error(f"invalid benchmark output: {exc}")
    (out_dir / "report.json").write_text(json.dumps(report, indent=2,
                                                    sort_keys=True) + "\n")
    manifest = {
        "run_id": run_id, "source_commit": source_commit,
        "head_commit": git("rev-parse", "HEAD"),
        "binary_sha256": binary_hash, "binary_version": version.strip(),
        "bench_sha256": sha256(BENCH), "rows": args.rows,
        "repeats": args.repeats, "iterations": args.iterations,
        "storage_state": args.storage_state, "platform": platform.platform(),
        "machine": platform.machine(), "python": platform.python_version(),
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2,
                                                      sort_keys=True) + "\n")
    print(out_dir)


if __name__ == "__main__":
    main()
