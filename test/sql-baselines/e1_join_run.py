#!/usr/bin/env python3
"""Run paired bounded-DP widths against real multiway JOIN execution.

This is an E1 exploratory workload, not a stage-matched q-error measurement.
EXPLAIN QUERY PLAN has per-loop estimates, not the JOIN-output estimate.
"""

import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
WORKLOAD = Path(__file__).resolve().with_name("e1_join_workload.lua")
WIDTH_KEYS = ("SQL_PATH_SOLVER_WIDTH_ONE", "SQL_PATH_SOLVER_WIDTH_TWO",
              "SQL_PATH_SOLVER_WIDTH_MANY")
CONFIGS = {"default": (1, 5, 10), "candidate": (2, 8, 16)}
QUERIES = {"two-hot", "two-rare", "three-filtered", "three-range-equality",
           "four-selective", "three-empty", "left-join"}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = int(position)
    return ordered[low] + (ordered[min(low + 1, len(ordered) - 1)] -
                           ordered[low]) * (position - low)


def summary(values):
    return {"n": len(values), "median": statistics.median(values),
            "p95": percentile(values, 0.95), "p99": percentile(values, 0.99)}


def validate_and_report(records, engines):
    """Reject changed results/provenance and summarize only measured rounds."""
    groups = defaultdict(dict)
    immutable = None
    for row in records:
        if row.get("schema_version") != 1 or row.get("workload_id") != "bounded-dp-joins-v1":
            raise ValueError("invalid JOIN workload schema")
        engine, config, query = (row.get("engine"), row.get("configuration"),
                                 row.get("query_id"))
        if engine not in engines or config not in CONFIGS or query not in QUERIES:
            raise ValueError("unexpected engine/configuration/query")
        if row.get("dispatcher") not in ("generated", "cnp", "llvm"):
            raise ValueError("unknown dispatcher")
        if row.get("widths") != list(CONFIGS[config]):
            raise ValueError("width metadata does not match configuration")
        repeat = row.get("repeat")
        if type(repeat) is not int or repeat not in range(8) or \
                row.get("warmup") is not (repeat < 3):
            raise ValueError("invalid repetition/warmup")
        for name in ("elapsed_us", "prepare_us", "actual_rows"):
            if type(row.get(name)) is not int or row[name] < (0 if name == "actual_rows" else 1):
                raise ValueError("invalid " + name)
        for name in ("binary_sha256", "data_sha256", "result_sha256", "plan_sha256"):
            if not isinstance(row.get(name), str) or not re.fullmatch("[0-9a-f]{64}", row[name]):
                raise ValueError("invalid " + name)
        provenance = tuple(row.get(name) for name in
                           ("source_commit", "binary_sha256", "data_sha256",
                            "statistics_id", "dispatcher"))
        if not all(provenance) or (immutable is not None and immutable != provenance):
            raise ValueError("mixed or missing provenance")
        immutable = provenance
        key = engine, config, query
        if repeat in groups[key]:
            raise ValueError("duplicate repetition")
        groups[key][repeat] = row

    expected = {(engine, config, query)
                for engine in engines for config in CONFIGS for query in QUERIES}
    if set(groups) != expected:
        raise ValueError("missing JOIN workload group")
    report = {"schema_version": 1, "workload_id": "bounded-dp-joins-v1",
              "provenance": dict(zip(("source_commit", "binary_sha256", "data_sha256",
                                      "statistics_id", "dispatcher"), immutable)),
              "configurations": {key: list(value) for key, value in CONFIGS.items()},
              "engines": {}, "limitations": [
                  "EXPLAIN per-loop estimates are not JOIN-output estimates; no JOIN q-error is claimed",
                  "prepare_us includes parse/compile but is captured once per query, not a planning-time distribution",
                  "this small synthetic workload is not a production latency acceptance gate",
              ]}
    for engine in engines:
        per_query = {}
        aggregate_default = []
        aggregate_candidate = []
        aggregate_ratios = []
        for query in sorted(QUERIES):
            base = groups[engine, "default", query]
            cand = groups[engine, "candidate", query]
            if set(base) != set(range(8)) or set(cand) != set(range(8)):
                raise ValueError("missing repetition")
            all_rows = list(base.values()) + list(cand.values())
            if len({(row["sql"], row["actual_rows"], row["result_sha256"])
                    for row in all_rows}) != 1:
                raise ValueError("JOIN result changed across widths/repetitions")
            for config_rows in (base, cand):
                if len({row["plan_sha256"] for row in config_rows.values()}) != 1:
                    raise ValueError("unstable EXPLAIN plan across repetitions")
            measured = range(3, 8)
            base_times = [base[i]["elapsed_us"] for i in measured]
            cand_times = [cand[i]["elapsed_us"] for i in measured]
            ratios = [cand[i]["elapsed_us"] / base[i]["elapsed_us"]
                      for i in measured]
            aggregate_default.extend(base_times)
            aggregate_candidate.extend(cand_times)
            aggregate_ratios.extend(ratios)
            per_query[query] = {
                "actual_rows": base[3]["actual_rows"],
                "result_sha256": base[3]["result_sha256"],
                "default_elapsed_us": summary(base_times),
                "candidate_elapsed_us": summary(cand_times),
                "candidate_over_default_ratio": summary(ratios),
                "default_prepare_us": base[3]["prepare_us"],
                "candidate_prepare_us": cand[3]["prepare_us"],
                "default_plan_sha256": base[3]["plan_sha256"],
                "candidate_plan_sha256": cand[3]["plan_sha256"],
                "plan_changed": base[3]["plan_sha256"] != cand[3]["plan_sha256"],
            }
        report["engines"][engine] = {
            "queries": per_query,
            "aggregate": {
                "default_elapsed_us": summary(aggregate_default),
                "candidate_elapsed_us": summary(aggregate_candidate),
                "candidate_over_default_ratio": summary(aggregate_ratios),
            },
            "plan_changes": sum(item["plan_changed"]
                                for item in per_query.values()),
        }
    return report


def run(binary, out, mode, engines, allow_stale_binary=False):
    if out.exists():
        raise ValueError("output directory already exists")
    version = subprocess.check_output([str(binary), "--version"], text=True)
    match = re.search(r"-g([0-9a-f]{10,40})\b", version)
    if match is None:
        raise ValueError("binary has no embedded git revision")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                   text=True).strip()
    embedded = subprocess.check_output(["git", "rev-parse", match.group(1)],
                                       cwd=ROOT, text=True).strip()
    if embedded != head and not allow_stale_binary:
        raise ValueError("binary revision differs from HEAD; rebuild or use "
                         "--allow-stale-binary for exploratory capture")
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain", "--", "src/box/sql", str(WORKLOAD),
         str(Path(__file__))], cwd=ROOT, text=True).strip()
    if dirty and not allow_stale_binary:
        raise ValueError("SQL or JOIN producer sources are dirty; commit before capture")
    out.mkdir(parents=True)
    records = []
    workload_hash = sha256(WORKLOAD)
    for engine in engines:
        # Reverse process order for Vinyl; never run timing samples concurrently.
        configs = ("candidate", "default") if engine == "vinyl" else ("default", "candidate")
        for config in configs:
            widths = CONFIGS[config]
            path = out / f"{engine}-{config}.jsonl"
            env = os.environ.copy()
            env.update(zip(WIDTH_KEYS, map(str, widths)))
            env.update({"VDBE_DISPATCHER": "cnp" if mode == "cnp" else "generated",
                        "SQL_JIT_ENABLE": "1" if mode == "llvm" else "0",
                        "E1_JOIN_OUTPUT": str(path), "E1_JOIN_ENGINE": engine,
                        "E1_JOIN_CONFIG": config,
                        "E1_JOIN_PROVENANCE": json.dumps({
                            "dispatcher": mode, "source_commit": embedded,
                            "binary_sha256": sha256(binary),
                            "data_sha256": workload_hash,
                            "statistics_id": "volatile-analyze-v1-" + workload_hash,
                            "widths": widths,
                        })})
            with tempfile.TemporaryDirectory(prefix="e1-join-") as work:
                result = subprocess.run([str(binary), str(WORKLOAD)], cwd=work,
                                        env=env, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=180)
            if result.returncode:
                raise RuntimeError(f"{engine}/{config}: {result.stdout[-4000:]}")
            if not path.is_file():
                raise ValueError(f"missing observations: {engine}/{config}")
            with path.open(encoding="utf-8") as stream:
                records.extend(json.loads(line) for line in stream if line.strip())
    report = validate_and_report(records, engines)
    report["binary_version"] = version.splitlines()[0]
    report["repository_head"] = head
    report["decision_grade_provenance"] = embedded == head and not dirty
    (out / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--mode", choices=("generated", "cnp", "llvm"),
                        default="generated")
    parser.add_argument("--engine", choices=("memtx", "vinyl"), action="append")
    parser.add_argument("--allow-stale-binary", action="store_true",
                        help="development only; marks provenance non-decision-grade")
    args = parser.parse_args()
    result = run(args.binary.resolve(), args.out.resolve(), args.mode,
                 sorted(set(args.engine or ("memtx", "vinyl"))),
                 args.allow_stale_binary)
    print(json.dumps({"report": str(args.out / "report.json"),
                      "decision_grade_provenance": result["decision_grade_provenance"],
                      "plan_changes": {engine: item["plan_changes"]
                                       for engine, item in result["engines"].items()}},
                     sort_keys=True))


if __name__ == "__main__":
    main()
