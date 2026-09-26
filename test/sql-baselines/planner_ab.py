#!/usr/bin/env python3
"""Offline bounded-DP width A/B on a bounded, reviewed SQL-TAP subset."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DEFAULT_TESTS = ("join.test.lua", "join2.test.lua", "join3.test.lua",
                 "join5.test.lua", "where3.test.lua")
DEFAULT_WIDTHS = (1, 5, 10)
WIDTH_KEYS = ("SQL_PATH_SOLVER_WIDTH_ONE", "SQL_PATH_SOLVER_WIDTH_TWO",
              "SQL_PATH_SOLVER_WIDTH_MANY")
METRICS = ("candidate_count", "fallback_count", "generated", "dominated",
           "truncated", "retained", "elapsed_us")


def widths(value):
    try:
        result = tuple(int(part) for part in value.split(","))
    except ValueError:
        raise argparse.ArgumentTypeError("widths must be three integers")
    if len(result) != 3 or any(n < 1 or n > 64 for n in result):
        raise argparse.ArgumentTypeError("widths must be three integers in 1..64")
    return result


def environment(config):
    env = os.environ.copy()
    env.update(dict(zip(WIDTH_KEYS, map(str, config))))
    env.update(VDBE_DISPATCHER="generated", SQL_JIT_ENABLE="0")
    return env


def selected(policy, names, engine, budget):
    included = {row["test"]: row for row in policy["included"]}
    total = 0
    for name in names:
        if "/" in name or not name.endswith(".test.lua"):
            raise ValueError("select top-level SQL-TAP .test.lua basenames")
        identity = "sql-tap/" + name
        row = included.get(identity)
        if row is None or engine not in row["engines"]:
            raise ValueError(f"not reviewed for {engine}: {identity}")
        count = row.get("evidence", {}).get(engine, {}).get("captured_queries")
        if type(count) is not int or count < 1:
            raise ValueError(f"missing reviewed query budget: {identity}/{engine}")
        total += count
    if total > budget:
        raise ValueError(f"reviewed subset has {total} queries, exceeds {budget}")
    return total


def run(command, env=None, timeout=300):
    result = subprocess.run([str(part) for part in command], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stdout[-4000:])
    return result.stdout


def binary_identity(binary):
    digest = hashlib.sha256()
    with binary.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(binary), "sha256": digest.hexdigest(),
            "version": run([binary, "--version"]).splitlines()[0]}


def measurements(out, engine):
    rows = {}
    count = 0
    for path in sorted((out / "manifests/sql-tap").glob(f"*.{engine}.json")):
        manifest = json.loads(path.read_text())
        if manifest.get("planner_metrics_version") != 2:
            raise ValueError("binary/harness does not expose planner_metrics v2")
        count += manifest["captured_queries"]
        for metric in manifest["planner_metrics"]:
            key = f"{manifest['test_file']}:{metric['query_index']}"
            if key in rows or any(type(metric.get(field)) not in (int, float)
                                  for field in METRICS):
                raise ValueError(f"invalid planner metrics: {key}")
            rows[key] = metric
    if not rows:
        raise ValueError("empty planner metric sample")
    return count, rows


def metric_delta(base, candidate):
    if set(base) != set(candidate):
        raise ValueError("planner metric query identities differ")
    totals = {field: {"default": sum(row[field] for row in base.values()),
                      "candidate": sum(row[field] for row in candidate.values())}
              for field in METRICS}
    for values in totals.values():
        values["delta"] = values["candidate"] - values["default"]
    changes = []
    for key in sorted(base):
        delta = {field: candidate[key][field] - base[key][field]
                 for field in METRICS}
        structural = any(delta[field] for field in METRICS if field != "elapsed_us")
        path_changed = any(base[key].get(field) != candidate[key].get(field)
                           for field in ("path_class", "fallback_reason"))
        if structural or path_changed:
            changes.append({"query": key, "delta": delta,
                            "default_path": base[key].get("path_class"),
                            "candidate_path": candidate[key].get("path_class")})
    return {"queries_measured": len(base), "totals": totals,
            "default_path_classes": dict(Counter(row.get("path_class") for row in base.values())),
            "candidate_path_classes": dict(Counter(row.get("path_class") for row in candidate.values())),
            "structurally_changed_queries": changes,
            "width_effect_observed": bool(changes)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--widths", type=widths, default=(2, 8, 16))
    parser.add_argument("--test", action="append")
    parser.add_argument("--engine", action="append", choices=("memtx", "vinyl"))
    parser.add_argument("--max-queries", type=int, default=1000)
    args = parser.parse_args()
    if args.widths == DEFAULT_WIDTHS:
        raise ValueError("candidate must differ from default widths 1,5,10")
    repo, binary, out = args.repo.resolve(), args.binary.resolve(), args.out.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError("output must be empty")
    names = sorted(set(args.test or DEFAULT_TESTS))
    engines = sorted(set(args.engine or ("memtx", "vinyl")))
    policy = json.loads((repo / "test/sql-baselines/corpus.json").read_text())
    budgets = {engine: selected(policy, names, engine, args.max_queries)
               for engine in engines}
    out.mkdir(parents=True, exist_ok=True)
    report = {"evaluation_version": 1, "source_commit": run(
        ["git", "-C", repo, "rev-parse", "HEAD"]).strip(),
        "binary": binary_identity(binary),
        "tests": ["sql-tap/" + name for name in names],
        "default_widths": list(DEFAULT_WIDTHS), "candidate_widths": list(args.widths),
        "dispatcher": "generated", "engines": {},
        "limitations": ["bounded reviewed subset, not full-corpus or hosted CI",
                         "planner metrics describe compile-time EXPLAIN snapshots, not runtime latency",
                         "elapsed_us is diagnostic and not a deterministic acceptance gate",
                         "snapshot parity includes EXPLAIN rows; changed plan-output TAP expectations may prevent capture and are not semantic SQL-result regressions",
                         "no native-dispatcher performance or plan-quality claim"]}
    for engine in engines:
        captured = {}
        for label, config in (("default", DEFAULT_WIDTHS),
                              ("default-repeat", DEFAULT_WIDTHS),
                              ("candidate", args.widths),
                              ("candidate-repeat", args.widths)):
            capture = out / engine / label
            env = environment(config)
            env["BUILDDIR"] = str(binary.parent.parent)
            with tempfile.TemporaryDirectory(prefix="planner-ab-work-") as temp:
                for index, name in enumerate(names):
                    work = Path(temp) / str(index)
                    work.mkdir()
                    env["LISTEN"] = f"unix/:{work}/listen.sock"
                    try:
                        run([binary, HERE / "harness/run.lua", repo / "test/sql-tap" / name,
                             f"--engine={engine}", f"--out={capture}",
                             f"--work-dir={work}"], env=env)
                    except (RuntimeError, subprocess.TimeoutExpired) as exc:
                        report["failure"] = {"engine": engine, "configuration": label,
                                             "test": "sql-tap/" + name,
                                             "detail": str(exc)}
                        report["parity_passed"] = False
                        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                        raise
            run([binary, HERE / "validate.lua", capture])
            count, captured[label] = measurements(capture, engine)
            if count != budgets[engine] or count > args.max_queries:
                raise ValueError("captured query count differs from reviewed bound")
        comparisons = {}
        for label, base_label in (("default-repeat", "default"),
                                  ("candidate-repeat", "candidate"),
                                  ("candidate", "default")):
            result = subprocess.run([str(binary), str(HERE / "diff.lua"),
                                     str(out / engine / base_label),
                                     str(out / engine / label), "--format=json"],
                                    text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=300)
            comparisons[label] = json.loads(result.stdout)
            if result.returncode not in (0, 1):
                raise RuntimeError(result.stderr)
        report["engines"][engine] = {
            "captured_queries_per_run": budgets[engine], "comparisons": comparisons,
            "repeat_planner_structure_stable": {
                label: not metric_delta(captured[label], captured[label + "-repeat"])["width_effect_observed"]
                for label in ("default", "candidate")},
            "planner_metrics": metric_delta(captured["default"], captured["candidate"])}
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    report["parity_passed"] = all(comparison["summary"]["passed"]
        for row in report["engines"].values() for comparison in row["comparisons"].values())
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(out / "report.json"),
                      "parity_passed": report["parity_passed"],
                      "width_effect_observed": {engine: row["planner_metrics"]["width_effect_observed"]
                        for engine, row in report["engines"].items()}}, sort_keys=True))
    if not report["parity_passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
