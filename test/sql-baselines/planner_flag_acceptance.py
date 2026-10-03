#!/usr/bin/env python3
"""Aggregate current reviewed-corpus evidence for the M3.5 flag gate."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


HERE = Path(__file__).resolve().parent
REQUIRED_MODES = ("generated", "cnp", "llvm")
REQUIRED_SUITES = ("sql-tap", "sql", "sql-luatest")
ENGINES = ("memtx", "vinyl")
EXPECTED = {
    ("sql-tap", "generated", "memtx"): (232, 47946),
    ("sql-tap", "generated", "vinyl"): (224, 37990),
    ("sql-tap", "cnp", "memtx"): (232, 47946),
    ("sql-tap", "cnp", "vinyl"): (224, 37990),
    ("sql-tap", "llvm", "memtx"): (232, 47946),
    ("sql-tap", "llvm", "vinyl"): (224, 37990),
    ("sql", "generated", "memtx"): (33, 1077),
    ("sql", "generated", "vinyl"): (34, 1085),
    ("sql", "cnp", "memtx"): (33, 1077),
    ("sql", "cnp", "vinyl"): (34, 1085),
    ("sql", "llvm", "memtx"): (33, 1077),
    ("sql", "llvm", "vinyl"): (34, 1085),
    ("sql-luatest", "generated", "memtx"): (32, 499),
    ("sql-luatest", "generated", "vinyl"): (31, 447),
    ("sql-luatest", "cnp", "memtx"): (31, 498),
    ("sql-luatest", "cnp", "vinyl"): (30, 446),
    ("sql-luatest", "llvm", "memtx"): (31, 498),
    ("sql-luatest", "llvm", "vinyl"): (30, 446),
}
# Required M3.5 ledger evidence is scoped to producers owned by a top-level
# SELECT. Embedded DML/trigger SELECTs remain supported and exercised by the
# producer matrix, but are diagnostic coverage rather than an acceptance gate.
PRODUCER_CASES = {
    "planner_final_paths_test.lua",
    "planner_flag_fallback_parity_test.lua",
    "planner_composite_prefix_range_test.lua",
}
ALLOWED_REPORT_DRIFT = {
    "docs/vdbe/roadmap.md",
    "test/sql-luatest/planner_flag_fallback_parity_test.lua",
    "test/sql-baselines/planner_flag_acceptance.py",
    "test/sql-baselines/test_planner_flag_acceptance.py",
    "test/sql-baselines/planner_flag_producer_matrix.py",
}
EXCLUSIONS = {
    ("sql", "generated"): {
        "sql/iproto.test.lua":
            "box.stat().EXECUTE observes snapshot-EXPLAIN instrumentation",
    },
    ("sql", "cnp"): {
        "sql/iproto.test.lua":
            "box.stat().EXECUTE observes snapshot-EXPLAIN instrumentation",
    },
    ("sql", "llvm"): {
        "sql/iproto.test.lua":
            "box.stat().EXECUTE observes snapshot-EXPLAIN instrumentation",
    },
    ("sql-luatest", "cnp"): {
        "sql-luatest/gh_8676_exists_in_multiselect_test.lua":
            "direct VALUES producer has no native planner-flag route",
    },
    ("sql-luatest", "llvm"): {
        "sql-luatest/gh_8676_exists_in_multiselect_test.lua":
            "direct VALUES producer has no native planner-flag route",
    },
}


def validate_producer_matrix(producer_report, expected_commit, blockers):
    if producer_report is None:
        blockers.append("focused producer runtime matrix is missing")
        return
    if producer_report.get("source_commit") != expected_commit:
        blockers.append("focused producer matrix is stale")
    expected = {(test, mode, engine) for test in PRODUCER_CASES
                for mode in REQUIRED_MODES for engine in ENGINES}
    observed = {}
    for case in producer_report.get("cases", []):
        key = (case.get("test"), case.get("mode"), case.get("engine"))
        if key in observed:
            blockers.append(f"duplicate producer case: {key}")
            continue
        observed[key] = case
    for key in sorted(expected - observed.keys()):
        blockers.append(f"missing focused producer case: {key}")
    for key in sorted(expected & observed.keys()):
        case = observed[key]
        if case.get("status") != "passed" or \
                case.get("component_ledger_version") != 1 or \
                case.get("captured_queries", 0) < 1:
            blockers.append(f"focused producer case failed: {key}")
        if key[1] == "cnp" and case.get("cnp_exec_delta", 0) < 1:
            blockers.append(f"CnP execution was not observed for {key}")
        if key[1] == "llvm" and case.get("llvm_exec_delta", 0) < 1:
            blockers.append(f"LLVM execution was not observed for {key}")


def evaluate(reports, expected_commit, producer_report=None,
             reports_commit=None):
    """Validate a complete, same-revision suite/mode/engine evidence matrix."""
    blockers = []
    indexed = {}
    reports_commit = reports_commit or expected_commit
    required_keys = {(suite, mode) for suite in REQUIRED_SUITES
                     for mode in REQUIRED_MODES}
    for report in reports:
        key = (report.get("suite"), report.get("mode"))
        if key not in required_keys:
            blockers.append(f"unexpected suite/mode report: {key}")
            continue
        if key in indexed:
            blockers.append(f"duplicate report for {key}")
            continue
        indexed[key] = report
        if report.get("source_commit") != reports_commit:
            blockers.append(f"inconsistent suite-report source for {key}: "
                            f"{report.get('source_commit')} != {reports_commit}")
        if report.get("route_review_required") is not False:
            blockers.append(f"route review required for {key}")
        if report.get("semantic_parity_passed") is not True:
            blockers.append(f"semantic parity not certified for {key}")

    for key in sorted(required_keys - indexed.keys()):
        blockers.append(f"missing suite/mode report: {key}")

    for suite, mode in sorted(required_keys & indexed.keys()):
        report = indexed[(suite, mode)]
        if report.get("engines", {}).keys() != set(ENGINES):
            blockers.append(f"{suite}/{mode} must cover both engines")
            continue
        for engine in ENGINES:
            row = report["engines"][engine]
            expected_tests, expected_queries = EXPECTED[(suite, mode, engine)]
            label = f"{suite}/{mode}/{engine}"
            if (row.get("tests"), row.get("queries")) != (
                    expected_tests, expected_queries):
                blockers.append(f"incomplete reviewed corpus for {label}")
            expected_exclusions = EXCLUSIONS.get((suite, mode), {})
            if row.get("excluded_tests", {}) != expected_exclusions:
                blockers.append(f"unexpected exclusion set for {label}")
            if row.get("route_review_required") is not False or \
                    row.get("unreviewed_route_transition_count") != 0:
                blockers.append(f"unreviewed route transition for {label}")
            if row.get("enabled_route_counts", {}).get("new_planner", 0) < 1:
                blockers.append(f"no new-planner route observed for {label}")
            for field in ("off_on_semantic_parity",
                          "off_repeat_semantic_parity"):
                parity = row.get(field, {})
                if parity.get("passed") is not True or \
                        parity.get("semantic_diffs") != 0:
                    blockers.append(f"{field} failed for {label}")
    validate_producer_matrix(producer_report, expected_commit, blockers)

    return {"acceptance_version": 1, "source_commit": expected_commit,
            "suite_report_commit": reports_commit,
            "feature_acceptance_passed": not blockers,
            "feature_acceptance_blockers": blockers,
            "suite_mode_reports": len(indexed)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--report", type=Path, action="append", default=[],
                        help="per-suite report.json; repeat for all 9 suite/mode pairs")
    parser.add_argument("--producer-report", type=Path, required=True,
                        help="focused component-ledger producer runtime matrix")
    parser.add_argument("--out", type=Path,
                        help="write aggregate report JSON to this path")
    args = parser.parse_args()
    repo = args.repo.resolve()
    commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    reports = [json.loads(path.read_text()) for path in args.report]
    report_commits = {report.get("source_commit") for report in reports}
    report_commit = next(iter(report_commits)) if len(report_commits) == 1 else ""
    producer_report = json.loads(args.producer_report.read_text())
    result = evaluate(reports, commit, producer_report, report_commit)
    if not report_commit:
        result["feature_acceptance_passed"] = False
        result["feature_acceptance_blockers"].append(
            "suite reports do not share one source revision")
    else:
        ancestor = subprocess.run(
            ["git", "-C", str(repo), "merge-base", "--is-ancestor",
             report_commit, commit], check=False).returncode == 0
        if not ancestor:
            result["feature_acceptance_blockers"].append(
                "suite-report source is not an ancestor of current HEAD")
        changed = subprocess.check_output(
            ["git", "-C", str(repo), "diff", "--name-only",
             report_commit, commit], text=True).splitlines()
        unexpected = sorted(set(changed) - ALLOWED_REPORT_DRIFT)
        if unexpected:
            result["feature_acceptance_blockers"].append(
                "suite-report revision drift touches unverified files: " +
                ", ".join(unexpected))
        if not ancestor or unexpected:
            result["feature_acceptance_passed"] = False
    if args.out:
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, sort_keys=True))
    return 0 if result["feature_acceptance_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
