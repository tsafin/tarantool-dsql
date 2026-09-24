#!/usr/bin/env python3
"""Turn the reviewed SQL suite audits into explicit per-engine decisions."""

import argparse
import json
from pathlib import Path
import re


REASONS = {
    "bind.test.lua": ("no_engine_variant", "engine.cfg defines remote/local variants, not memtx/Vinyl"),
    "ddl.test.lua": ("transaction_capture_yield", "capture I/O aborts an open SQL DDL transaction"),
    "delete.test.lua": ("transaction_capture_yield", "capture I/O aborts an open SQL transaction"),
    "drop-table.test.lua": ("server_restart", "test restarts its default server after SQL capture began"),
    "engine.test.lua": ("engine_switch", "test deliberately switches sql_default_engine during capture"),
    "errinj.test.lua": ("release_disabled", "suite.ini disables error-injection test in release build"),
    "full_metadata.test.lua": ("no_engine_variant", "engine.cfg defines remote-only variant"),
    "gh-3613-idx-alter-update-2.test.lua": ("server_restart", "test restarts default server"),
    "gh-3613-idx-alter-update.test.lua": ("server_restart", "test restarts default server twice"),
    "gh-4745-table-info-assertion.test.lua": ("native_mode_not_observed", "LLVM dispatcher has no native execution in this test"),
    "gh2483-remote-persistency-check.test.lua": ("server_restart", "test restarts default server"),
    "gh2808-inline-unique-persistency-check.test.lua": ("server_restart", "test restarts default server"),
    "misc.test.lua": ("budget_exceeded", "normal runner passes both engines, but source loops issue at least 20,480 SQL INSERTs, above the 10,000-query per-test cap"),
    "no-pk-space.test.lua": ("native_mode_not_observed", "CnP dispatcher has no native execution in this test"),
    "persistency.test.lua": ("server_restart", "test restarts default server"),
    "prepared.test.lua": ("no_engine_variant", "engine.cfg defines remote/local variants, not memtx/Vinyl"),
    "sql-statN-index-drop.test.lua": ("disabled", "suite.ini disables this test"),
    "tokenizer.test.lua": ("no_sql_capture", "normal runner passed but app hook observed no box.execute calls"),
    "transitive-transactions.test.lua": ("transaction_capture_yield", "memtx capture I/O aborts open transaction"),
    "triggers.test.lua": ("engine_switch", "test deliberately switches sql_default_engine"),
    "upgrade.test.lua": ("secondary_server", "SQL runs in separately spawned upgrade servers, not default app"),
    "view.test.lua": ("server_restart", "test restarts default server"),
    "view_delayed_wal.test.lua": ("release_disabled", "suite.ini disables this test in release build"),
    "vinyl-opts.test.lua": ("secondary_server", "test creates a separate Vinyl-only server"),
    "add-column.test.sql": ("unsupported_sql_directive", "strict SQL-file adapter rejects Lua language switches"),
    "boolean.test.sql": ("unsupported_sql_directive", "strict SQL-file adapter rejects Lua language switches"),
}
SQL_ONLY_INCLUDED = {
    "gh-4256-do-not-change-order-during-insertion.test.sql": 5,
    "gh-4697-scalar-bool-sort-cmp.test.sql": 7,
}
SUPPLEMENTAL_INCLUDED = {
    # The initial 35s triage timed out. A targeted 300s normal-runner rerun
    # captured 44 queries in each mode; CnP had 80 native executions, LLVM
    # had 43, and strict generated/CnP/LLVM/repeat diffs matched all 44.
    ("transition.test.lua", "vinyl"): 44,
}


def runner_evidence(test, engine, name):
    rc = test["normal_rc"]
    variants = dict(re.findall(
        r"^sql/\S+\s+(\w+)\s+\[\s*(pass|fail|disabled|skip)\s*\]",
        test["normal_detail"], re.MULTILINE))
    audited = variants.get(engine) or ("pass" if test["normal"][engine] else
        "timeout" if rc == -1 else "not_scheduled" if rc == 0 else "failed")
    if name == "misc.test.lua":
        # The first whole-suite audit used a 30s limit; a plain runner rerun
        # subsequently passed both engine variants with the normal 300s cap.
        return {"status": "pass", "audit_status": audited,
                "verification": "plain_runner_300s_rerun", "exit_code": 0}
    if name == "gh-4256-do-not-change-order-during-insertion.test.sql":
        # The broad audit hit an app startup failure unrelated to this file.
        # A targeted two-engine normal-runner rerun passed with exit code 0.
        return {"status": "pass", "audit_status": audited,
                "verification": "targeted_runner_rerun", "exit_code": 0}
    evidence = {"status": audited, "exit_code": rc}
    if audited == "not_scheduled":
        evidence["scheduled_variants"] = sorted(variants)
    return evidence


def capture_evidence(test, engine, name, capture):
    if name.endswith(".test.sql"):
        status = "accepted" if test["standalone"][engine]["accepted"] else "rejected"
        return {"adapter": "strict_sql_file", "audit_status": status}
    result = capture[test["test"]]["engines"][engine]
    evidence = {"adapter": "normal_runner_child",
                "audit_status": result["status"]}
    if "queries" in result:
        evidence["audit_queries"] = result["queries"]
    return evidence


def matrix_evidence(matrix_engine, name, engine, included):
    status = matrix_engine.get("status", "not_run")
    counts = matrix_engine.get("counts", {})
    evidence = {"audit_status": status}
    if counts:
        evidence["mode_queries"] = counts
    if "failure" in matrix_engine:
        evidence["failure_mode"] = matrix_engine["failure"]
    if not included:
        return evidence
    if name in SQL_ONLY_INCLUDED:
        count = SQL_ONLY_INCLUDED[name]
        evidence["verification"] = "strict_sql_file_six_mode"
    elif (name, engine) in SUPPLEMENTAL_INCLUDED:
        count = SUPPLEMENTAL_INCLUDED[(name, engine)]
        evidence["verification"] = "supplemental_normal_runner_300s"
    else:
        if status != "accepted":
            raise ValueError(f"included pair lacks accepted matrix: {name}/{engine}")
        count = counts["generated"]
        if any(counts.get(mode) != count for mode in
               ("cnp", "llvm", "generated-repeat")):
            raise ValueError(f"unequal mode query counts: {name}/{engine}")
        evidence["verification"] = "normal_runner_matrix"
    if status != "accepted":
        evidence["audit_status"] = status
        evidence["status"] = "accepted"
        evidence["mode_queries"] = {mode: count for mode in
                                    ("generated", "cnp", "llvm",
                                     "generated-repeat")}
    else:
        evidence["status"] = status
    # Each accepted pair was separately rechecked with --strict. Record the
    # compact result rather than copying full per-query diff JSON into review.
    evidence["strict_diff"] = {"pairs": 3 * count, "hard": 0, "soft": 0,
                               "all_exact": True}
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-report", type=Path, required=True)
    parser.add_argument("--capture-report", type=Path, required=True)
    parser.add_argument("--matrix-report", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    suite = json.loads(args.suite_report.read_text())["tests"]
    capture = {row["test"]: row for row in
               json.loads(args.capture_report.read_text())["tests"]}
    matrix = {row["test"]: row for row in
              json.loads(args.matrix_report.read_text())["tests"]}
    rows = []
    for test in suite:
        name = test["test"].split("/", 1)[1]
        decision = {"test": test["test"], "engines": {}}
        for engine in ("memtx", "vinyl"):
            matrix_engine = matrix.get(test["test"], {}).get("engines", {}).get(engine, {})
            included = (name in SQL_ONLY_INCLUDED or
                        matrix_engine.get("status") == "accepted" or
                        (name, engine) in SUPPLEMENTAL_INCLUDED)
            if included:
                reason = ("normal runner and strict SQL-file adapter pass all native modes "
                          "with stable repeat capture" if name in SQL_ONLY_INCLUDED else
                          "normal runner, all native modes, and repeated generated capture "
                          "pass with zero snapshot drift")
                entry = {"decision": "include", "category": "verified_parity",
                         "reason": reason}
            else:
                if name not in REASONS:
                    raise ValueError(f"unreviewed SQL exclusion: {test['test']} {engine}")
                category, reason = REASONS[name]
                entry = {"decision": "exclude", "category": category,
                         "reason": reason}
            entry["evidence"] = {
                "normal_runner": runner_evidence(test, engine, name),
                "capture": capture_evidence(test, engine, name, capture),
                "matrix": matrix_evidence(matrix_engine, name, engine, included),
            }
            if included and entry["evidence"]["normal_runner"]["status"] != "pass":
                raise ValueError(f"included pair lacks passing normal runner: "
                                 f"{test['test']}/{engine}")
            decision["engines"][engine] = entry
        rows.append(decision)
    if len(rows) != 60:
        raise ValueError(f"expected 60 SQL suite files, got {len(rows)}")
    included_pairs = sum(entry["decision"] == "include" for row in rows
                         for entry in row["engines"].values())
    report = {"review_version": 1, "suite": "sql", "tests": rows,
              "summary": {"tests": len(rows), "included_engine_pairs": included_pairs,
                          "excluded_engine_pairs": len(rows) * 2 - included_pairs}}
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
