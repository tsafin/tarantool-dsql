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
    ("sql-luatest", "generated", "memtx"): (32, 498),
    ("sql-luatest", "generated", "vinyl"): (31, 446),
    ("sql-luatest", "cnp", "memtx"): (31, 498),
    ("sql-luatest", "cnp", "vinyl"): (30, 446),
    ("sql-luatest", "llvm", "memtx"): (31, 498),
    ("sql-luatest", "llvm", "vinyl"): (30, 446),
}
PRODUCER_CASES = {
    "sql-luatest/planner_final_paths_test.lua",
    "sql-luatest/planner_insert_select_snapshot_test.lua",
    "sql-luatest/planner_flag_fallback_parity_test.lua",
    "sql-luatest/planner_composite_prefix_range_test.lua",
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


def evaluate(reports, expected_commit):
    """Validate a complete, same-revision suite/mode/engine evidence matrix."""
    blockers = []
    indexed = {}
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
        if report.get("source_commit") != expected_commit:
            blockers.append(f"stale source for {key}: "
                            f"{report.get('source_commit')} != {expected_commit}")
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
            if suite == "sql-luatest":
                missing = PRODUCER_CASES - set(row.get("test_ids", []))
                if missing:
                    blockers.append(f"producer cases missing for {label}: "
                                    f"{sorted(missing)}")

    return {"acceptance_version": 1, "source_commit": expected_commit,
            "feature_acceptance_passed": not blockers,
            "feature_acceptance_blockers": blockers,
            "suite_mode_reports": len(indexed)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--report", type=Path, action="append", default=[],
                        help="per-suite report.json; repeat for all 9 suite/mode pairs")
    parser.add_argument("--out", type=Path,
                        help="write aggregate report JSON to this path")
    args = parser.parse_args()
    repo = args.repo.resolve()
    commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    reports = [json.loads(path.read_text()) for path in args.report]
    result = evaluate(reports, commit)
    if args.out:
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, sort_keys=True))
    return 0 if result["feature_acceptance_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
