#!/usr/bin/env python3
"""Analyze stage-matched planner estimates and paired execution timings."""

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics
import sys


SCHEMA_VERSION = 1
MIN_MEASURED_REPEATS = 5


def percentile(values, pct):
    """Linear-interpolated percentile; values must be non-empty."""
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct
    low = math.floor(rank)
    high = math.ceil(rank)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def validate_row(row, source, line_number):
    label = f"{source}:{line_number}"
    if not isinstance(row, dict) or row.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"{label}: expected JSON object schema_version={SCHEMA_VERSION}")
    required_strings = ("workload_id", "query_id", "engine", "dispatcher",
                        "configuration", "source_commit", "binary_sha256",
                        "data_sha256", "statistics_id")
    for key in required_strings:
        if not isinstance(row.get(key), str) or not row[key]:
            raise ValueError(f"{label}: {key} must be a non-empty string")
    for key in ("binary_sha256", "data_sha256"):
        if len(row[key]) != 64 or any(
                char not in "0123456789abcdef" for char in row[key]):
            raise ValueError(f"{label}: {key} must be lowercase SHA-256 hex")
    if type(row.get("repeat")) is not int or row["repeat"] < 0:
        raise ValueError(f"{label}: repeat must be a non-negative integer")
    if type(row.get("warmup")) is not bool:
        raise ValueError(f"{label}: warmup must be boolean")
    if type(row.get("elapsed_us")) is not int or row["elapsed_us"] < 1:
        raise ValueError(f"{label}: elapsed_us must be a positive integer")
    stages = row.get("cardinalities")
    if not isinstance(stages, list) or not stages:
        raise ValueError(f"{label}: cardinalities must be a non-empty list")
    seen = set()
    for stage in stages:
        if not isinstance(stage, dict) or not isinstance(stage.get("stage_id"), str) or \
                not stage["stage_id"]:
            raise ValueError(f"{label}: each cardinality needs a non-empty stage_id")
        if stage["stage_id"] in seen:
            raise ValueError(f"{label}: duplicate stage_id {stage['stage_id']!r}")
        seen.add(stage["stage_id"])
        for key in ("estimated_rows", "actual_rows"):
            if type(stage.get(key)) is not int or stage[key] < 0:
                raise ValueError(f"{label}: {key} must be a non-negative integer")
    return row


def q_error(estimate, actual):
    """Return null for an unbounded one-sided zero; (0, 0) is exact."""
    if estimate == 0 and actual == 0:
        return 1.0
    if estimate == 0 or actual == 0:
        return None
    return max(estimate / actual, actual / estimate)


def summarize(rows):
    latencies = [row["elapsed_us"] for row in rows]
    errors = []
    unbounded = 0
    for row in rows:
        for stage in row["cardinalities"]:
            value = q_error(stage["estimated_rows"], stage["actual_rows"])
            if value is None:
                unbounded += 1
            else:
                errors.append(value)
    result = {
        "execution_samples": len(rows),
        "elapsed_us": {
            "median": statistics.median(latencies),
            "p95": percentile(latencies, 0.95),
            "p99": percentile(latencies, 0.99),
        },
        "cardinality_stages": len(errors) + unbounded,
        "unbounded_q_error_stages": unbounded,
        "q_error_finite_stages": len(errors),
    }
    if errors:
        result["q_error"] = {
            "median": statistics.median(errors),
            "p95": percentile(errors, 0.95),
            "max": max(errors),
            "geometric_mean": math.exp(sum(math.log(value) for value in errors) /
                                       len(errors)),
        }
    else:
        result["q_error"] = None
    return result


def analyze(rows, baseline, candidate):
    if not baseline or not candidate or baseline == candidate:
        raise ValueError("baseline and candidate must be distinct non-empty names")
    if not rows:
        raise ValueError("no measurement records")
    groups = defaultdict(list)
    record_keys = set()
    immutable = {}
    stage_inventory = {}
    for row in rows:
        scope = (row["workload_id"], row["engine"], row["dispatcher"])
        identity = scope + (row["configuration"], row["query_id"], row["repeat"])
        if identity in record_keys:
            raise ValueError(f"duplicate observation key: {identity}")
        record_keys.add(identity)
        provenance = (row["source_commit"], row["binary_sha256"],
                      row["data_sha256"], row["statistics_id"])
        if scope in immutable and immutable[scope] != provenance:
            raise ValueError(f"mixed source/binary provenance in scope: {scope}")
        immutable[scope] = provenance
        stage_key = scope + (row["configuration"], row["query_id"])
        stages = frozenset(stage["stage_id"]
                           for stage in row["cardinalities"])
        if stage_key in stage_inventory and stage_inventory[stage_key] != stages:
            raise ValueError(
                "inconsistent cardinality stages across repetitions: "
                f"{'/'.join(stage_key)}")
        stage_inventory[stage_key] = stages
        if not row["warmup"]:
            groups[scope + (row["configuration"],)].append(row)

    summaries = {}
    query_summaries = {}
    for scope_config, observations in sorted(groups.items()):
        key = "/".join(scope_config)
        summaries[key] = summarize(observations)
        by_query = defaultdict(list)
        for observation in observations:
            by_query[observation["query_id"]].append(observation)
        for query_id, query_rows in sorted(by_query.items()):
            if len(query_rows) < MIN_MEASURED_REPEATS:
                raise ValueError(
                    f"insufficient measured repeats for {key}/{query_id}: "
                    f"got {len(query_rows)}, need {MIN_MEASURED_REPEATS}")
            query_summaries[key + "/" + query_id] = summarize(query_rows)
    if not groups:
        raise ValueError("no non-warmup observations")

    comparisons = {}
    scopes = sorted({key[:3] for key in groups})
    for scope in scopes:
        base_rows = { (r["query_id"], r["repeat"]): r
                     for r in groups.get(scope + (baseline,), []) }
        cand_rows = { (r["query_id"], r["repeat"]): r
                     for r in groups.get(scope + (candidate,), []) }
        if not base_rows or not cand_rows:
            raise ValueError(f"missing baseline/candidate observations for {scope}")
        if base_rows.keys() != cand_rows.keys():
            raise ValueError(f"unpaired baseline/candidate observations for {scope}")
        ratios = []
        for key in sorted(base_rows):
            base_stages = {stage["stage_id"] for stage in
                           base_rows[key]["cardinalities"]}
            candidate_stages = {stage["stage_id"] for stage in
                                cand_rows[key]["cardinalities"]}
            if base_stages != candidate_stages:
                raise ValueError(f"unmatched cardinality stages for {scope}/{key}")
            base_actual = {stage["stage_id"]: stage["actual_rows"]
                           for stage in base_rows[key]["cardinalities"]}
            candidate_actual = {
                stage["stage_id"]: stage["actual_rows"]
                for stage in cand_rows[key]["cardinalities"]}
            if base_actual != candidate_actual:
                raise ValueError(
                    f"unmatched actual cardinalities for {scope}/{key}")
            ratios.append(cand_rows[key]["elapsed_us"] /
                          base_rows[key]["elapsed_us"])
        comparisons["/".join(scope)] = {
            "baseline": baseline,
            "candidate": candidate,
            "paired_samples": len(ratios),
            "candidate_over_baseline_latency_ratio": {
                "median": statistics.median(ratios),
                "p95": percentile(ratios, 0.95),
                "geometric_mean": math.exp(sum(math.log(value) for value in ratios) /
                                           len(ratios)),
            },
        }
    return {"schema_version": 1, "groups": summaries,
            "queries": query_summaries,
            "paired_comparisons": comparisons}


def read_jsonl(paths):
    rows = []
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    value = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(f"{path}:{line_number}: {error}") from error
                rows.append(validate_row(value, path, line_number))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("observations", nargs="+", type=Path,
                        help="JSONL observation file(s)")
    parser.add_argument("--baseline", default="default")
    parser.add_argument("--candidate", default="candidate")
    parser.add_argument("--out", type=Path, help="write JSON report (default stdout)")
    args = parser.parse_args(argv)
    try:
        result = analyze(read_jsonl(args.observations), args.baseline,
                         args.candidate)
        output = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.out:
            args.out.write_text(output, encoding="utf-8")
        else:
            sys.stdout.write(output)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
