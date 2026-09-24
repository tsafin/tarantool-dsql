#!/usr/bin/env python3
"""Resumable, independent per-engine sql-luatest capture/parity audit."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


MODES = ("generated", "cnp", "llvm", "generated-repeat")


def invoke(command, timeout):
    try:
        result = subprocess.run([str(part) for part in command], text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        return result.returncode == 0, result.stdout[-4000:]
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or b""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return False, f"outer timeout {timeout}s\n{output[-2000:]}"


def save(report, rows):
    report.write_text(json.dumps({"suite": "sql-luatest", "tests": rows},
                                 indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=330)
    parser.add_argument("--test", action="append", help="Limit to basename(s)")
    args = parser.parse_args()
    repo = args.repo.resolve()
    binary = args.binary.resolve()
    root = args.out_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    report = args.report.resolve()
    rows = json.loads(report.read_text())["tests"] if report.exists() else {}
    selected = set(args.test or [])
    for path in sorted((repo / "test/sql-luatest").glob("*_test.lua")):
        if selected and path.name not in selected:
            continue
        identity = f"sql-luatest/{path.name}"
        row = rows.setdefault(identity, {})
        for engine in ("memtx", "vinyl"):
            engine_row = row.setdefault(engine, {"modes": {}})
            modes = engine_row["modes"]
            stem = path.name[:-len(".lua")]
            generated_out = root / stem / engine / "generated"
            for mode in MODES:
                if mode in modes:
                    continue
                if mode != "generated" and modes["generated"]["status"] != "passed":
                    modes[mode] = {"status": "not_run",
                                   "reason": "generated capture failed"}
                    continue
                actual = "generated" if mode == "generated-repeat" else mode
                out = root / stem / engine / mode
                command = [sys.executable,
                           repo / "test/sql-baselines/luatest_capture.py",
                           "--repo", repo, "--runner-repo", args.runner_repo,
                           "--binary", binary, "--out", out,
                           "--test", path.name, "--engine", engine,
                           "--mode", actual]
                ok, detail = invoke(command, args.timeout)
                result = {"status": "passed" if ok else "capture_failed",
                          "detail": detail, "output": str(out)}
                if ok:
                    manifest_path = out / "manifests/sql-luatest" / f"{stem}.{engine}.json"
                    manifest = json.loads(manifest_path.read_text())
                    result["captured_queries"] = manifest["captured_queries"]
                    result["eligible_queries"] = manifest["eligible_queries"]
                    result["native_participation_queries"] = \
                        manifest["native_participation_queries"]
                    if mode != "generated":
                        diff_command = [binary, repo / "test/sql-baselines/diff.lua",
                                        generated_out, out, "--format=json"]
                        parity, diff_detail = invoke(diff_command, args.timeout)
                        result["parity"] = "passed" if parity else "drift"
                        result["parity_detail"] = diff_detail
                modes[mode] = result
                save(report, rows)
                print(f"{identity} {engine}/{mode}: {result['status']}"
                      f"{('/' + result['parity']) if 'parity' in result else ''}",
                      flush=True)
    save(report, rows)


if __name__ == "__main__":
    main()
