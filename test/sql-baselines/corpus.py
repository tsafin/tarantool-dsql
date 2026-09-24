#!/usr/bin/env python3
"""Inventory, capture and compare the explicitly reviewed SQL seed corpus.

Capture consumes harness manifest v1; every test gets an empty database work
directory, and the entire result tree is validated before it can be compared.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


SUITES = ("sql", "sql-tap", "sql-luatest")
ENGINES = ("memtx", "vinyl")
HERE = Path(__file__).resolve().parent
POLICY = json.loads((HERE / "corpus.json").read_text())


def inventory(repo, policy=POLICY):
    discovered = set()
    for suite in SUITES:
        suite_dir = repo / "test" / suite
        patterns = ("*.test.lua", "*.test.sql") if suite != "sql-luatest" \
                   else ("*_test.lua",)
        for pattern in patterns:
            discovered.update(f"{suite}/{path.name}" for path in suite_dir.glob(pattern))
    included = {}
    for entry in policy["included"]:
        test = entry["test"]
        if test in included or test not in discovered:
            raise ValueError(f"duplicate or absent corpus test: {test}")
        engines = entry["engines"]
        if not engines or len(engines) != len(set(engines)) or \
           any(e not in ENGINES for e in engines) or not entry.get("reason"):
            raise ValueError(f"invalid engine policy for {test}")
        included[test] = entry
    excluded = {}
    for entry in policy.get("excluded", []):
        test, engines = entry["test"], entry["engines"]
        if test not in discovered or not engines or not entry.get("reason") or \
           len(engines) != len(set(engines)) or any(e not in ENGINES for e in engines):
            raise ValueError(f"invalid exclusion policy for {test}")
        excluded.setdefault(test, {})
        for engine in engines:
            if engine in excluded[test] or engine in included.get(test, {}).get("engines", []):
                raise ValueError(f"duplicate inclusion/exclusion for {test}/{engine}")
            excluded[test][engine] = entry["reason"]
    rows = []
    for test in sorted(discovered):
        engines = included.get(test, {}).get("engines", [])
        excluded_engines = excluded.get(test, {})
        pending_engines = [e for e in ENGINES if e not in engines and
                           e not in excluded_engines]
        if policy["scope"] != "seed-smoke" and pending_engines:
            raise ValueError(f"unreviewed corpus engine: {test}/{pending_engines}")
        status = "included" if engines else "pending" if pending_engines else "excluded"
        rows.append({"test": test, "status": status, "engines": engines,
                     "excluded_engines": excluded_engines,
                     "pending_engines": pending_engines,
                     "reason": included[test]["reason"] if engines else
                               policy.get("pending_reason", "reviewed exclusion")})
    return rows


def run(*argv, env=None, cwd=None, timeout=None):
    subprocess.run([str(item) for item in argv], check=True, env=env, cwd=cwd,
                   timeout=timeout)


def manifests(root):
    result = {}
    for path in sorted((root / "manifests").rglob("*.json")):
        manifest = json.loads(path.read_text())
        key = (manifest["test_file"], manifest["engine"])
        if key in result:
            raise ValueError(f"duplicate manifest identity: {key}")
        result[key] = manifest
    return result


def enforce_budgets(root, actual, policy=POLICY):
    limits = policy["capture_limits"]
    query_limit = limits["max_queries_per_test"]
    byte_limit = limits["max_snapshot_bytes_per_test"]
    if type(query_limit) is not int or query_limit < 1 or \
       type(byte_limit) is not int or byte_limit < 1:
        raise ValueError("invalid capture_limits in corpus policy")
    for (test, engine), manifest in actual.items():
        count = manifest["captured_queries"]
        if count > query_limit:
            raise ValueError(f"query budget exceeded: {test}/{engine}: "
                             f"{count} > {query_limit}")
        name = Path(test).name
        stem = name
        for suffix in (".test.lua", ".test.sql", ".lua"):
            if name.endswith(suffix):
                stem = name[:-len(suffix)]
                break
        suite = test.split("/", 1)[0]
        snapshots = root / "snapshots" / suite / stem
        size = sum(path.stat().st_size for path in snapshots.glob(f"*.{engine}.yaml"))
        if size > byte_limit:
            raise ValueError(f"snapshot byte budget exceeded: {test}/{engine}: "
                             f"{size} > {byte_limit}")


def check_mode_proof(manifest):
    count = manifest["captured_queries"]
    names = ("executed_query_indices", "native_compile_attempt_query_indices",
             "native_compile_success_query_indices",
             "native_participation_query_indices", "eligible_query_indices",
             "mode_miss_queries")
    indices = {}
    for name in names:
        values = manifest.get(name)
        if not isinstance(values, list) or any(type(i) is not int or i < 1 or
                                               i > count for i in values) or \
           values != sorted(set(values)):
            raise ValueError(f"invalid {name} in {manifest['test_file']}")
        indices[name] = set(values)
    executed = indices["executed_query_indices"]
    success = indices["native_compile_success_query_indices"]
    participation = indices["native_participation_query_indices"]
    eligible = indices["eligible_query_indices"]
    expected = executed & (success | participation) if \
               manifest["execution_mode"] != "generated" else set()
    if manifest.get("eligible_queries") != len(eligible) or \
       manifest.get("native_participation_queries") != len(participation) or \
       not participation <= executed or eligible != expected or \
       indices["mode_miss_queries"] != eligible - participation or \
       indices["mode_miss_queries"]:
        raise ValueError(f"invalid native participation in {manifest['test_file']}")
    if manifest["execution_mode"] == "generated" and any(
            indices[name] for name in names if name != "executed_query_indices"):
        raise ValueError(f"generated run claims native work in {manifest['test_file']}")
    if manifest["execution_mode"] != "generated" and not participation:
        raise ValueError(f"native mode has no attributed query in {manifest['test_file']}")


def check_coverage(root, rows, engine, mode):
    expected = {(row["test"], engine) for row in rows
                if row["status"] == "included" and engine in row["engines"]}
    if not expected:
        raise ValueError(f"empty reviewed corpus for engine {engine}")
    actual = manifests(root)
    if set(actual) != expected:
        raise ValueError(f"manifest coverage mismatch: missing={sorted(expected - set(actual))}; "
                         f"unexpected={sorted(set(actual) - expected)}")
    for key, manifest in actual.items():
        if manifest.get("manifest_version") != 1 or not manifest.get("accepted") or \
           manifest.get("execution_mode") != mode or \
           manifest.get("mode_executed") is not True:
            raise ValueError(f"rejected or wrong-mode manifest: {key}")
        counter = {"cnp": "cnp_exec_delta", "llvm": "llvm_exec_delta"}.get(mode)
        if counter and (not isinstance(manifest.get(counter), (int, float)) or
                        manifest[counter] <= 0):
            raise ValueError(f"requested {mode} did not execute for {key}")
        check_mode_proof(manifest)
    enforce_budgets(root, actual)
    print(f"coverage: engine={engine} mode={mode} tests={len(actual)} "
          f"queries={sum(m['captured_queries'] for m in actual.values())}")
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    inv = sub.add_parser("inventory")
    inv.add_argument("--repo", type=Path, required=True)
    inv.add_argument("--out", type=Path)
    cap = sub.add_parser("capture")
    cap.add_argument("--repo", type=Path, required=True)
    cap.add_argument("--binary", type=Path, required=True)
    cap.add_argument("--out", type=Path, required=True)
    cap.add_argument("--engine", choices=("memtx", "vinyl"), required=True)
    cap.add_argument("--mode", choices=("generated", "cnp", "llvm"), required=True)
    cmp = sub.add_parser("compare-coverage")
    cmp.add_argument("--base", type=Path, required=True)
    cmp.add_argument("--candidate", type=Path, required=True)
    cmp.add_argument("--repo", type=Path, required=True)
    cmp.add_argument("--engine", choices=("memtx", "vinyl"), required=True)
    cmp.add_argument("--base-mode", choices=("generated", "cnp", "llvm"), default="generated")
    cmp.add_argument("--candidate-mode", choices=("generated", "cnp", "llvm"), default="generated")
    args = parser.parse_args()
    rows = inventory(args.repo.resolve())
    if args.command == "inventory":
        report = {"policy_version": POLICY["policy_version"],
                  "scope": POLICY["scope"], "tests": rows,
                  "summary": {"total": len(rows),
                              "included": sum(r["status"] == "included" for r in rows),
                              "excluded": sum(r["status"] == "excluded" for r in rows),
                              "pending": sum(r["status"] == "pending" for r in rows)}}
        encoded = json.dumps(report, indent=2) + "\n"
        if args.out:
            args.out.write_text(encoded)
        else:
            print(encoded, end="")
    elif args.command == "capture":
        repo, binary, out = args.repo.resolve(), args.binary.resolve(), args.out.resolve()
        if not binary.is_file():
            raise ValueError(f"binary does not exist: {binary}")
        if out.exists() and any(out.iterdir()):
            raise ValueError(f"capture output must be empty: {out}")
        out.mkdir(parents=True, exist_ok=True)
        selected = [r for r in rows if r["status"] == "included" and
                    args.engine in r["engines"]]
        if not selected:
            raise ValueError(f"empty reviewed corpus for engine {args.engine}")
        env = os.environ.copy()
        env["VDBE_DISPATCHER"] = "cnp" if args.mode == "cnp" else "generated"
        env["SQL_JIT_ENABLE"] = "1" if args.mode == "llvm" else "0"
        harness = HERE / "harness" / "run.lua"
        build_dir = binary.parent.parent
        env["BUILDDIR"] = str(build_dir)
        with tempfile.TemporaryDirectory(prefix="sql-corpus-work-") as temp:
            for index, row in enumerate(selected):
                suite = row["test"].split("/", 1)[0]
                if suite == "sql-luatest" or (suite == "sql" and
                                               row["test"].endswith(".test.lua")):
                    with tempfile.TemporaryDirectory(prefix="sql-runner-capture-") as child_temp:
                        child_out = Path(child_temp) / "capture"
                        name = Path(row["test"]).name
                        stem = name[:-len(".test.lua")] if suite == "sql" else \
                               name[:-len(".lua")]
                        run(sys.executable, HERE / "luatest_capture.py",
                            "--repo", HERE.parent.parent,
                            "--runner-repo", repo,
                            "--binary", binary,
                            "--out", child_out,
                            "--test", name,
                            "--suite", suite,
                            "--engine", args.engine,
                            "--mode", args.mode)
                        child_snap = child_out / "snapshots" / suite / stem
                        target_snap = out / "snapshots" / suite / stem
                        target_snap.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copytree(child_snap, target_snap)
                        child_manifest = child_out / "manifests" / suite / \
                                         f"{stem}.{args.engine}.json"
                        target_manifest = out / "manifests" / suite / child_manifest.name
                        target_manifest.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(child_manifest, target_manifest)
                    continue
                work = Path(temp) / str(index)
                work.mkdir()
                test_env = env.copy()
                # Several SQL TAP tests reconfigure box.cfg.listen and then
                # connect through LISTEN. Give each test its own Unix socket;
                # no shared TCP port or database directory is involved.
                test_env["LISTEN"] = f"unix/:{work}/listen.sock"
                run(binary, harness, repo / "test" / row["test"],
                    f"--engine={args.engine}", f"--out={out}",
                    f"--work-dir={work}", env=test_env, cwd=build_dir,
                    timeout=300)
        run(binary, HERE / "validate.lua", out, cwd=build_dir)
        check_coverage(out, rows, args.engine, args.mode)
    else:
        base = check_coverage(args.base, rows, args.engine, args.base_mode)
        candidate = check_coverage(args.candidate, rows, args.engine,
                                   args.candidate_mode)
        for key in base:
            if base[key]["captured_queries"] != candidate[key]["captured_queries"]:
                raise ValueError(f"query count changed for {key}: "
                                 f"{base[key]['captured_queries']} -> "
                                 f"{candidate[key]['captured_queries']}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired, KeyError) as exc:
        print(f"corpus: {exc}", file=sys.stderr)
        sys.exit(1)
