#!/usr/bin/env python3
"""Capture one SQL or single-child sql-luatest file through its normal runner."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def unsupported_luatest_source(source):
    """Reject SQL paths the one-child Lua box.execute hook cannot observe."""
    if len(re.findall(r"\bserver:new\s*\(", source)) != 1:
        return "capture requires exactly one child-server construction"
    if re.search(r":restart\s*\(", source):
        return "capture cannot preserve a restarted child's query sequence"
    if "net_box" in source and re.search(r":execute\s*\(", source):
        return "direct net.box SQL bypasses the child box.execute hook"
    if re.search(r"\bbox\.prepare\s*\(", source):
        return "prepared-statement execution bypasses the string SQL hook"
    return None


def capture(args):
    repo = args.repo.resolve()
    runner_repo = args.runner_repo.resolve()
    binary = args.binary.resolve()
    out = args.out.resolve()
    test = args.test
    suffix = "_test.lua" if args.suite == "sql-luatest" else ".test.lua"
    if not test.endswith(suffix) or "/" in test:
        raise ValueError(f"test must be a top-level {args.suite} *{suffix} file")
    if not (runner_repo / "test" / args.suite / test).is_file():
        raise ValueError("test file does not exist in runner repository")
    if args.suite == "sql-luatest":
        source = (runner_repo / "test" / args.suite / test).read_text()
        unsupported = unsupported_luatest_source(source)
        if unsupported:
            raise ValueError(unsupported)
    if out.exists() and any(out.iterdir()):
        raise ValueError("output must be empty")
    out.mkdir(parents=True, exist_ok=True)
    hook = repo / "test/sql-baselines/harness/luatest_child.lua"
    env = os.environ.copy()
    env.update({
        "VDBE_DISPATCHER": "cnp" if args.mode == "cnp" else "generated",
        "SQL_JIT_ENABLE": "1" if args.mode == "llvm" else "0",
        "SQL_BASELINE_OUT": str(out),
        "SQL_BASELINE_TEST": f"{args.suite}/{test}",
        "SQL_BASELINE_ENGINE": args.engine,
        "SQL_BASELINE_MODE": args.mode,
    })
    if args.planner_flag is not None:
        env["SQL_BASELINE_PLANNER_FLAG"] = args.planner_flag
    if args.suite == "sql-luatest":
        env["TARANTOOL_RUN_BEFORE_BOX_CFG"] = f"dofile({str(hook)!r})"
    else:
        env["SQL_BASELINE_HOOK"] = str(hook)
    with tempfile.TemporaryDirectory(prefix="lt-") as vardir:
        command = [sys.executable, str(runner_repo / "test/test-run.py"),
                   "--builddir", str(binary.parent.parent),
                   "--executable", str(binary),
                   "--vardir", vardir, "--suite", args.suite,
                   "-j", "-1", "--force"]
        if args.suite == "sql":
            command += ["--conf", args.engine]
        # test-run's positional selector is a substring, not an exact test
        # identity. Exclude matching siblings so, for example, `collation`
        # does not also execute `planner_fallback_collation` and restart the
        # capture-owned server.
        test_stem = test[:-len(suffix)]
        patterns = ("*_test.lua",) if args.suite == "sql-luatest" else \
                   ("*.test.lua", "*.test.sql")
        for pattern in patterns:
            for sibling in sorted((runner_repo / "test" / args.suite).glob(pattern)):
                if sibling.name != test and test_stem in sibling.name:
                    command.extend(("--exclude", sibling.name))
        command += [test]
        completed = subprocess.run(command, cwd=runner_repo, env=env,
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=300)
    if completed.returncode != 0:
        raise RuntimeError("normal runner failed:\n" + completed.stdout)
    if "[ pass ]" not in completed.stdout:
        raise RuntimeError("normal runner did not report a passing test")
    if (out / "luatest-capture-error").exists():
        raise RuntimeError("child snapshot write failed: " +
                           (out / "luatest-capture-error").read_text())
    state_path = out / "luatest-child-state.json"
    if not state_path.is_file():
        raise RuntimeError("normal runner passed without a captured child")
    state = json.loads(state_path.read_text())
    identity = f"{args.suite}/{test}"
    if state.get("test_file") != identity or state.get("engine") != args.engine or \
       state.get("execution_mode") != args.mode or \
       state.get("engine_mismatch") is not False or \
       state.get("planner_flag") != args.planner_flag or \
       state.get("planner_flag_mismatch") is not False or \
       not isinstance(state.get("captured_queries"), int) or \
       state["captured_queries"] < 1:
        raise RuntimeError("child capture identity or engine mismatch")
    cnp = state["cnp_exec_delta"]
    llvm = state["llvm_exec_delta"]
    mode_executed = (args.mode == "generated" and cnp == 0 and llvm == 0) or \
                    (args.mode == "cnp" and cnp > 0) or \
                    (args.mode == "llvm" and llvm > 0)
    if not mode_executed:
        raise RuntimeError("requested execution mode was not observed")
    count = state["captured_queries"]
    index_fields = (
        "executed_query_indices",
        "native_compile_attempt_query_indices",
        "native_compile_success_query_indices",
        "native_participation_query_indices",
        "eligible_query_indices",
        "mode_miss_queries",
    )
    indices = {}
    for field in index_fields:
        values = state.get(field)
        if not isinstance(values, list) or \
           any(type(i) is not int or i < 1 or i > count for i in values) or \
           values != sorted(set(values)):
            raise RuntimeError(f"invalid {field} in child capture")
        indices[field] = values
    eligible_indices = indices["eligible_query_indices"]
    participation = indices["native_participation_query_indices"]
    misses = indices["mode_miss_queries"]
    expected_eligible = sorted(
        set(indices["executed_query_indices"]) &
        (set(indices["native_compile_success_query_indices"]) |
         set(participation)))
    expected_misses = sorted(set(eligible_indices) - set(participation)) \
                      if args.mode != "generated" else []
    if eligible_indices != expected_eligible or misses != expected_misses or \
       state.get("eligible_queries") != len(eligible_indices) or \
       state.get("native_participation_queries") != len(participation):
        raise RuntimeError("inconsistent per-query dispatcher participation evidence")
    if args.mode != "generated" and misses:
        raise RuntimeError(f"native mode misses at queries {misses}")
    manifest = {
        "manifest_version": 1,
        "suite": args.suite,
        "test_file": identity,
        "engine": args.engine,
        "dispatcher_requested": env["VDBE_DISPATCHER"],
        "sql_jit_enable": args.mode == "llvm",
        "execution_mode": args.mode,
        "planner_flag": args.planner_flag,
        "mode_executed": True,
        "cnp_exec_delta": cnp,
        "llvm_exec_delta": llvm,
        "executed_query_indices": indices["executed_query_indices"],
        "native_compile_attempt_query_indices":
            indices["native_compile_attempt_query_indices"],
        "native_compile_success_query_indices":
            indices["native_compile_success_query_indices"],
        "eligible_queries": len(eligible_indices),
        "eligible_query_indices": eligible_indices,
        "native_participation_queries": len(participation),
        "native_participation_query_indices": participation,
        "mode_miss_queries": misses,
        "runtime_engine": args.engine,
        "engine_mismatch": False,
        "test_exit_code": 0,
        "test_load_ok": True,
        "test_load_error": "",
        "cfg_errors": 0,
        "captured_queries": count,
        "written_snapshots": count,
        "skipped_queries": 0,
        "snapshot_errors": 0,
        "planner_metrics_version": 2,
        "component_ledger_version": 1,
        "planner_metrics": state.get("planner_metrics", []),
        "accepted": True,
    }
    stem = test[:-len(".test.lua")] if args.suite == "sql" else \
           test[:-len(".lua")]
    manifest_path = out / "manifests" / args.suite / \
                    f"{stem}.{args.engine}.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest) + "\n")
    subprocess.run([str(binary), str(repo / "test/sql-baselines/validate.lua"),
                    str(out)], check=True, cwd=binary.parent.parent)
    print(f"captured {identity} engine={args.engine} mode={args.mode} "
          f"queries={count} cnp={cnp} llvm={llvm}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--runner-repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--test", required=True)
    parser.add_argument("--suite", choices=("sql", "sql-luatest"),
                        default="sql-luatest")
    parser.add_argument("--engine", choices=("memtx", "vinyl"), required=True)
    parser.add_argument("--mode", choices=("generated", "cnp", "llvm"),
                        required=True)
    parser.add_argument("--planner-flag", choices=("off", "on"))
    args = parser.parse_args()
    try:
        capture(args)
    except (ValueError, OSError, subprocess.CalledProcessError, RuntimeError,
            KeyError) as exc:
        print(f"luatest capture: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
