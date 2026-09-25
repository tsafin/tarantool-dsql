#!/usr/bin/env python3
"""Assemble evidence-backed per-engine SQL-TAP M0 inclusion decisions."""

import argparse
from collections import Counter
import json
from pathlib import Path


ENGINES = ("memtx", "vinyl")


def read(path):
    return json.loads(path.read_text()) if path.exists() else {}


def normal_status(name, engine, runner, defaults):
    key = "sql-tap/" + name
    status = runner.get("results", {}).get(key)
    if status is not None:
        return status, "engine-suite"
    status = defaults.get("results", {}).get(key + ":" + engine)
    if status is not None:
        return status, "explicit-default-variant"
    status = defaults.get("results", {}).get(key + ":default")
    if status is not None:
        return status, "unlabelled-default-variant"
    return None, "not-scheduled"


def decision(name, engine, capture, parity, runner, defaults, engine_cfg):
    result = capture.get("results", {}).get(name)
    normal, normal_source = normal_status(name, engine, runner, defaults)
    evidence = {"normal_runner": normal, "normal_runner_source": normal_source}
    if result is not None:
        evidence["capture_status"] = result.get("status")
        evidence["captured_queries"] = result.get("captured_queries")
        evidence["snapshot_bytes"] = result.get("snapshot_bytes")
    outcome = parity.get("results", {}).get(name)
    if outcome is not None:
        evidence["parity_passed"] = outcome["passed"]
        evidence["mode_proof"] = {
            mode: {key: record.get(key) for key in
                   ("captured_queries", "executed_queries", "eligible_queries",
                    "native_participation_queries")}
            for mode, record in outcome["captures"].items()}

    def entry(category, reason, include=False):
        return {"decision": "include" if include else "exclude",
                "category": category, "reason": reason, "evidence": evidence}

    variants = engine_cfg.get(name, engine_cfg.get("*", {}))
    if engine not in variants:
        return entry("no_engine_variant", "engine.cfg does not schedule this engine")
    if normal == "disabled":
        return entry("normal_runner_disabled", "disabled by sql-tap/suite.ini")
    if normal == "skip":
        return entry("normal_runner_skip", "normal SQL-TAP runner skips this test")
    if normal_source == "unlabelled-default-variant":
        return entry("no_engine_variant", "normal runner has only an unlabelled default variant")
    if normal != "pass":
        return entry("normal_runner_unverified", "no passing normal runner result for this engine")
    if name == "gh-2723-concurrency.test.lua":
        return entry("concurrent_sql", "global SQL counters cannot attribute child-fiber executions to one intercepted query")
    if result is None:
        return entry("capture_pending", "generated standalone recapture has not completed")
    if result.get("status") != "accepted":
        failures = result.get("tap_failures", [])
        detail = failures[0] if failures else result.get("test_load_error", "")
        return entry("capture_rejected", "standalone capture rejected: " + detail[:180])
    count = result.get("captured_queries", 0)
    bytes_ = result.get("snapshot_bytes")
    if count > 10000 or isinstance(bytes_, int) and bytes_ > 64 * 1024 * 1024:
        return entry("budget_exceeded", f"{count} SQL statements, {bytes_} snapshot bytes exceed 10,000-statement or 64-MiB cap")
    if bytes_ is None:
        return entry("capture_unmeasured", "snapshot byte count not measured")
    if name in ("array.test.lua", "map.test.lua"):
        line = 1045 if name == "array.test.lua" else 1001
        evidence["source_line"] = line
        return entry("partial_capture", f"net.box cn:execute at source line {line} bypasses box.execute interception")
    if name == "where7.test.lua":
        evidence["source_line"] = 4
        evidence["llvm_native_participation"] = "query 1 of 2146 only"
        return entry("session_disables_native_mode", "query 1 disables sql_jit; only that setting statement enters LLVM")
    if name == "selectG.test.lua":
        evidence["source_lines"] = [47, 57]
        return entry("timing_dependent_sql", "test embeds os.time() elapsed seconds in SQL text, changing query identity across runs")
    if name == "default.test.lua":
        evidence["cnp_tap_failure"] = "default-3.1: Miscompare"
        return entry("native_semantic_failure", "CnP runs native queries but fails TAP assertion default-3.1 on both engines")
    if name == "gh-4659-block-hash-index.test.lua":
        evidence["cnp_native_exec_delta"] = 0
        return entry("no_native_eligible", "all four SQL statements are compile/error-only; CnP has no native execution")
    if outcome is None:
        return entry("parity_pending", "repeat/generated/CnP/LLVM parity has not completed")
    if not outcome["passed"]:
        failed = [mode for mode, record in outcome["captures"].items()
                  if not record.get("accepted") or record.get("validation_rc") != 0]
        drift = [mode for mode, record in outcome["diffs"].items()
                 if record.get("returncode") != 0]
        evidence["failed_modes"] = failed
        evidence["drift_modes"] = drift
        return entry("parity_failure", "native/repeat capture or snapshot diff failed: " + ", ".join(failed + drift))
    return entry("verified_parity", "normal runner, all native modes, and repeated generated capture pass with zero snapshot drift", True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--audit-dir", type=Path, default=Path("/tmp"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    base = args.audit_dir
    captures = {engine: read(base / f"m0-sqltap-{engine}-contract-audit.json")
                for engine in ENGINES}
    parities = {engine: read(base / f"m0-sqltap-{engine}-final-parity.json")
                for engine in ENGINES}
    runners = {engine: read(base / f"m0-sqltap-runner-{engine}.json")
               for engine in ENGINES}
    defaults = read(base / "m0-sqltap-runner-default.json")
    engine_cfg = read(args.repo / "test/sql-tap/engine.cfg")
    names = sorted(path.name for path in (args.repo / "test/sql-tap").glob("*.test.lua"))
    tests = [{"test": "sql-tap/" + name,
              "engines": {engine: decision(name, engine, captures[engine],
                                            parities[engine], runners[engine],
                                            defaults, engine_cfg)
                          for engine in ENGINES}}
             for name in names]
    categories = Counter(engine_result["category"]
                         for test in tests for engine_result in test["engines"].values())
    included = categories["verified_parity"]
    if categories["parity_pending"] or categories["capture_pending"]:
        raise ValueError("review has unverified SQL-TAP engine pairs")
    report = {"review_version": 1, "suite": "sql-tap", "tests": tests,
              "summary": {"tests": len(tests), "included_engine_pairs": included,
                          "excluded_engine_pairs": len(tests) * len(ENGINES) - included,
                          "categories": dict(sorted(categories.items()))},
              "sources": {"generated_capture": {engine: str(base / f"m0-sqltap-{engine}-contract-audit.json") for engine in ENGINES},
                          "parity": {engine: str(base / f"m0-sqltap-{engine}-final-parity.json") for engine in ENGINES},
                          "normal_runner": {engine: str(base / f"m0-sqltap-runner-{engine}.json") for engine in ENGINES},
                          "normal_runner_default": str(base / "m0-sqltap-runner-default.json")}}
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
