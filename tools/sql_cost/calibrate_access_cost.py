#!/usr/bin/env python3
"""Fit and validate an empirical, engine-specific SQL access-price pilot.

The model is deliberately fixture-local and never changes WhereLoop costs.
It separates a full primary scan's fixed work from returned-row work, and a
secondary probe's startup from returned-row work. Units are microseconds per
prepared, materialized SQL execution, not storage-engine primitives.
"""

import argparse
import hashlib
import json
from pathlib import Path

from access_cost_report import ENGINES


BENCH = Path(__file__).resolve().with_name("access_cost_bench.lua")
SHAPES = {
    "filtered": ("primary_filtered", "secondary_payload", 16),
    "range": ("primary_range", "secondary_range", 16),
    "broad": ("primary_broad", "secondary_broad", None),
    "tail": ("primary_tail", "secondary_tail", 16),
}
COMMON_MANIFEST = ("bench_sha256", "binary_sha256", "source_commit",
                   "rows", "iterations", "repeats")


def load_capture(directory):
    directory = Path(directory)
    manifest = json.loads((directory / "manifest.json").read_text())
    report = json.loads((directory / "report.json").read_text())
    for name in ("source_commit", "binary_sha256", "rows", "storage_state"):
        if report[name] != manifest[name]:
            raise ValueError("report/manifest mismatch: " + name)
    if report["schema_version"] != 2 or report["run_id"] != manifest["run_id"]:
        raise ValueError("report/manifest mismatch: schema or run")
    if manifest["bench_sha256"] != hashlib.sha256(BENCH.read_bytes()).hexdigest():
        raise ValueError("capture uses a different benchmark revision")
    return manifest, report


def fit(report):
    rows = report["rows"]
    if rows < 128 or rows % 16:
        raise ValueError("fixture row count is unsuitable for fitting")
    result = {"rows": rows, "training_state": report["storage_state"],
              "unit": "prepared_materialized_sql_us", "engines": {}}
    for engine in ENGINES:
        costs = lambda access: report["accesses"][access][engine + "_median_us"]
        primary_base = costs("primary_filtered")
        primary_rate = (costs("primary_scan") - primary_base) / (rows - 16)
        secondary_base = costs("secondary_payload")
        secondary_rate = (costs("secondary_broad") - secondary_base) / \
            (rows // 2 - 16)
        if min(primary_base, secondary_base, primary_rate, secondary_rate) < 0:
            raise ValueError("negative empirical coefficient")
        result["engines"][engine] = {
            "primary_scan_base_us": primary_base,
            "primary_output_us_per_row": primary_rate,
            "secondary_startup_us": secondary_base,
            "secondary_output_us_per_row": secondary_rate,
        }
    return result


def predict(parameters, path, output_rows):
    if path == "primary":
        return parameters["primary_scan_base_us"] + \
            parameters["primary_output_us_per_row"] * (output_rows - 16)
    if path == "secondary":
        return parameters["secondary_startup_us"] + \
            parameters["secondary_output_us_per_row"] * (output_rows - 16)
    raise ValueError("unknown access path")


def evaluate(model, report):
    if report["rows"] != model["rows"]:
        raise ValueError("training and validation fixture sizes differ")
    observations = {}
    matches = 0
    total = 0
    for engine in ENGINES:
        observations[engine] = {}
        for name, (primary_access, secondary_access, fixed_rows) in SHAPES.items():
            output_rows = fixed_rows if fixed_rows is not None else model["rows"] // 2
            parameters = model["engines"][engine]
            predicted_primary = predict(parameters, "primary", output_rows)
            predicted_secondary = predict(parameters, "secondary", output_rows)
            actual_primary = report["accesses"][primary_access][engine + "_median_us"]
            actual_secondary = report["accesses"][secondary_access][engine + "_median_us"]
            same_rank = (predicted_secondary < predicted_primary) == \
                (actual_secondary < actual_primary)
            matches += same_rank
            total += 1
            observations[engine][name] = {
                "output_rows": output_rows,
                "predicted_secondary_over_primary":
                    predicted_secondary / predicted_primary,
                "observed_secondary_over_primary":
                    actual_secondary / actual_primary,
                "ranking_match": same_rank,
                "primary_prediction_over_observed":
                    predicted_primary / actual_primary,
                "secondary_prediction_over_observed":
                    predicted_secondary / actual_secondary,
            }
    return {"storage_state": report["storage_state"],
            "ranking_matches": matches, "ranking_total": total,
            "engines": observations}


def calibrate(training_dir, validation_dirs):
    train_manifest, train = load_capture(training_dir)
    if train["storage_state"] != "memory":
        raise ValueError("training capture must be the memory state")
    model = fit(train)
    seen = {"memory"}
    validation = []
    for directory in validation_dirs:
        manifest, report = load_capture(directory)
        if any(manifest[name] != train_manifest[name] for name in COMMON_MANIFEST):
            raise ValueError("captures have mismatched source/binary/fixture provenance")
        state = report["storage_state"]
        if state in seen:
            raise ValueError("duplicate validation storage state")
        seen.add(state)
        validation.append(evaluate(model, report))
    return {"model": model, "validation": validation,
            "provenance": {name: train_manifest[name] for name in COMMON_MANIFEST}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", type=Path, required=True)
    parser.add_argument("--validate", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        result = calibrate(args.train, args.validate)
    except (KeyError, OSError, ValueError) as exc:
        parser.error(str(exc))
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out is None:
        print(output, end="")
    else:
        args.out.write_text(output)


if __name__ == "__main__":
    main()
