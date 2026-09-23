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
