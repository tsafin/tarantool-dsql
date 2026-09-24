# SQL parity corpus policy

`corpus.json` is the reviewed inclusion policy for the current **seed smoke
gate**. It includes three SQL TAP tests. The remaining tests are inventoried as
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

## Baseline storage and promotion

The pre-existing local `snapshots/` tree is an investigation artifact, not an
accepted baseline: it contains 132,413 YAML files occupying about 528 MiB,
only for `sql-tap`/memtx. It must not be added to Git or used as the CI
reference. The reviewed seed generated captures occupy about 312 KiB for
memtx and 324 KiB for Vinyl; these figures do not predict full-corpus size.

For M0-B, prefer a *reproducible capture from a named integration commit* as
the authoritative baseline, with the commit SHA, corpus policy version,
engine, capture manifest, and snapshot digest recorded together. CI can
build and capture that pinned commit, compare its manifest coverage with the
candidate, and upload the diff and capture manifests as artifacts. A PR's
moving merge-base is not an accepted baseline. Promotion to a new SHA must
be explicit and include repeat-capture and normal-runner evidence, full
suite/engine coverage, and a measured size/runtime review. This avoids
committing an unreviewed bulk YAML tree while retaining a regenerable
reference. Git-tracked snapshots or external object storage remain options
if full-corpus measurement shows regeneration is impractical.

The current `parity-corpus` workflow still uses a merge-base and the current
corpus policy is seed-only. The pinned-baseline workflow change is deferred
until a full-corpus integration commit is accepted; do not describe the seed
job as M0-B-complete.
