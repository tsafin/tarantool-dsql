#!/usr/bin/env python3
"""Validate fixture-size transfer of a two-engine SQL access-price pilot."""

import argparse
import json
from pathlib import Path
import statistics

from access_cost_report import ENGINES
from calibrate_access_cost import COMMON_MANIFEST, SHAPES, load_capture


def fit_scaled(reports):
    if len(reports) < 2 or len({item["rows"] for item in reports}) != len(reports):
        raise ValueError("at least two distinct training fixture sizes required")
    if any(item["storage_state"] != "memory" for item in reports):
        raise ValueError("size-transfer training requires memory-state captures")
    result = {"model": "scan_input_plus_output_v1",
              "unit": "prepared_materialized_sql_us", "engines": {},
              "training_rows": sorted(item["rows"] for item in reports)}
    for engine in ENGINES:
        def cost(report, access):
            return report["accesses"][access][engine + "_median_us"]

        scan_rates = [cost(item, "primary_filtered") / item["rows"]
                      for item in reports]
        primary_output_rates = [
            (cost(item, "primary_broad") - cost(item, "primary_filtered")) /
            (item["rows"] // 2 - 16) for item in reports]
        secondary_startups = [cost(item, "secondary_payload")
                              for item in reports]
        secondary_output_rates = [
            (cost(item, "secondary_broad") - cost(item, "secondary_payload")) /
            (item["rows"] // 2 - 16) for item in reports]
        parameters = {
            "primary_scan_us_per_input_row": statistics.mean(scan_rates),
            "primary_output_us_per_row": statistics.mean(primary_output_rates),
            "secondary_startup_us": statistics.mean(secondary_startups),
            "secondary_output_us_per_row": statistics.mean(secondary_output_rates),
        }
        if min(parameters.values()) < 0:
            raise ValueError("negative empirical coefficient")
        result["engines"][engine] = parameters
    return result


def evaluate_scaled(model, report):
    rows = report["rows"]
    if rows in model["training_rows"]:
        raise ValueError("validation fixture size was used in training")
    observations = {}
    matches = 0
    for engine in ENGINES:
        parameters = model["engines"][engine]
        observations[engine] = {}
        for name, (primary_access, secondary_access, fixed_rows) in SHAPES.items():
            output_rows = fixed_rows if fixed_rows is not None else rows // 2
            predicted_primary = (
                parameters["primary_scan_us_per_input_row"] * rows +
                parameters["primary_output_us_per_row"] * (output_rows - 16))
            predicted_secondary = (
                parameters["secondary_startup_us"] +
                parameters["secondary_output_us_per_row"] * (output_rows - 16))
            actual_primary = report["accesses"][primary_access][engine + "_median_us"]
            actual_secondary = report["accesses"][secondary_access][engine + "_median_us"]
            same_rank = (predicted_secondary < predicted_primary) == \
                (actual_secondary < actual_primary)
            matches += same_rank
            observations[engine][name] = {
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
    return {"rows": rows, "storage_state": report["storage_state"],
            "ranking_matches": matches, "ranking_total": len(SHAPES) * len(ENGINES),
            "engines": observations}


def calibrate_scaled(training_dirs, validation_dirs):
    loaded_train = [load_capture(path) for path in training_dirs]
    reference = loaded_train[0][0]
    provenance_fields = tuple(name for name in COMMON_MANIFEST if name != "rows")
    if any(any(manifest[name] != reference[name] for name in provenance_fields)
           for manifest, _ in loaded_train):
        raise ValueError("training captures have different provenance")
    model = fit_scaled([report for _, report in loaded_train])
    validations = []
    for path in validation_dirs:
        manifest, report = load_capture(path)
        if any(manifest[name] != reference[name] for name in provenance_fields):
            raise ValueError("validation capture has different provenance")
        validations.append(evaluate_scaled(model, report))
    return {"model": model, "validation": validations,
            "provenance": {name: reference[name] for name in provenance_fields}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", action="append", type=Path, required=True)
    parser.add_argument("--validate", action="append", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        result = calibrate_scaled(args.train, args.validate)
    except (KeyError, OSError, ValueError) as exc:
        parser.error(str(exc))
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out is None:
        print(output, end="")
    else:
        args.out.write_text(output)


if __name__ == "__main__":
    main()
