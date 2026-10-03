#!/usr/bin/env python3
"""Validate and summarize exploratory SQL access-cost benchmark JSONL."""

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics

ENGINES = ("memtx", "vinyl")
ACCESSES = ("primary_point", "secondary_covering", "secondary_payload",
            "secondary_range", "primary_scan", "primary_cycling",
            "secondary_cycling")


def summarize(path):
    groups = defaultdict(dict)
    counter_groups = defaultdict(dict)
    row_iterations = {}
    plans = {}
    fixture = None
    rows = None
    version = None
    provenance = None
    storage_state = None
    for lineno, line in enumerate(Path(path).read_text().splitlines(), 1):
        try:
            item = json.loads(line)
            engine = item["engine"]
            access = item["access"]
            repeat = item["repeat_no"]
            value = item["per_execution_us"]
            if item["schema_version"] != 2 or engine not in ENGINES or \
                    access not in ACCESSES or not isinstance(repeat, int) or \
                    repeat < 1 or not isinstance(value, (int, float)) or \
                    not math.isfinite(value) or value <= 0:
                raise ValueError("invalid benchmark observation")
            if item["cache_state"] != "uncontrolled" or \
                    item["warmup_scope"] != "one_execution" or \
                    item["timing_scope"] != "prepared_execute_and_materialize":
                raise ValueError("mixed timing scope")
            if not isinstance(item["iterations"], int) or item["iterations"] < 1 or \
                    not math.isclose(value * item["iterations"], item["elapsed_us"],
                                     rel_tol=1e-8):
                raise ValueError("inconsistent elapsed time")
            if not item["explain"] or not item["sql"]:
                raise ValueError("missing measured plan or SQL")
            if item["storage_state"] not in ("memory", "dumped"):
                raise ValueError("invalid storage state")
            if engine == "vinyl":
                counters = item["vinyl_counters"]
                if not isinstance(counters, dict) or any(
                        not isinstance(counters.get(name), int) or
                        counters[name] < 0 for name in
                        ("run_count", "disk_read_pages", "disk_lookup",
                         "cache_lookup", "cache_get_rows", "memory_get_rows")):
                    raise ValueError("missing Vinyl read counters")
                if item["storage_state"] == "dumped" and counters["run_count"] < 1:
                    raise ValueError("dumped fixture has no Vinyl run")
            elif item.get("vinyl_counters") is not None:
                raise ValueError("memtx row has Vinyl counters")
            shape = (item["sql"], item["explain"], item["result_rows"])
            key = (access, engine)
            if key in plans and plans[key] != shape:
                raise ValueError("SQL, plan, or result shape changed across repeats")
            plans[key] = shape
            if fixture is None:
                fixture, rows, version = (item["fixture"], item["rows"],
                                          item.get("tarantool_version"))
                provenance = (item["source_commit"], item["binary_sha256"],
                              item["run_id"])
                storage_state = item["storage_state"]
            if (fixture, rows, version) != (item["fixture"], item["rows"],
                                           item.get("tarantool_version")):
                raise ValueError("mixed fixtures")
            if provenance != (item["source_commit"], item["binary_sha256"],
                              item["run_id"]) or storage_state != item["storage_state"]:
                raise ValueError("mixed provenance or storage state")
            if not all(isinstance(value, str) and value for value in provenance):
                raise ValueError("missing provenance")
            if repeat in groups[access, engine]:
                raise ValueError("duplicate repeat")
            groups[access, engine][repeat] = value
            if engine == "vinyl":
                counter_groups[access][repeat] = counters
                row_iterations[access, repeat] = item["iterations"]
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"line {lineno}: {exc}") from exc

    if not groups:
        raise ValueError("empty benchmark")
    result = {"schema_version": 2, "fixture": fixture, "rows": rows,
              "tarantool_version": version,
              "source_commit": provenance[0], "binary_sha256": provenance[1],
              "run_id": provenance[2], "storage_state": storage_state,
              "timing_scope": "prepared_execute_and_materialize", "accesses": {}}
    for access in ACCESSES:
        left = groups[access, "memtx"]
        right = groups[access, "vinyl"]
        if not left or set(left) != set(right) or \
                sorted(left) != list(range(1, len(left) + 1)):
            raise ValueError(f"incomplete or unpaired repeats for {access}")
        if len(left) < 5:
            raise ValueError(f"at least five paired repeats required for {access}")
        memtx = statistics.median(left.values())
        vinyl = statistics.median(right.values())
        result["accesses"][access] = {
            "repeats": len(left), "memtx_median_us": memtx,
            "vinyl_median_us": vinyl, "vinyl_to_memtx_ratio": vinyl / memtx,
            "paired_ratios": [right[i] / left[i] for i in sorted(left)],
            "vinyl_median_run_count": statistics.median(
                counter_groups[access][i]["run_count"] for i in sorted(left)),
            "vinyl_median_counters_per_execution": {
                name: statistics.median(
                    counter_groups[access][i][name] /
                    row_iterations[access, i]
                    for i in sorted(left))
                for name in ("disk_read_pages", "disk_lookup", "cache_lookup",
                             "cache_get_rows", "memory_get_rows")
            },
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("observations", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(args.observations), indent=2, sort_keys=True))
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
