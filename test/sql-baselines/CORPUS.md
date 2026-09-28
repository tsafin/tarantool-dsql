# SQL parity corpus policy

`corpus.json` is the accepted **full-corpus v2** policy. It covers all 402
discovered tests across SQL TAP, SQL-language, and luatest: 588 test/engine
pairs are included and 216 are explicitly excluded, with no pending pair.
The inventory command scans top-level `*.test.lua` and `*.test.sql` in
`test/sql` and `test/sql-tap`, and `*_test.lua` in `test/sql-luatest`.
An added test fails full-corpus inventory until both engine decisions are
reviewed; it cannot silently inherit seed-smoke pending status. A narrowly
scoped `introduced_after_baseline` exclusion records a reviewed diagnostic
fixture. Candidate inventory must always contain the declared test. Baseline
capture may omit it only when the selected immutable anchor predates the test;
if the anchor already contains it, the exact reviewed exclusion must also be
present in that anchor's policy.

Full-corpus acceptance requires a reviewed decision for both engines of
every discovered test. Every inclusion is `verified_parity` with
per-engine audit evidence; every exclusion needs a specific category, reason,
and evidence. `capture_pending`, `parity_pending`, and other unreviewed states
are rejected as full-corpus exclusions. The three suite reviews are assembled
with `assemble_policy.py`; ordinary policy changes require a new reviewed
anchor. The explicit `introduced_after_baseline` exclusion is the exception:
it retains the accepted SHA while making a non-workload diagnostic test
visible in candidate inventory, without requiring an artificial snapshot.

```sh
python3 test/sql-baselines/corpus.py inventory --repo . --out /tmp/sql-corpus-inventory.json
python3 test/sql-baselines/corpus.py capture --repo . \
  --binary /absolute/path/to/tarantool --out /tmp/sql-full-memtx \
  --engine memtx --mode generated
```

Capture requires an empty output directory, creates a fresh database directory
for each test, and runs the existing manifest-v1 validator. It fails if any
test fails, a dispatcher does not execute, or actual manifest coverage differs
from the policy. `compare-coverage` checks test identities and query counts
before `diff.lua` compares snapshots. The workflows run this sequence on both
memtx and Vinyl. The dispatcher workflow also requires positive CnP/LLVM
execution counters and validates per-query native participation evidence in
manifest v1. A successful compilation without native entry is a mode miss;
an interpreter-only statement that the native compiler deliberately declined
is recorded as executed but not eligible. The accepted local full-corpus
matrix passed all six engine/mode captures on a clean CI-style native build:
298 memtx tests / 49,535 queries and 290 Vinyl tests / 39,535 queries per
mode. CnP and LLVM each matched generated with zero hard or soft drift; both
generated repeat captures and the post-provenance-fix captures matched every
snapshot and manifest identity. The first hosted full-corpus CI run remains
to be observed after this branch is published.
Per-test native totals are only a minimum sanity check: a test that executes
native code for a setup statement and then disables the native mode for its
workload is excluded during review, even if its aggregate counter is positive.

The `gh-2884-forbid-rowid-syntax` seed relies on harness semantic fix
`b1535bb0ba`: `box.execute` can return an error as its second result. The
combined native-dispatch build also needs `ENABLE_SQL_CNP=ON` alongside
`ENABLE_SQL_JIT=ON`. Future inclusions still need normal-runner comparison on
every listed engine and a documented reason.

The per-PR corpus has a reviewed per-test budget of at most 10,000 captured
SQL statements and 64 MiB of YAML snapshots. `corpus.py` enforces both limits
when accepting capture coverage. Stress/volume tests above either limit stay
in the inventory with an explicit engine-specific exclusion reason and normal
runner evidence; they are not silently omitted or mislabeled as regressions.

## Baseline storage and promotion

The pre-existing local `snapshots/` tree is an investigation artifact, not an
accepted baseline: it contains 132,413 YAML files occupying about 528 MiB,
only for `sql-tap`/memtx. It must not be added to Git or used as the CI
reference. The accepted generated full-corpus capture has 44,520,089 YAML
payload bytes on memtx and 38,032,834 on Vinyl (about 203 MiB and 164 MiB
on disk because of small-file overhead). This supports regenerating the
baseline from its named commit instead of committing bulk YAML.

M0-B uses a *reproducible capture from a named integration commit* as
the authoritative baseline. The accepted anchor is
`d8fc1e339b0bb8579c8b08e39d062e20edf66666`. The `parity-corpus`
workflow reads `baseline_commit` from `corpus.json`; in full-corpus scope,
the field is mandatory and must be a full 40-character SHA that is an
ancestor of the PR head. The baseline commit must itself carry the same
corpus scope, policy version, and reviewed decisions/evidence (except the
`baseline_commit` pointer). The workflow builds both commits, captures
the declared corpus, compares manifest coverage and snapshots, and uploads
the diff, manifests, and a baseline provenance record containing the SHA,
policy version, engine, and capture digest. No bulk YAML tree is required in
Git. The baseline checkout is a detached worktree made from the already
verified PR history, so fork-only anchor commits need not exist upstream.
A PR's moving merge-base remains permitted only for a seed-smoke policy; it
is not an accepted M0-B baseline. `captured.against_commit` is supplied from
the tested repository, even when the head's harness captures the baseline;
the separate provenance artifact records the full anchor SHA.

The first full-policy commit is an anchor: it contains the complete decisions
and capture contract. A following, separate promotion commit sets
`baseline_commit` to that anchor's SHA after its baseline capture, repeat
check, and size/runtime review. This two-commit sequence avoids pretending
that a commit can contain its own SHA. CI compares the policies while ignoring
only the pointer, so later decision or evidence changes require a new anchor.

Promotion to a new SHA is an explicit policy change: include repeat-capture
and normal-runner evidence, full suite/engine coverage, and a measured
size/runtime review. Git-tracked snapshots or external object storage remain
options if later corpus growth makes regeneration impractical.

Both CI jobs reserve 120 minutes. The local post-fix generated pair completed
in about two minutes in parallel on this host, and the full local native
matrix completed well inside that allowance; clean hosted builds, dependency
download, and artifact upload may dominate. Revisit the timeout after the
first hosted full-corpus run rather than extrapolating the local wall time.

The dispatcher workflow installs LLVM/Clang 11 and Clang 19 from the signed
`apt.llvm.org` Focal repository because `tarantool/testing:ubuntu-focal`
does not include either toolchain. JIT bitcode uses Clang 11 to match its LLVM
libraries; CnP stencils use Clang 19. The `test-run` gitlink currently points
to a commit unavailable from its public remote. Parity jobs therefore
initialize build-required submodules, then clone the public `test-run`
revision `71c1373f9b64088cee1e8aba4d446bd42dca925c` and its nested
submodules. This published revision still includes the luatest child runner
used by the SQL-luatest capture hook; the newer public tip removed it.
`test/test-run.py` resolves through the resulting pinned checkout, while
ordinary SQL TAP tests still use the standalone harness. The missing
runner Python dependency `gevent==22.10.2` is installed explicitly in both
jobs; the Focal image already provides the pinned PyYAML 5.3.1. The missing
`checkpatch` and `third_party/luarocks` mappings were restored in
`.gitmodules`, but a full recursive checkout of the repository's current
gitlinks remains unsuitable until the `test-run` gitlink is published or
replaced.
