#!/usr/bin/env python3
"""Inventory, capture and compare the explicitly reviewed SQL seed corpus.

Capture consumes harness manifest v1; every test gets an empty database work
directory, and the entire result tree is validated before it can be compared.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


SUITES = ("sql", "sql-tap", "sql-luatest")
ENGINES = ("memtx", "vinyl")
PENDING_CATEGORIES = {"capture_pending", "parity_pending", "audit_pending",
                      "unreviewed", "normal_runner_unverified",
                      "capture_unmeasured"}
HERE = Path(__file__).resolve().parent
POLICY = json.loads((HERE / "corpus.json").read_text())


def inventory(repo, policy=POLICY, allow_post_baseline_absent=False,
              suites=SUITES, tests=None):
    if policy.get("scope") not in ("seed-smoke", "full-corpus") or \
       type(policy.get("policy_version")) is not int or \
       policy["policy_version"] < 1:
        raise ValueError("invalid corpus scope or policy version")
    if policy["scope"] == "full-corpus" and not re.fullmatch(
            r"[0-9a-f]{40}", policy.get("baseline_commit", "")):
        raise ValueError("full corpus requires a 40-character baseline commit")
    discovered = set()
    if not suites or any(suite not in SUITES for suite in suites):
        raise ValueError("invalid corpus inventory suite scope")
    for suite in suites:
        suite_dir = repo / "test" / suite
        patterns = ("*.test.lua", "*.test.sql") if suite != "sql-luatest" \
                   else ("*_test.lua",)
        for pattern in patterns:
            discovered.update(f"{suite}/{path.name}" for path in suite_dir.glob(pattern))
    if tests is not None:
        requested = set(tests)
        if not requested or requested - discovered:
            raise ValueError(f"unknown corpus test selection: "
                             f"{sorted(requested - discovered)}")
        discovered.intersection_update(requested)
    included = {}
    for entry in policy["included"]:
        test = entry["test"]
        if test.split("/", 1)[0] not in suites or \
           (tests is not None and test not in tests):
            continue
        if test in included or test not in discovered:
            raise ValueError(f"duplicate or absent corpus test: {test}")
        engines = entry["engines"]
        if not engines or len(engines) != len(set(engines)) or \
           any(e not in ENGINES for e in engines) or \
           not isinstance(entry.get("reason"), str) or \
           not entry["reason"].strip():
            raise ValueError(f"invalid engine policy for {test}")
        if policy["scope"] == "full-corpus" and \
           entry.get("category") != "verified_parity":
            raise ValueError(f"unverified full-corpus inclusion: {test}")
        if policy["scope"] == "full-corpus" and \
           (not isinstance(entry.get("evidence"), dict) or
            set(entry["evidence"]) != set(engines) or
            any(not isinstance(entry["evidence"][e], dict) or
                not entry["evidence"][e] for e in engines)):
            raise ValueError(f"missing full-corpus inclusion evidence: {test}")
        included[test] = entry
    excluded = {}
    for entry in policy.get("excluded", []):
        test, engines = entry["test"], entry["engines"]
        if test.split("/", 1)[0] not in suites or \
           (tests is not None and test not in tests):
            continue
        introduced_after_baseline = entry.get("introduced_after_baseline", False)
        if type(introduced_after_baseline) is not bool or \
           (introduced_after_baseline and policy["scope"] != "full-corpus"):
            raise ValueError(f"invalid post-baseline exclusion marker for {test}")
        if (test not in discovered and
            (not introduced_after_baseline or not allow_post_baseline_absent)) or \
           not engines or \
           not isinstance(entry.get("reason"), str) or \
           not entry["reason"].strip() or \
           len(engines) != len(set(engines)) or any(e not in ENGINES for e in engines):
            raise ValueError(f"invalid exclusion policy for {test}")
        if policy["scope"] == "full-corpus" and \
           (not isinstance(entry.get("category"), str) or
            not entry["category"].strip() or
            entry["category"] in PENDING_CATEGORIES):
            raise ValueError(f"unreviewed full-corpus exclusion: {test}")
        if policy["scope"] == "full-corpus" and \
           (not isinstance(entry.get("evidence"), dict) or
            not entry["evidence"]):
            raise ValueError(f"missing full-corpus exclusion evidence: {test}")
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
    if policy["scope"] == "full-corpus" and tests is None and \
       tuple(suites) == SUITES and any(
            not any(engine in row["engines"] for row in rows)
            for engine in ENGINES):
        raise ValueError("full corpus must include tests on both engines")
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
    attempt = indices["native_compile_attempt_query_indices"]
    success = indices["native_compile_success_query_indices"]
    participation = indices["native_participation_query_indices"]
    eligible = indices["eligible_query_indices"]
    expected = executed & (success | participation) if \
               manifest["execution_mode"] != "generated" else set()
    if manifest.get("eligible_queries") != len(eligible) or \
       manifest.get("native_participation_queries") != len(participation) or \
       not success <= attempt or not participation <= executed or \
       eligible != expected or \
       indices["mode_miss_queries"] != eligible - participation or \
       indices["mode_miss_queries"]:
        raise ValueError(f"invalid native participation in {manifest['test_file']}")
    if manifest["execution_mode"] == "generated" and any(
            indices[name] for name in names if name != "executed_query_indices"):
        raise ValueError(f"generated run claims native work in {manifest['test_file']}")
    if manifest["execution_mode"] != "generated" and not participation:
        raise ValueError(f"native mode has no attributed query in {manifest['test_file']}")


def check_coverage(root, rows, engine, mode, planner_flag=None):
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
        if planner_flag is not None and manifest.get("planner_flag") != planner_flag:
            raise ValueError(f"wrong planner flag in manifest: {key}")
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
    cap.add_argument("--allow-post-baseline-absent", action="store_true",
                     help="allow reviewed post-baseline exclusions absent here")
    cap.add_argument("--planner-flag", choices=("off", "on"),
                     help="capture a reviewed SQL suite with a fixed planner flag")
    cap.add_argument("--suite", choices=("sql", "sql-tap", "sql-luatest"),
                     default="sql-tap",
                     help="suite used by fixed planner-flag capture")
    cap.add_argument("--test", action="append",
                     help="capture only this reviewed suite/test identity; repeatable")
    cmp = sub.add_parser("compare-coverage")
    cmp.add_argument("--base", type=Path, required=True)
    cmp.add_argument("--candidate", type=Path, required=True)
    cmp.add_argument("--repo", type=Path, required=True)
    cmp.add_argument("--engine", choices=("memtx", "vinyl"), required=True)
    cmp.add_argument("--base-mode", choices=("generated", "cnp", "llvm"), default="generated")
    cmp.add_argument("--candidate-mode", choices=("generated", "cnp", "llvm"), default="generated")
    args = parser.parse_args()
    inventory_suites = (args.suite,) if args.command == "capture" and \
        args.planner_flag is not None else SUITES
    inventory_tests = None
    if args.command == "capture" and args.planner_flag is not None and args.test:
        inventory_tests = args.test
    rows = inventory(args.repo.resolve(), allow_post_baseline_absent=getattr(
        args, "allow_post_baseline_absent", False), suites=inventory_suites,
        tests=inventory_tests)
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
        capture_rows = rows
        if args.planner_flag is not None:
            capture_rows = [r for r in rows if r["test"].startswith(
                args.suite + "/")]
            if not capture_rows:
                raise ValueError(f"no {args.suite} tests in reviewed corpus")
        if args.test:
            requested = set(args.test)
            reviewed = {r["test"] for r in capture_rows
                        if r["status"] == "included" and
                        args.engine in r["engines"]}
            unknown = requested - reviewed
            if unknown:
                raise ValueError(f"tests are not included in this reviewed capture: "
                                 f"{sorted(unknown)}")
            capture_rows = [r for r in capture_rows if r["test"] in requested]
        selected = [r for r in capture_rows if r["status"] == "included" and
                    args.engine in r["engines"]]
        if not selected:
            raise ValueError(f"empty reviewed corpus for engine {args.engine}")
        env = os.environ.copy()
        env["VDBE_DISPATCHER"] = "cnp" if args.mode == "cnp" else "generated"
        env["SQL_JIT_ENABLE"] = "1" if args.mode == "llvm" else "0"
        source_commit = subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
        if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
            raise ValueError(f"invalid source repository commit: {repo}")
        env["SQL_BASELINE_SOURCE_COMMIT"] = source_commit
        harness = HERE / "harness" / "run.lua"
        build_dir = binary.parent.parent
        env["BUILDDIR"] = str(build_dir)
        for row in selected:
            suite = row["test"].split("/", 1)[0]
            if suite == "sql-luatest" or (suite == "sql" and
                                           row["test"].endswith(".test.lua")):
                with tempfile.TemporaryDirectory(prefix="sql-runner-capture-") as child_temp:
                    child_out = Path(child_temp) / "capture"
                    name = Path(row["test"]).name
                    stem = name[:-len(".test.lua")] if suite == "sql" else \
                           name[:-len(".lua")]
                    child_command = [
                        sys.executable, HERE / "luatest_capture.py",
                        "--repo", HERE.parent.parent,
                        "--runner-repo", repo,
                        "--binary", binary,
                        "--out", child_out,
                        "--test", name,
                        "--suite", suite,
                        "--engine", args.engine,
                        "--mode", args.mode]
                    if args.planner_flag is not None:
                        child_command.extend(("--planner-flag",
                                              args.planner_flag))
                    run(*child_command, env=env)
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
            # Each process owns a fresh work directory. Keep it only for the
            # lifetime of that process: large SQL-TAP tests can write enough
            # WAL/snapshot data that retaining every database until the end
            # of a full-corpus capture exhausts the shared temporary volume.
            with tempfile.TemporaryDirectory(prefix="sql-corpus-work-") as temp:
                work = Path(temp)
                test_env = env.copy()
                # Several SQL TAP tests reconfigure box.cfg.listen and then
                # connect through LISTEN. Give each test its own Unix socket;
                # no shared TCP port or database directory is involved.
                test_env["LISTEN"] = f"unix/:{work}/listen.sock"
                command = [binary, harness, repo / "test" / row["test"],
                           f"--engine={args.engine}", f"--out={out}",
                           f"--work-dir={work}"]
                if args.planner_flag is not None:
                    command.append(f"--planner-flag={args.planner_flag}")
                timeout = 600 if args.planner_flag is not None else 300
                run(*command, env=test_env, cwd=build_dir, timeout=timeout)
        run(binary, HERE / "validate.lua", out, cwd=build_dir)
        check_coverage(out, capture_rows, args.engine, args.mode,
                       planner_flag=args.planner_flag)
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
