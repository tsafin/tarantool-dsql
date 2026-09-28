#!/usr/bin/env python3
"""Turn a full SQL-luatest matrix into reviewed per-engine corpus decisions."""

import argparse
from collections import Counter
import json
from pathlib import Path

from luatest_capture import unsupported_luatest_source


EXCLUSIONS = {
    "bind_test.lua": ("partial_capture",
                      "direct net.box SQL bypasses the child box.execute hook"),
    "cnp_bitwise_mix_test.lua": ("forced_dispatcher",
                                "test forces VDBE_DISPATCHER=cnp and executes prepared statement IDs, so generated/native parity is not capturable"),
    "compat_test.lua": ("partial_capture",
                        "two extra child servers override the pre-box.cfg hook and execute uncaptured SQL"),
    "constraint_test.lua": ("multiple_children",
                            "test starts two child servers; one-child capture cannot preserve both query streams"),
    "datetime_test.lua": ("nondeterministic_result",
                          "datetime.now() changes captured rows across generated repeats (query 164)"),
    "explain_modifiers_test.lua": ("nondeterministic_result",
                                    "EXPLAIN disassembly embeds process addresses; generated repeat drifts and native execution is unobserved"),
    "gh_5526_no_error_on_too_many_indexes_test.lua":
        ("no_native_execution", "normal runner passes, but CnP and LLVM produce no native execution event"),
    "gh_6766_mp_ext_via_netbox_test.lua":
        ("partial_capture", "all SQL uses direct net.box execute; no child box.execute statement is captured"),
    "gh_8365_no_func_in_index_def_test.lua":
        ("no_native_execution", "normal runner passes, but CnP and LLVM produce no native execution event"),
    "gh_9469_too_big_decimals_test.lua":
        ("no_native_execution", "normal runner passes, but CnP and LLVM produce no native execution event"),
    "ghs_119_too_long_mem_values_test.lua":
        ("long_run", "suite.ini marks this long_run; standard runner skips it, so no normal-runner capture is accepted"),
    "ghs_120_use_after_free_test.lua":
        ("no_native_execution", "normal runner passes, but CnP and LLVM produce no native execution event"),
    "ghs_122_allocations_in_printf_test.lua":
        ("long_run", "suite.ini marks this long_run; standard runner skips it, so no normal-runner capture is accepted"),
    "interval_test.lua":
        ("partial_capture", "direct net.box execute bypasses the child box.execute hook; Vinyl also fails normal-runner assertions"),
    "sql_func_expr_test.lua":
        ("restarted_child", "test restarts its child server; one-child capture cannot preserve its query sequence"),
    "sql_stats_test.lua":
        ("multiple_children", "test starts multiple child servers and executes prepared statement IDs"),
}


def clean(modes):
    return all(modes[mode]["status"] == "passed" for mode in
               ("generated", "cnp", "llvm", "generated-repeat")) and \
           all(modes[mode]["parity"] == "passed" for mode in
               ("cnp", "llvm", "generated-repeat"))


def matrix_evidence(modes, source_guard):
    """Retain reproducible outcomes without copying bulky runner/diff logs."""
    evidence = {"modes": {}}
    if source_guard:
        evidence["source_guard"] = source_guard
    for mode in ("generated", "cnp", "llvm", "generated-repeat"):
        row = modes[mode]
        outcome = {"status": row["status"]}
        if "captured_queries" in row:
            outcome["captured_queries"] = row["captured_queries"]
        if mode in ("cnp", "llvm") and "eligible_queries" in row:
            outcome["eligible_queries"] = row["eligible_queries"]
            outcome["native_participation_queries"] = \
                row["native_participation_queries"]
        if "parity" in row:
            outcome["parity"] = row["parity"]
        evidence["modes"][mode] = outcome
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    matrix = json.loads(args.matrix.read_text())["tests"]
    expected = {f"sql-luatest/{p.name}" for p in
                (args.repo / "test/sql-luatest").glob("*_test.lua")}
    if set(matrix) != expected:
        raise ValueError(f"matrix test inventory mismatch: {expected ^ set(matrix)}")
    reviews = []
    counts = Counter()
    for test in sorted(matrix):
        basename = test.split("/", 1)[1]
        source = (args.repo / "test" / test).read_text()
        guard = unsupported_luatest_source(source)
        engines = {}
        for engine in ("memtx", "vinyl"):
            modes = matrix[test][engine]["modes"]
            if basename in EXCLUSIONS:
                category, reason = EXCLUSIONS[basename]
                if basename == "datetime_test.lua" and engine == "vinyl":
                    category = "normal_runner_failure"
                    reason = "Vinyl generated capture fails existing luatest assertions; memtx also has datetime.now() repeat drift"
                engines[engine] = {"decision": "exclude", "category": category,
                                   "reason": reason}
            elif basename == "show_create_table_test.lua" and engine == "vinyl":
                engines[engine] = {
                    "decision": "exclude", "category": "engine_assertion",
                    "reason": "normal runner expects literal WITH ENGINE = 'memtx' but Vinyl output reports 'vinyl'",
                }
            elif guard:
                raise ValueError(f"unreviewed capture bypass in {test}: {guard}")
            elif clean(modes):
                queries = modes["generated"]["captured_queries"]
                engines[engine] = {
                    "decision": "include", "category": "verified_parity",
                    "reason": f"{queries} SQL statements captured by normal runner; CnP, LLVM, and generated repeat match with per-query native proof",
                }
            else:
                raise ValueError(f"unreviewed matrix failure: {test}/{engine}")
            engines[engine]["evidence"] = matrix_evidence(modes, guard)
            counts[engines[engine]["decision"]] += 1
        reviews.append({"test": test, "engines": engines})
    result = {"review_version": 1, "suite": "sql-luatest", "tests": reviews,
              "summary": {"tests": len(reviews), "engine_pairs": sum(counts.values()),
                          "included_engine_pairs": counts["include"],
                          "excluded_engine_pairs": counts["exclude"]}}
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
