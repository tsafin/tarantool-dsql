#!/usr/bin/env python3
"""Record resumable standalone SQL-TAP capture outcomes for corpus review.

This is an audit, not an inclusion decision or runner-equivalence proof.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def save(path, report):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--engine", choices=("memtx", "vinyl"), required=True)
    parser.add_argument("--mode", choices=("generated", "cnp", "llvm"),
                        default="generated")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=12)
    args = parser.parse_args()
    repo, binary = args.repo.resolve(), args.binary.resolve()
    harness = repo / "test/sql-baselines/harness/run.lua"
    tests = sorted((repo / "test/sql-tap").glob("*.test.lua"))
    identity = {"repo": str(repo), "binary": str(binary),
                "engine": args.engine, "mode": args.mode,
                "timeout_seconds": args.timeout}
    if args.out.exists():
        report = json.loads(args.out.read_text())
        if report["identity"] != identity:
            raise ValueError("report identity differs from requested sweep")
    else:
        report = {"identity": identity, "results": {}}
    env = dict(os.environ,
               VDBE_DISPATCHER="cnp" if args.mode == "cnp" else "generated",
               SQL_JIT_ENABLE="1" if args.mode == "llvm" else "0",
               BUILDDIR=str(binary.parent.parent))
    for index, test in enumerate(tests, 1):
        if test.name in report["results"]:
            continue
        with tempfile.TemporaryDirectory(prefix="m0-sqltap-audit-") as dirname:
            root = Path(dirname)
            work, out = root / "work", root / "out"
            work.mkdir()
            out.mkdir()
            started = time.monotonic()
            timed_out = False
            try:
                process = subprocess.run(
                    [str(binary), str(harness), str(test),
                     f"--engine={args.engine}", f"--out={out}",
                     f"--work-dir={work}"],
                    env=env, cwd=work, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True,
                    timeout=args.timeout)
                output = process.stdout
                rc = process.returncode
            except subprocess.TimeoutExpired as exc:
                timed_out = True
                output = exc.stdout or ""
                if isinstance(output, bytes):
                    output = output.decode(errors="replace")
                rc = None
            manifest_path = out / "manifests/sql-tap" / \
                (test.name[:-9] + f".{args.engine}.json")
            manifest = json.loads(manifest_path.read_text()) if \
                manifest_path.exists() else {}
            failed_tap = [line for line in output.splitlines()
                          if line.startswith("not ok") or "Miscompare" in line]
            result = {
                "status": "timeout" if timed_out else
                          ("accepted" if rc == 0 else "rejected"),
                "returncode": rc,
                "duration_seconds": round(time.monotonic() - started, 3),
                "captured_queries": manifest.get("captured_queries", 0),
                "written_snapshots": manifest.get("written_snapshots", 0),
                "test_exit_code": manifest.get("test_exit_code"),
                "test_load_error": manifest.get("test_load_error"),
                "mode_executed": manifest.get("mode_executed"),
                "tap_failures": failed_tap[:5],
                "output_tail": output[-800:],
            }
            report["results"][test.name] = result
            save(args.out, report)
            print(f"{index}/{len(tests)} {test.name}: {result['status']} "
                  f"queries={result['captured_queries']}", flush=True)
    counts = {}
    for result in report["results"].values():
        status = result["status"]
        counts[status] = counts.get(status, 0) + 1
    print(json.dumps(counts, sort_keys=True))


if __name__ == "__main__":
    main()
