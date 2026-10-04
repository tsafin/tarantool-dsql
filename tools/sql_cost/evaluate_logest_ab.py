#!/usr/bin/env python3
"""Translate empirical access prices to LogEst and evaluate them offline.

This is a selection-policy experiment, not a production planner switch.  It
fits the existing size-transfer model, quantizes its coefficients into a
fixed work unit, and converts the resulting integer work totals with the same
sqlLogEst() approximation used by the SQL planner.  Validation uses forced
primary/secondary timings.  Where a capture contains an unforced plan, the
script also compares the production choice with the translated candidate.
"""

import argparse
import json
import re
from pathlib import Path

from access_cost_report import ENGINES
from calibrate_access_cost import COMMON_MANIFEST, SHAPES, load_capture
from calibrate_access_scale import fit_scaled


DEFAULT_WORK_UNIT_US = 0.01
UNFORCED_SHAPES = {
    "broad": "unforced_broad_plan",
    "tail": "unforced_tail_plan",
}


def sql_log_est(value):
    """Python equivalent of src/box/sql/util.c:sqlLogEst()."""
    if not isinstance(value, int) or value < 0:
        raise ValueError("LogEst input must be a non-negative integer")
    lookup = (0, 2, 3, 5, 6, 7, 8, 9)
    result = 40
    if value < 8:
        if value < 2:
            return 0
        while value < 8:
            result -= 10
            value <<= 1
    else:
        while value > 255:
            result += 40
            value >>= 4
        while value > 15:
            result += 10
            value >>= 1
    return lookup[value & 7] + result - 10


def quantize_model(model, work_unit_us=DEFAULT_WORK_UNIT_US):
    if work_unit_us <= 0:
        raise ValueError("work unit must be positive")
    result = {
        "model": "integer_work_logest_v1",
        "work_unit_us": work_unit_us,
        "training_rows": model["training_rows"],
        "engines": {},
    }
    fields = (
        "primary_scan_us_per_input_row",
        "primary_output_us_per_row",
        "secondary_startup_us",
        "secondary_output_us_per_row",
    )
    for engine in ENGINES:
        result["engines"][engine] = {}
        for field in fields:
            value = model["engines"][engine][field]
            ticks = max(1, round(value / work_unit_us))
            result["engines"][engine][field.replace("_us", "_ticks")] = ticks
    return result


def candidate_cost(parameters, input_rows, output_rows):
    extra_output = max(0, output_rows - 16)
    primary_ticks = (
        parameters["primary_scan_ticks_per_input_row"] * input_rows +
        parameters["primary_output_ticks_per_row"] * extra_output)
    secondary_ticks = (
        parameters["secondary_startup_ticks"] +
        parameters["secondary_output_ticks_per_row"] * extra_output)
    return {
        "primary_work_ticks": primary_ticks,
        "secondary_work_ticks": secondary_ticks,
        "primary_logest": sql_log_est(primary_ticks),
        "secondary_logest": sql_log_est(secondary_ticks),
        "winner": "secondary" if secondary_ticks < primary_ticks else "primary",
    }


def observed_winner(report, engine, primary_access, secondary_access):
    primary = report["accesses"][primary_access][engine + "_median_us"]
    secondary = report["accesses"][secondary_access][engine + "_median_us"]
    return "secondary" if secondary < primary else "primary"


def unforced_winner(report, engine, shape):
    key = UNFORCED_SHAPES.get(shape)
    if key is None or key not in report:
        return None
    rows = report[key].get(engine)
    if not rows:
        return None
    details = " ".join(str(row[-1]) for row in rows if row)
    match = re.search(r"\bUSING (?:COVERING )?INDEX ([^ ]+)", details)
    if match is None:
        return "primary"
    expected_secondary = "cost_" + engine + "_a"
    return "secondary" if match.group(1) == expected_secondary else "primary"


def evaluate_candidate(model, report):
    observations = {}
    candidate_matches = 0
    baseline_matches = 0
    baseline_total = 0
    for engine in ENGINES:
        observations[engine] = {}
        parameters = model["engines"][engine]
        for shape, (primary_access, secondary_access, fixed_rows) in SHAPES.items():
            output_rows = fixed_rows if fixed_rows is not None else report["rows"] // 2
            candidate = candidate_cost(parameters, report["rows"], output_rows)
            observed = observed_winner(report, engine, primary_access,
                                       secondary_access)
            baseline = unforced_winner(report, engine, shape)
            candidate_match = candidate["winner"] == observed
            candidate_matches += candidate_match
            item = {
                "input_rows": report["rows"],
                "output_rows": output_rows,
                "observed_winner": observed,
                "candidate_match": candidate_match,
                **candidate,
            }
            if baseline is not None:
                baseline_match = baseline == observed
                baseline_matches += baseline_match
                baseline_total += 1
                item["production_unforced_winner"] = baseline
                item["production_match"] = baseline_match
            observations[engine][shape] = item
    return {
        "rows": report["rows"],
        "storage_state": report["storage_state"],
        "candidate_ranking_matches": candidate_matches,
        "candidate_ranking_total": len(SHAPES) * len(ENGINES),
        "production_choice_matches": baseline_matches,
        "production_choice_total": baseline_total,
        "engines": observations,
    }


def evaluate_ab(training_dirs, validation_dirs,
                work_unit_us=DEFAULT_WORK_UNIT_US):
    loaded_train = [load_capture(path) for path in training_dirs]
    reference = loaded_train[0][0]
    provenance_fields = tuple(name for name in COMMON_MANIFEST if name != "rows")
    if any(any(manifest[name] != reference[name] for name in provenance_fields)
           for manifest, _ in loaded_train):
        raise ValueError("training captures have different provenance")
    empirical = fit_scaled([report for _, report in loaded_train])
    translated = quantize_model(empirical, work_unit_us)
    validation = []
    for path in validation_dirs:
        manifest, report = load_capture(path)
        if any(manifest[name] != reference[name] for name in provenance_fields):
            raise ValueError("validation capture has different provenance")
        if report["rows"] in empirical["training_rows"]:
            raise ValueError("validation fixture size was used in training")
        validation.append(evaluate_candidate(translated, report))
    return {
        "schema_version": 1,
        "candidate": translated,
        "validation": validation,
        "provenance": {name: reference[name] for name in provenance_fields},
        "scope": {
            "production_defaults_changed": False,
            "uses_actual_output_cardinality": True,
            "production_comparison":
                "captured unforced broad/tail choices only",
            "decision": "offline_evidence_only",
            "remaining_gate":
                "planner-estimated cardinality and repeated JOIN-inner-loop A/B",
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", action="append", type=Path, required=True)
    parser.add_argument("--validate", action="append", type=Path, required=True)
    parser.add_argument("--work-unit-us", type=float,
                        default=DEFAULT_WORK_UNIT_US)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        result = evaluate_ab(args.train, args.validate, args.work_unit_us)
    except (KeyError, OSError, ValueError) as exc:
        parser.error(str(exc))
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out is None:
        print(output, end="")
    else:
        args.out.write_text(output)


if __name__ == "__main__":
    main()
