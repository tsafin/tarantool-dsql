#!/usr/bin/env python3
"""Translate empirical access prices to LogEst and evaluate them offline.

This is a selection-policy experiment, not a production planner switch.  It
fits the existing size-transfer model, quantizes its coefficients into a
fixed work unit, and converts the resulting integer work totals with the same
sqlLogEst() approximation used by the SQL planner.  Validation uses forced
primary/secondary timings.  Where a capture contains an unforced plan, the
script also compares the production choice with the translated candidate.
Both known result cardinalities and the per-path estimates recorded by EXPLAIN
are evaluated, and their ranking quality is reported separately.
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
ESTIMATE_RE = re.compile(r"\(~([0-9]+) rows\)")
PAIRED_ACCESSES = frozenset(
    access for pair in SHAPES.values() for access in pair[:2])
CAPTURE_ACCESSES = PAIRED_ACCESSES | {"primary_scan"}


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


def candidate_cost(parameters, input_rows, primary_output_rows,
                   secondary_output_rows=None):
    if secondary_output_rows is None:
        secondary_output_rows = primary_output_rows
    primary_extra_output = max(0, primary_output_rows - 16)
    secondary_extra_output = max(0, secondary_output_rows - 16)
    primary_ticks = (
        parameters["primary_scan_ticks_per_input_row"] * input_rows +
        parameters["primary_output_ticks_per_row"] * primary_extra_output)
    secondary_ticks = (
        parameters["secondary_startup_ticks"] +
        parameters["secondary_output_ticks_per_row"] *
        secondary_extra_output)
    return {
        "primary_work_ticks": primary_ticks,
        "secondary_work_ticks": secondary_ticks,
        "primary_logest": sql_log_est(primary_ticks),
        "secondary_logest": sql_log_est(secondary_ticks),
        "winner": "secondary" if secondary_ticks < primary_ticks else "primary",
    }


def plan_estimated_rows(plan):
    estimates = []
    for row in plan:
        if not row:
            continue
        match = ESTIMATE_RE.search(str(row[-1]))
        if match is not None:
            estimates.append(int(match.group(1)))
    if len(estimates) != 1:
        raise ValueError("expected exactly one EXPLAIN row estimate")
    return estimates[0]


def load_planner_estimates(directory, manifest, report):
    """Load stable per-access EXPLAIN estimates from the raw capture."""
    estimates = {(engine, access): set() for engine in ENGINES
                 for access in CAPTURE_ACCESSES}
    path = Path(directory) / "observations.jsonl"
    with path.open() as stream:
        for line in stream:
            if not line.strip():
                continue
            item = json.loads(line)
            access = item.get("access")
            engine = item.get("engine")
            if (engine, access) not in estimates:
                continue
            for name in ("run_id", "source_commit", "binary_sha256", "rows",
                         "storage_state"):
                expected = manifest[name] if name in manifest else report[name]
                if item.get(name) != expected:
                    raise ValueError("observation provenance mismatch: " + name)
            estimates[engine, access].add(plan_estimated_rows(item["explain"]))
    result = {engine: {} for engine in ENGINES}
    for (engine, access), values in estimates.items():
        if len(values) != 1:
            raise ValueError("missing or unstable planner estimate for " +
                             engine + "/" + access)
        result[engine][access] = next(iter(values))
    return result


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


def evaluate_candidate(model, report, planner_estimates):
    observations = {}
    oracle_matches = 0
    planner_matches = 0
    oracle_direct_matches = 0
    planner_direct_matches = 0
    baseline_matches = 0
    baseline_total = 0
    for engine in ENGINES:
        observations[engine] = {}
        parameters = model["engines"][engine]
        for shape, (primary_access, secondary_access, fixed_rows) in SHAPES.items():
            output_rows = fixed_rows if fixed_rows is not None else report["rows"] // 2
            oracle_candidate = candidate_cost(parameters, report["rows"],
                                              output_rows)
            primary_estimate = planner_estimates[engine][primary_access]
            secondary_estimate = planner_estimates[engine][secondary_access]
            planner_input_rows = planner_estimates[engine]["primary_scan"]
            planner_candidate = candidate_cost(parameters, planner_input_rows,
                                               primary_estimate,
                                               secondary_estimate)
            observed = observed_winner(report, engine, primary_access,
                                       secondary_access)
            baseline = unforced_winner(report, engine, shape)
            oracle_match = oracle_candidate["winner"] == observed
            planner_match = planner_candidate["winner"] == observed
            oracle_matches += oracle_match
            planner_matches += planner_match
            item = {
                "input_rows": report["rows"],
                "actual_output_rows": output_rows,
                "planner_estimated_input_rows": planner_input_rows,
                "primary_planner_estimated_rows": primary_estimate,
                "secondary_planner_estimated_rows": secondary_estimate,
                "observed_winner": observed,
                "oracle_cardinality_candidate": {
                    **oracle_candidate,
                    "ranking_match": oracle_match,
                },
                "planner_cardinality_candidate": {
                    **planner_candidate,
                    "ranking_match": planner_match,
                },
            }
            if baseline is not None:
                baseline_match = baseline == observed
                baseline_matches += baseline_match
                baseline_total += 1
                oracle_direct_matches += oracle_match
                planner_direct_matches += planner_match
                item["production_unforced_winner"] = baseline
                item["production_match"] = baseline_match
            observations[engine][shape] = item
    return {
        "rows": report["rows"],
        "storage_state": report["storage_state"],
        "oracle_cardinality_candidate_ranking_matches": oracle_matches,
        "planner_cardinality_candidate_ranking_matches": planner_matches,
        "candidate_ranking_total": len(SHAPES) * len(ENGINES),
        "planner_cardinality_match_delta": planner_matches - oracle_matches,
        "oracle_cardinality_direct_matches": oracle_direct_matches,
        "planner_cardinality_direct_matches": planner_direct_matches,
        "direct_comparison_total": baseline_total,
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
        planner_estimates = load_planner_estimates(path, manifest, report)
        validation.append(evaluate_candidate(translated, report,
                                             planner_estimates))
    return {
        "schema_version": 2,
        "candidate": translated,
        "validation": validation,
        "provenance": {name: reference[name] for name in provenance_fields},
        "scope": {
            "production_defaults_changed": False,
            "cardinality_modes": ["known_actual_output",
                                  "captured_per_path_planner_estimate"],
            "planner_estimate_source": "observations.jsonl EXPLAIN output",
            "production_comparison":
                "captured unforced broad/tail choices only",
            "decision": "offline_evidence_only",
            "remaining_gate":
                "repeated JOIN-inner-loop A/B and common-unit composition",
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
