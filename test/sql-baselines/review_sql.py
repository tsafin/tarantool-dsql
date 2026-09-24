#!/usr/bin/env python3
"""Turn the reviewed SQL suite audits into explicit per-engine decisions."""

import argparse
import json
from pathlib import Path


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
    "gh-4256-do-not-change-order-during-insertion.test.sql",
    "gh-4697-scalar-bool-sort-cmp.test.sql",
}
SUPPLEMENTAL_INCLUDED = {
    # The initial 35s triage timed out. A targeted 300s normal-runner rerun
    # captured 44 queries in each mode; CnP had 80 native executions, LLVM
    # had 43, and strict generated/CnP/LLVM/repeat diffs matched all 44.
    ("transition.test.lua", "vinyl"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-report", type=Path, required=True)
    parser.add_argument("--matrix-report", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    suite = json.loads(args.suite_report.read_text())["tests"]
    matrix = {row["test"]: row for row in
              json.loads(args.matrix_report.read_text())["tests"]}
    rows = []
    for test in suite:
        name = test["test"].split("/", 1)[1]
        decision = {"test": test["test"], "engines": {}}
        for engine in ("memtx", "vinyl"):
            matrix_engine = matrix.get(test["test"], {}).get("engines", {}).get(engine, {})
            if name in SQL_ONLY_INCLUDED or matrix_engine.get("status") == "accepted" or \
               (name, engine) in SUPPLEMENTAL_INCLUDED:
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
