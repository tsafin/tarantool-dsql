#!/usr/bin/env python3
"""Assemble a fail-closed full SQL corpus policy from reviewed suite decisions.

Each review is a JSON document with `tests: [{test, engines: {memtx,
vinyl}}]`; each engine decision has `decision`, `category`, and `reason`.
The generated policy is still a review artifact: callers must inspect it and
pin its baseline commit in a subsequent explicit promotion commit.
"""

import argparse
import json
from pathlib import Path
import re

import corpus


def assemble(repo, review_paths, baseline_commit, policy_version=2):
    if not re.fullmatch(r"[0-9a-f]{40}", baseline_commit):
        raise ValueError("baseline commit must be a full lowercase SHA")
    decisions = {}
    for path in review_paths:
        review = json.loads(Path(path).read_text())
        suite = review["suite"]
        if suite not in corpus.SUITES:
            raise ValueError(f"invalid reviewed suite: {suite}")
        for row in review["tests"]:
            test = row["test"]
            if test in decisions or not test.startswith(suite + "/"):
                raise ValueError(f"duplicate or wrong-suite review: {test}")
            engines = row["engines"]
            if set(engines) != set(corpus.ENGINES):
                raise ValueError(f"incomplete engine review: {test}")
            for engine in corpus.ENGINES:
                entry = engines[engine]
                if entry.get("decision") not in ("include", "exclude") or \
                   not isinstance(entry.get("category"), str) or \
                   not entry["category"].strip() or \
                   not isinstance(entry.get("reason"), str) or \
                   not entry["reason"].strip():
                    raise ValueError(f"invalid review: {test}/{engine}")
            decisions[test] = engines
    included, excluded = [], []
    for test, engines in sorted(decisions.items()):
        selected = [e for e in corpus.ENGINES
                    if engines[e]["decision"] == "include"]
        if selected:
            reasons = [engines[e]["reason"] for e in selected]
            reason = reasons[0] if len(set(reasons)) == 1 else \
                     "; ".join(f"{e}: {engines[e]['reason']}"
                               for e in selected)
            included.append({"test": test, "engines": selected,
                             "reason": reason,
                             "category": "verified_parity",
                             "evidence": {e: engines[e].get("evidence", {})
                                          for e in selected}})
        for engine in corpus.ENGINES:
            entry = engines[engine]
            if entry["decision"] == "exclude":
                excluded.append({"test": test, "engines": [engine],
                                 "category": entry["category"],
                                 "reason": entry["reason"],
                                 "evidence": entry.get("evidence", {})})
    policy = {"policy_version": policy_version, "scope": "full-corpus",
              "baseline_commit": baseline_commit,
              "capture_limits": corpus.POLICY["capture_limits"],
              "included": included, "excluded": excluded}
    rows = corpus.inventory(repo, policy)
    if len(rows) != len(decisions):
        raise ValueError("review does not cover the complete SQL inventory")
    return policy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--review", type=Path, action="append", required=True)
    parser.add_argument("--baseline-commit", required=True)
    parser.add_argument("--policy-version", type=int, default=2)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    policy = assemble(args.repo.resolve(), args.review, args.baseline_commit,
                      args.policy_version)
    args.out.write_text(json.dumps(policy, indent=2) + "\n")
    print(f"reviewed {len(policy['included'])} included tests and "
          f"{len(policy['excluded'])} excluded engine pairs")


if __name__ == "__main__":
    main()
