# SQL parity corpus policy

`corpus.json` is the reviewed inclusion policy for the current **seed smoke
gate**. It includes seven tests across SQL TAP, SQL-language, and luatest
suites. The remaining tests are inventoried as
`pending`, with an explicit reason; this is not a claim that M0-B full corpus
coverage is complete. The inventory command scans top-level `*.test.lua` and
`*.test.sql` in `test/sql` and `test/sql-tap`, and `*_test.lua` in
`test/sql-luatest`. Tests added to those suites become pending automatically.

```sh
python3 test/sql-baselines/corpus.py inventory --repo . --out /tmp/sql-corpus-inventory.json
python3 test/sql-baselines/corpus.py capture --repo . \
  --binary /absolute/path/to/tarantool --out /tmp/sql-seed-memtx \
  --engine memtx --mode generated
```

Capture requires an empty output directory, creates a fresh database directory
for each test, and runs the existing manifest-v1 validator. It fails if any
test fails, a dispatcher does not execute, or actual manifest coverage differs
from the policy. `compare-coverage` checks test identities and query counts
before `diff.lua` compares snapshots. The workflows run this sequence on both
memtx and Vinyl. The dispatcher workflow also requires positive CnP/LLVM
execution counters in manifest v1.

The `gh-2884-forbid-rowid-syntax` seed relies on harness semantic fix
`b1535bb0ba`: `box.execute` can return an error as its second result. The
combined native-dispatch build also needs `ENABLE_SQL_CNP=ON` alongside
`ENABLE_SQL_JIT=ON`. Each future inclusion needs normal-runner comparison on
every listed engine and a documented reason. Full M0-B acceptance also
requires a reviewed decision for every pending test, stable recapture, and
baseline storage review.

The per-PR corpus has a reviewed per-test budget of at most 10,000 captured
SQL statements and 64 MiB of YAML snapshots. `corpus.py` enforces both limits
when accepting capture coverage. Stress/volume tests above either limit stay
in the inventory with an explicit engine-specific exclusion reason and normal
runner evidence; they are not silently omitted or mislabeled as regressions.

## Baseline storage and promotion

The pre-existing local `snapshots/` tree is an investigation artifact, not an
accepted baseline: it contains 132,413 YAML files occupying about 528 MiB,
only for `sql-tap`/memtx. It must not be added to Git or used as the CI
reference. The reviewed seed generated captures occupy about 312 KiB for
memtx and 324 KiB for Vinyl; these figures do not predict full-corpus size.

For M0-B, use a *reproducible capture from a named integration commit* as
the authoritative baseline. The `parity-corpus` workflow reads
`baseline_commit` from `corpus.json`: once `scope` is no longer `seed-smoke`,
the field is mandatory and must be a full 40-character SHA that is an
ancestor of the PR head. The baseline commit must itself carry the same
corpus scope and policy version. The workflow builds both commits, captures
the declared corpus, compares manifest coverage and snapshots, and uploads
the diff, manifests, and a baseline provenance record containing the SHA,
policy version, engine, and capture digest. No bulk YAML tree is required in
Git. A PR's moving merge-base remains permitted only for the seed smoke
gate; it is not an accepted M0-B baseline.

Promotion to a new SHA is an explicit policy change: include repeat-capture
and normal-runner evidence, full suite/engine coverage, and a measured
size/runtime review. The accepted corpus commit is named only after that
review. Git-tracked snapshots or external object storage remain options if
full-corpus measurement shows regeneration is impractical. The current
policy is still seed-only and has no accepted `baseline_commit`; the seed
job must not be described as M0-B-complete.

Both CI jobs reserve 120 minutes for the full corpus because the SQL-TAP
sweep contains long tests and the under-budget candidate set may exceed
50,000 SQL statements on memtx. This is a capacity allowance, not a measured
full-matrix runtime;
record wall time and artifact size from the first integrated capture and
revisit the timeout before marking the gate stable.

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
