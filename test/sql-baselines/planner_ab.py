#!/usr/bin/env python3
"""Offline bounded-DP width A/B on reviewed SQL-TAP tests."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DEFAULT_TESTS = ("join.test.lua", "join2.test.lua", "join3.test.lua",
                 "join5.test.lua", "where3.test.lua")
DEFAULT_LUATEST_TESTS = ("collation_test.lua",)
DEFAULT_WIDTHS = (1, 5, 10)
WIDTH_KEYS = ("SQL_PATH_SOLVER_WIDTH_ONE", "SQL_PATH_SOLVER_WIDTH_TWO",
              "SQL_PATH_SOLVER_WIDTH_MANY")
METRICS = ("candidate_count", "fallback_count", "generated", "dominated",
           "truncated", "retained", "elapsed_us")


def widths(value):
    try:
        result = tuple(int(part) for part in value.split(","))
    except ValueError:
        raise argparse.ArgumentTypeError("widths must be three integers")
    if len(result) != 3 or any(n < 1 or n > 64 for n in result):
        raise argparse.ArgumentTypeError("widths must be three integers in 1..64")
    return result


def environment(config):
    env = os.environ.copy()
    env.update(dict(zip(WIDTH_KEYS, map(str, config))))
    env.update(VDBE_DISPATCHER="generated", SQL_JIT_ENABLE="0")
    return env


def selected(policy, names, engine, budget):
    included = {row["test"]: row for row in policy["included"]}
    total = 0
    for name in names:
        if "/" in name or not name.endswith(".test.lua"):
            raise ValueError("select top-level SQL-TAP .test.lua basenames")
        identity = "sql-tap/" + name
        row = included.get(identity)
        if row is None or engine not in row["engines"]:
            raise ValueError(f"not reviewed for {engine}: {identity}")
        count = row.get("evidence", {}).get(engine, {}).get("captured_queries")
        if type(count) is not int or count < 1:
            raise ValueError(f"missing reviewed query budget: {identity}/{engine}")
        total += count
    if total > budget:
        raise ValueError(f"reviewed subset has {total} queries, exceeds {budget}")
    return total


def full_corpus_tests(policy, engine):
    """Return every reviewed SQL-TAP test eligible for this engine."""
    return sorted(row["test"][len("sql-tap/"):]
                  for row in policy["included"]
                  if row["test"].startswith("sql-tap/") and
                  engine in row["engines"])


def selected_luatest(policy, names, engine, budget):
    included = {row["test"]: row for row in policy["included"]}
    total = 0
    for name in names:
        if "/" in name or not name.endswith("_test.lua"):
            raise ValueError("select top-level SQL-luatest *_test.lua basenames")
        identity = "sql-luatest/" + name
        row = included.get(identity)
        if row is None or engine not in row["engines"]:
            raise ValueError(f"not reviewed for {engine}: {identity}")
        evidence = row.get("evidence", {}).get(engine, {}).get("modes", {}).get("generated", {})
        count = evidence.get("captured_queries")
        if evidence.get("status") != "passed" or type(count) is not int or count < 1:
            raise ValueError(f"missing reviewed generated query budget: {identity}/{engine}")
        total += count
    if total > budget:
        raise ValueError(f"reviewed subset has {total} queries, exceeds {budget}")
    return total


def explain_changes(diff, capture_dir, suite):
    """Partition snapshot diffs by SQL text; EXPLAIN output is not result parity."""
    explain, semantic, unknown = [], [], []
    for item in diff.get("diffs", []):
        query_id = item.get("query_id", "")
        parts = query_id.split("/")
        relative_parts = parts[1:] if parts and parts[0] == "snapshots" else parts
        relative = Path(*relative_parts)
        if not relative_parts or relative_parts[0] != suite:
            relative = Path(suite) / relative
        snapshot = capture_dir / "snapshots" / relative
        snapshot = Path(str(snapshot) + ".yaml")
        try:
            content = snapshot.read_text()
            match = re.search(r"(?m)^\s*query_sql:\s*['\"]?\s*(EXPLAIN)\b", content)
        except OSError:
            unknown.append(query_id)
            continue
        (explain if match else semantic).append(query_id)
    return {"explain_output_differences": explain,
            "non_explain_result_or_diagnostic_differences": semantic,
            "unclassified_differences": unknown,
            "semantic_result_parity": not semantic and not unknown}


def merge_capture(source, destination):
    """Merge an independently validated one-test capture, rejecting collisions."""
    for path in source.rglob("*"):
        if not path.is_file():
            continue
        # These files describe only the last child process in an isolated
        # capture. They are not part of the aggregate snapshot/manifest
        # contract and would collide for every subsequent test.
        if path.relative_to(source).as_posix() in (
                "luatest-child-state.json", "luatest-capture-error"):
            continue
        target = destination / path.relative_to(source)
        if target.exists():
            raise ValueError(f"duplicate capture output: {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def run(command, env=None, timeout=300):
    result = subprocess.run([str(part) for part in command], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stdout[-4000:])
    return result.stdout


def binary_identity(binary):
    digest = hashlib.sha256()
    with binary.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(binary), "sha256": digest.hexdigest(),
            "version": run([binary, "--version"]).splitlines()[0]}


def measurements(out, engine):
    rows = {}
    count = 0
    for path in sorted((out / "manifests/sql-tap").glob(f"*.{engine}.json")):
        manifest = json.loads(path.read_text())
        if manifest.get("planner_metrics_version") != 2:
            raise ValueError("binary/harness does not expose planner_metrics v2")
        count += manifest["captured_queries"]
        for metric in manifest["planner_metrics"]:
            key = f"{manifest['test_file']}:{metric['query_index']}"
            if key in rows or any(type(metric.get(field)) not in (int, float)
                                  for field in METRICS):
                raise ValueError(f"invalid planner metrics: {key}")
            rows[key] = metric
    if not rows:
        raise ValueError("empty planner metric sample")
    return count, rows


def metric_delta(base, candidate):
    if set(base) != set(candidate):
        raise ValueError("planner metric query identities differ")
    totals = {field: {"default": sum(row[field] for row in base.values()),
                      "candidate": sum(row[field] for row in candidate.values())}
              for field in METRICS}
    for values in totals.values():
        values["delta"] = values["candidate"] - values["default"]
    changes = []
    for key in sorted(base):
        delta = {field: candidate[key][field] - base[key][field]
                 for field in METRICS}
        structural = any(delta[field] for field in METRICS if field != "elapsed_us")
        path_changed = any(base[key].get(field) != candidate[key].get(field)
                           for field in ("path_class", "fallback_reason"))
        if structural or path_changed:
            changes.append({"query": key, "delta": delta,
                            "default_path": base[key].get("path_class"),
                            "candidate_path": candidate[key].get("path_class")})
    return {"queries_measured": len(base), "totals": totals,
            "default_path_classes": dict(Counter(row.get("path_class") for row in base.values())),
            "candidate_path_classes": dict(Counter(row.get("path_class") for row in candidate.values())),
            "structurally_changed_queries": changes,
            "width_effect_observed": bool(changes)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path,
                        help="source tree used by test-run (defaults to --repo; useful when a worktree lacks submodules)")
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--widths", type=widths, default=(2, 8, 16))
    parser.add_argument("--suite", choices=("sql-tap", "sql-luatest"), default="sql-tap")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--test", action="append")
    selection.add_argument("--full-corpus", action="store_true",
                           help="all reviewed tests in selected suite eligible per engine")
    parser.add_argument("--engine", action="append", choices=("memtx", "vinyl"))
    parser.add_argument("--max-queries", type=int,
                        help="query bound (default 1000 for subset; corpus total for --full-corpus)")
    args = parser.parse_args()
    if args.widths == DEFAULT_WIDTHS:
        raise ValueError("candidate must differ from default widths 1,5,10")
    repo, binary, out = args.repo.resolve(), args.binary.resolve(), args.out.resolve()
    runner_repo = (args.runner_repo or repo).resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError("output must be empty")
    engines = sorted(set(args.engine or ("memtx", "vinyl")))
    policy = json.loads((repo / "test/sql-baselines/corpus.json").read_text())
    if args.suite == "sql-luatest":
        selector = selected_luatest
        names_by_engine = {
            engine: (sorted(row["test"].split("/", 1)[1] for row in policy["included"]
                            if row["test"].startswith("sql-luatest/") and
                            engine in row["engines"])
                     if args.full_corpus else
                     sorted(set(args.test or DEFAULT_LUATEST_TESTS)))
            for engine in engines}
    else:
        selector = selected
        names_by_engine = {
            engine: (full_corpus_tests(policy, engine) if args.full_corpus else
                     sorted(set(args.test or DEFAULT_TESTS)))
            for engine in engines}
    budgets = {}
    for engine in engines:
        names = names_by_engine[engine]
        total = sum(selector(policy, (name,), engine, 2**63 - 1)
                    for name in names)
        max_queries = args.max_queries
        if max_queries is None:
            max_queries = total if args.full_corpus else 1000
        budgets[engine] = selector(policy, names, engine, max_queries)
    out.mkdir(parents=True, exist_ok=True)
    report = {"evaluation_version": 2, "source_commit": run(
        ["git", "-C", repo, "rev-parse", "HEAD"]).strip(),
        "binary": binary_identity(binary),
        "suite": args.suite,
        "tests": {engine: [args.suite + "/" + name for name in names]
                  for engine, names in names_by_engine.items()},
        "default_widths": list(DEFAULT_WIDTHS), "candidate_widths": list(args.widths),
        "dispatcher": "generated", "engines": {},
        "limitations": ["only reviewed tests in the selected suite and engine are captured; non-SQL suites are excluded",
                         "not hosted CI",
                         "planner metrics describe compile-time EXPLAIN snapshots, not runtime latency",
                         "elapsed_us is diagnostic and not a deterministic acceptance gate",
                         "strict snapshot parity includes EXPLAIN rows; SQL-luatest differences are partitioned into EXPLAIN output and non-EXPLAIN result/diagnostic differences",
                         "no native-dispatcher performance or plan-quality claim"]}
    for engine in engines:
        captured = {}
        names = names_by_engine[engine]
        for label, config in (("default", DEFAULT_WIDTHS),
                              ("default-repeat", DEFAULT_WIDTHS),
                              ("candidate", args.widths),
                              ("candidate-repeat", args.widths)):
            capture = out / engine / label
            env = environment(config)
            env["BUILDDIR"] = str(binary.parent.parent)
            with tempfile.TemporaryDirectory(prefix="planner-ab-work-") as temp:
                for index, name in enumerate(names):
                    work = Path(temp) / str(index)
                    work.mkdir()
                    env["LISTEN"] = f"unix/:{work}/listen.sock"
                    try:
                        if args.suite == "sql-luatest":
                            test_capture = Path(temp) / f"luatest-{index}"
                            run(["python3", HERE / "luatest_capture.py", "--repo", repo,
                                 "--runner-repo", runner_repo, "--binary", binary,
                                 "--out", test_capture, "--test", name,
                                 "--suite", args.suite, "--engine", engine,
                                 "--mode", "generated"], env=env)
                            merge_capture(test_capture, capture)
                        else:
                            run([binary, HERE / "harness/run.lua", repo / "test/sql-tap" / name,
                                 f"--engine={engine}", f"--out={capture}",
                                 f"--work-dir={work}"], env=env)
                    except (RuntimeError, subprocess.TimeoutExpired) as exc:
                        report["failure"] = {"engine": engine, "configuration": label,
                                             "test": args.suite + "/" + name,
                                             "detail": str(exc)}
                        report["parity_passed"] = False
                        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                        raise
            if args.suite == "sql-tap":
                run([binary, HERE / "validate.lua", capture])
                count, captured[label] = measurements(capture, engine)
            else:
                run([binary, HERE / "validate.lua", capture])
                manifest_files = list((capture / "manifests/sql-luatest").glob(f"*.{engine}.json"))
                count = sum(json.loads(path.read_text())["captured_queries"]
                            for path in manifest_files)
                captured[label] = {}
            if count != budgets[engine]:
                raise ValueError("captured query count differs from reviewed bound")
        comparisons = {}
        for label, base_label in (("default-repeat", "default"),
                                  ("candidate-repeat", "candidate"),
                                  ("candidate", "default")):
            result = subprocess.run([str(binary), str(HERE / "diff.lua"),
                                     str(out / engine / base_label),
                                     str(out / engine / label), "--format=json"],
                                    text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=300)
            comparisons[label] = json.loads(result.stdout)
            if result.returncode not in (0, 1):
                raise RuntimeError(result.stderr)
        report["engines"][engine] = {
            "captured_queries_per_run": budgets[engine], "comparisons": comparisons,
            "repeat_planner_structure_stable": ({
                label: not metric_delta(captured[label], captured[label + "-repeat"])["width_effect_observed"]
                for label in ("default", "candidate")}
                if args.suite == "sql-tap" else None),
            "planner_metrics": (metric_delta(captured["default"], captured["candidate"])
                                if args.suite == "sql-tap" else None)}
        if args.suite == "sql-luatest":
            report["engines"][engine]["semantic_vs_explain"] = explain_changes(
                comparisons["candidate"], out / engine / "candidate", args.suite)
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    report["parity_passed"] = all(comparison["summary"]["passed"]
        for row in report["engines"].values() for comparison in row["comparisons"].values())
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(out / "report.json"),
                      "parity_passed": report["parity_passed"],
                      "width_effect_observed": {engine: row["planner_metrics"]["width_effect_observed"]
                        for engine, row in report["engines"].items() if row["planner_metrics"] is not None},
                      "semantic_result_parity": {engine: row["semantic_vs_explain"]["semantic_result_parity"]
                        for engine, row in report["engines"].items() if "semantic_vs_explain" in row}}, sort_keys=True))
    if not report["parity_passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
