#!/usr/bin/env python3
"""Run a reviewed SQL-suite parity audit with the planner off/on/off."""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
POLICY = HERE / "corpus.json"
ROUTE_CLASSES = HERE / "planner_flag_route_classes.json"
CORPUS = HERE / "corpus.py"
DIFF = HERE / "diff.lua"

# These tests are part of the general capture corpus, but cannot be included in
# the fixed planner-flag A/B comparison. Keep the exceptions local and
# reasoned: general capture and planner-feature coverage must not be narrowed.
PLANNER_FLAG_EXCLUSIONS = {
    "sql": {
        "sql/iproto.test.lua":
            "box.stat().EXECUTE observes snapshot-EXPLAIN instrumentation",
    },
    "sql-luatest": {
        "sql-luatest/gh_8676_exists_in_multiselect_test.lua":
            "direct VALUES producer has no native planner-flag route",
    },
}


def invoke(command, *, env=None, timeout=3600):
    result = subprocess.run([str(part) for part in command], env=env,
                            text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout,
                            errors="replace")
    return result


def diff(binary, base, candidate, *, ignore_path_class=False):
    command = [binary, DIFF, base, candidate, "--format=json"]
    if ignore_path_class:
        command.append("--ignore-path-class")
    result = invoke(command, timeout=600)
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stdout[-4000:])
    return json.loads(result.stdout)


def route_summary(records):
    transitions = Counter()
    examples = {}
    for record in records:
        if record.get("category") != "PATH-CLASS-SHIFT":
            continue
        values = {item["name"]: (item.get("baseline"), item.get("candidate"))
                  for item in record.get("fields", [])}
        old_route, new_route = values.get("l3_path_class.taken", (None, None))
        old_reason, new_reason = values.get("l3_path_class.reason", (None, None))
        key = (old_route, new_route, old_reason, new_reason)
        transitions[key] += 1
        if key not in examples:
            examples[key] = []
        if len(examples[key]) < 12:
            examples[key].append(record.get("query_id"))
    return [{"from": {"route": key[0], "reason": key[2]},
             "to": {"route": key[1], "reason": key[3]},
             "queries": count, "examples": examples[key]}
            for key, count in sorted(transitions.items(), key=lambda row: str(row[0]))]


def classify_route_transitions(transitions, route_policy):
    """Mark known transition classes; this is not feature acceptance."""
    known = {tuple(item) for item in route_policy.get("classes", [])}
    result = []
    for item in transitions:
        key = (item["from"]["route"], item["from"]["reason"],
               item["to"]["route"], item["to"]["reason"])
        result.append({**item, "class_review":
                       "documented" if key in known else "unreviewed"})
    return result


def select_tests(policy, suite, engine, mode, selected=()):
    """Return the reviewed fixed-mode selection, honoring explicit exclusions."""
    reviewed = {item["test"] for item in policy["included"]
                if item["test"].startswith(suite + "/") and
                engine in item["engines"]}
    requested = set(selected)
    unknown = requested - reviewed
    if unknown:
        raise ValueError(f"not reviewed for {suite}/{engine}: {sorted(unknown)}")
    exclusions = PLANNER_FLAG_EXCLUSIONS.get(suite, {})
    excluded = {test: reason for test, reason in exclusions.items()
                if test in reviewed}
    # The direct-VALUES exception applies only when CnP/LLVM participation is
    # required. Generated dispatch still audits its planner-flag behavior.
    if suite == "sql-luatest" and mode == "generated":
        excluded = {}
    rejected = requested & excluded.keys()
    if rejected:
        details = "; ".join(f"{test}: {excluded[test]}"
                             for test in sorted(rejected))
        raise ValueError(f"excluded from {suite}/{mode} planner-flag audit: "
                         f"{details}")
    tests = reviewed if not requested else requested
    return sorted(tests - excluded.keys()), excluded


def explain_output_diffs(records, candidate_root):
    """EXPLAIN program rows are plan evidence, not executed query results."""
    explain, semantic = [], []
    for record in records:
        query_id = record.get("query_id", "")
        relative = query_id.split("snapshots/", 1)[-1]
        snapshot = candidate_root / "snapshots" / (relative + ".yaml")
        try:
            source = snapshot.read_text()
        except OSError:
            semantic.append(record)
            continue
        is_explain = re.search(
            r"(?m)^\s*query_sql:\s*['\"]?\s*EXPLAIN\b", source) is not None
        if not is_explain:
            is_explain = re.search(
                r"(?m)^\s*query_sql:\s*['\"]?\s*"
                r"(?:--[^\n]*\n\s*)+EXPLAIN\b", source) is not None
        fields = record.get("fields", [])
        only_explain_rows = is_explain and fields and all(
            item.get("name", "").startswith("l1_result.") for item in fields)
        (explain if only_explain_rows else semantic).append(record)
    return explain, semantic


def capture(binary, repo, out, engine, mode, flag, suite, tests, env):
    out.mkdir(parents=True)
    command = ["python3", CORPUS, "capture", "--repo", repo,
               "--binary", binary, "--out", out, "--engine", engine,
               "--mode", mode, "--planner-flag", flag, "--suite", suite]
    for test in tests:
        command.extend(("--test", test))
    result = invoke(command, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"capture failed ({engine}/{mode}/{flag}):\n" +
                           result.stdout[-6000:])
    return result.stdout.splitlines()[-1:] 


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--engine", action="append", choices=("memtx", "vinyl"))
    parser.add_argument("--mode", choices=("generated", "cnp", "llvm"),
                        default="generated")
    parser.add_argument("--suite", choices=("sql", "sql-tap", "sql-luatest"),
                        default="sql-tap")
    parser.add_argument("--test", action="append",
                        help="reviewed SQL-TAP test identity; repeat to select a subset")
    args = parser.parse_args()
    repo, binary, out = args.repo.resolve(), args.binary.resolve(), args.out.resolve()
    if not binary.is_file():
        raise ValueError(f"Tarantool binary does not exist: {binary}")
    if out.exists() and any(out.iterdir()):
        raise ValueError(f"output must be empty: {out}")
    out.mkdir(parents=True, exist_ok=True)
    policy = json.loads(POLICY.read_text())
    route_policy = json.loads(ROUTE_CLASSES.read_text())
    if policy.get("scope") != "full-corpus":
        raise ValueError("planner flag evaluation requires reviewed full-corpus policy")
    selected = sorted(set(args.test or []))
    engines = sorted(set(args.engine or ("memtx", "vinyl")))
    env = os.environ.copy()
    env["VDBE_DISPATCHER"] = "cnp" if args.mode == "cnp" else "generated"
    env["SQL_JIT_ENABLE"] = "1" if args.mode == "llvm" else "0"
    env["TMPDIR"] = env.get("TMPDIR", "/dev/shm")
    source_commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    report = {"evaluation_version": 1, "source_commit": source_commit,
              "binary": str(binary), "suite": args.suite,
              "mode": args.mode, "engines": {},
              "scope": f"all reviewed {args.suite} tests per engine after "
                       "documented mode-specific exclusions" if not selected
                       else f"selected reviewed {args.suite} tests",
              "route_review_required": True}
    with tempfile.TemporaryDirectory(prefix="planner-flag-ab-",
                                     dir=env["TMPDIR"]) as temp_name:
        temp_root = Path(temp_name)
        for engine in engines:
            tests, exclusions = select_tests(
                policy, args.suite, engine, args.mode, selected)
            if not tests:
                raise ValueError(f"empty {args.suite} corpus for {engine}")
            captures = {}
            for flag, label in (("off", "off"), ("on", "on"),
                                ("off", "off-repeat")):
                path = temp_root / engine / label
                path.parent.mkdir(parents=True, exist_ok=True)
                capture(binary, repo, path, engine, args.mode, flag,
                        args.suite, tests, env)
                captures[label] = path
            off_on = diff(binary, captures["off"], captures["on"],
                          ignore_path_class=True)
            off_repeat = diff(binary, captures["off"], captures["off-repeat"])
            route_diff = diff(binary, captures["off"], captures["on"])
            explain_diffs, semantic_diffs = explain_output_diffs(
                off_on.get("diffs", []), captures["on"])
            repeat_explain_diffs, repeat_semantic_diffs = explain_output_diffs(
                off_repeat.get("diffs", []), captures["off-repeat"])
            transitions = classify_route_transitions(
                route_summary(route_diff.get("diffs", [])),
                route_policy.get(args.suite, {}))
            if semantic_diffs or repeat_semantic_diffs:
                source_diffs = semantic_diffs or repeat_semantic_diffs
                details = [{"query_id": item.get("query_id"),
                            "category": item.get("category"),
                            "fields": item.get("fields", [])}
                           for item in source_diffs[:5]]
                raise RuntimeError(f"parity failure for {engine}; "
                                   f"semantic examples={json.dumps(details)}")
            metrics = []
            query_count = 0
            for manifest_path in sorted(
                    (captures["on"] / f"manifests/{args.suite}").glob(
                        f"*.{engine}.json")):
                manifest = json.loads(manifest_path.read_text())
                if manifest.get("planner_flag") != "on":
                    raise ValueError(f"enabled capture lacks flag proof: {manifest_path}")
                query_count += manifest.get("captured_queries", 0)
                metrics.extend(manifest.get("planner_metrics", []))
            route_counts = Counter(item.get("path_class") for item in metrics)
            if not selected and route_counts.get("new_planner", 0) == 0:
                raise ValueError(f"planner flag did not select any new_planner route on {engine}")
            report["engines"][engine] = {
                "tests": len(tests),
                "test_ids": tests,
                "excluded_tests": exclusions,
                "queries": query_count,
                "off_on_semantic_parity": {
                    "passed": not semantic_diffs,
                    "semantic_diffs": len(semantic_diffs),
                    "raw_hard_diffs_including_explain":
                        off_on["summary"]["hard_count"],
                },
                "allowed_explain_output_diff_count": len(explain_diffs),
                "semantic_diff_count": len(semantic_diffs),
                "off_repeat_semantic_parity": {
                    "passed": not repeat_semantic_diffs,
                    "semantic_diffs": len(repeat_semantic_diffs),
                    "raw_hard_diffs_including_explain":
                        off_repeat["summary"]["hard_count"],
                },
                "enabled_route_counts": dict(route_counts),
                "off_on_explain_output_diff_count": len(explain_diffs),
                "off_repeat_explain_output_diff_count":
                    len(repeat_explain_diffs),
                "route_transitions": transitions,
                "route_transition_count": sum(item["queries"] for item in transitions),
            }
            report["engines"][engine]["unreviewed_route_transition_count"] = sum(
                item["queries"] for item in transitions
                if item["class_review"] == "unreviewed")
            report["engines"][engine]["route_review_required"] = any(
                item["class_review"] == "unreviewed" for item in transitions)
            (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    passed = all(not row["route_review_required"] for row in report["engines"].values())
    report["route_review_required"] = not passed
    report["feature_acceptance_passed"] = False
    report["feature_acceptance_blockers"] = [
        "a per-suite report cannot certify the aggregate M3.5 gate",
        "run planner_flag_acceptance.py with all current suite/mode reports",
    ]
    report["semantic_parity_passed"] = True
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(out / "report.json"),
                      "engines": {engine: {
                          "queries": row["queries"],
                          "new_planner_routes": row["enabled_route_counts"].get("new_planner", 0),
                          "route_transitions": row["route_transition_count"]}
                          for engine, row in report["engines"].items()},
                      "route_review_required": report["route_review_required"]},
                     sort_keys=True))


if __name__ == "__main__":
    main()
