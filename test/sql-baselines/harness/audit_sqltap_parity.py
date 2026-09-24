#!/usr/bin/env python3
"""Resumable per-test SQL-TAP repeat and dispatcher parity audit.

Inputs are triage reports, not inclusion policy. Captures are discarded after
each test; only validated outcomes and diff summaries are retained.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


MODES = ("generated", "repeat", "cnp", "llvm")


def run(command, *, cwd, env=None, timeout=300):
    try:
        result = subprocess.run([str(item) for item in command], cwd=cwd,
                                env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=timeout)
        return result.returncode, result.stdout
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return None, output


def save(path, report):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--engine", choices=("memtx", "vinyl"), required=True)
    parser.add_argument("--capture-report", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--test", action="append", default=[])
    args = parser.parse_args()
    repo, binary = args.repo.resolve(), args.binary.resolve()
    commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"],
                                     text=True).strip()
    tests = {}
    for path in args.capture_report:
        source = json.loads(path.read_text())
        if source["identity"]["engine"] != args.engine:
            raise ValueError(f"capture report has wrong engine: {path}")
        tests.update(source["results"])
    names = sorted(name for name, result in tests.items()
                   if result["status"] == "accepted")
    if args.test:
        missing = set(args.test) - set(names)
        if missing:
            raise ValueError(f"tests lack accepted generated triage: {sorted(missing)}")
        names = [name for name in names if name in args.test]
    identity = {"repo": str(repo), "binary": str(binary),
                "commit": commit, "engine": args.engine,
                "timeout_seconds": args.timeout,
                "tests": names}
    if args.out.exists():
        report = json.loads(args.out.read_text())
        if report["identity"] != identity:
            raise ValueError("existing report identity differs")
    else:
        report = {"identity": identity, "results": {}}
    harness = repo / "test/sql-baselines/harness/run.lua"
    validator = repo / "test/sql-baselines/validate.lua"
    differ = repo / "test/sql-baselines/diff.lua"
    for index, name in enumerate(names, 1):
        if name in report["results"]:
            continue
        current = subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
        if current != commit:
            raise RuntimeError("repository commit changed during parity audit")
        started = time.monotonic()
        outcome = {"captures": {}, "diffs": {}}
        with tempfile.TemporaryDirectory(prefix="m0-sqltap-parity-") as dirname:
            root = Path(dirname)
            for mode in MODES:
                work, out = root / (mode + "-work"), root / (mode + "-out")
                work.mkdir()
                out.mkdir()
                env = dict(os.environ,
                           VDBE_DISPATCHER="cnp" if mode == "cnp" else "generated",
                           SQL_JIT_ENABLE="1" if mode == "llvm" else "0",
                           BUILDDIR=str(binary.parent.parent),
                           LISTEN="unix/:" + str(work / "listen.sock"))
                command = [binary, harness, repo / "test/sql-tap" / name,
                           "--engine=" + args.engine, "--out=" + str(out),
                           "--work-dir=" + str(work)]
                rc, output = run(command, cwd=work, env=env, timeout=args.timeout)
                manifest_path = out / "manifests/sql-tap" / \
                    (name[:-9] + "." + args.engine + ".json")
                manifest = json.loads(manifest_path.read_text()) if \
                    manifest_path.exists() else {}
                capture = {"returncode": rc,
                           "accepted": manifest.get("accepted") is True,
                           "captured_queries": manifest.get("captured_queries", 0),
                           "cnp_exec_delta": manifest.get("cnp_exec_delta"),
                           "llvm_exec_delta": manifest.get("llvm_exec_delta"),
                           "error_tail": output[-500:] if rc != 0 else ""}
                if rc == 0:
                    validate_rc, validate_output = run(
                        [binary, validator, out], cwd=work, timeout=args.timeout)
                    capture["validation_rc"] = validate_rc
                    if validate_rc != 0:
                        capture["error_tail"] = validate_output[-500:]
                outcome["captures"][mode] = capture
                if rc != 0 or capture.get("validation_rc") != 0:
                    break
            if len(outcome["captures"]) == len(MODES):
                base = root / "generated-out"
                for mode in ("repeat", "cnp", "llvm"):
                    candidate = root / (mode + "-out")
                    options = ["--format=json"]
                    if mode == "repeat":
                        options.append("--strict")
                    else:
                        options.append("--ignore-path-class")
                    rc, output = run([binary, differ, base, candidate] + options,
                                     cwd=root, timeout=args.timeout)
                    try:
                        diff = json.loads(output)
                        outcome["diffs"][mode] = {
                            "returncode": rc, "summary": diff["summary"],
                            "first_diffs": diff["diffs"][:3]}
                    except (ValueError, KeyError):
                        outcome["diffs"][mode] = {
                            "returncode": rc, "error_tail": output[-500:]}
        outcome["duration_seconds"] = round(time.monotonic() - started, 3)
        outcome["passed"] = (len(outcome["captures"]) == len(MODES) and
                             all(c["accepted"] and c["returncode"] == 0 and
                                 c.get("validation_rc") == 0
                                 for c in outcome["captures"].values()) and
                             len(outcome["diffs"]) == 3 and
                             all(d.get("returncode") == 0
                                 for d in outcome["diffs"].values()))
        report["results"][name] = outcome
        save(args.out, report)
        print(f"{index}/{len(names)} {name}: "
              f"{'pass' if outcome['passed'] else 'FAIL'} "
              f"{outcome['duration_seconds']}s", flush=True)
    print("passed", sum(result["passed"] for result in report["results"].values()),
          "of", len(report["results"]))


if __name__ == "__main__":
    main()
