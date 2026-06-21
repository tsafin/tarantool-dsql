# S0 — Statistics Audit Report

**Branch:** `worktree-agent-aa9d970c7848e495f` (off `tsafin/llvm_jit`)
**Issue:** #16 under epic #14 in `tsafin/tarantool-dsql`
**Scope:** decide reuse / delete / extract-helper per file for the historical
`_sql_stat1` / `_sql_stat4` scaffolding before S1 introduces the new
`_sql_stats_relation` / `_sql_stats_index` / `_sql_stats_column` system spaces.

## Executive summary

The historical `_sql_stat1` / `_sql_stat4` scaffolding in this tree is almost
entirely **vestigial text**, not code. There is:

- **no `analyze.c`** anywhere under `src/box/sql/`;
- **no ANALYZE grammar** in `src/box/sql/parse.y` (zero matches for `ANALYZE`
  or `TK_ANALYZE` outside one comment in `where.c`);
- **no `_sql_stat1` / `_sql_stat4` system space** created on bootstrap (only
  test text mentions them);
- **two no-op bodies for `OP_LoadAnalysis`** (original `vdbe.c:3483` and
  extracted `vdbe_ops_inline_medium_8.c:439`), both early-out with
  `assert(P1 == 0); return 0;`;
- the cost model is driven entirely by a 7-element constant array
  `default_tuple_est[]` in `src/box/sql.c:1487`, consumed by
  `index_field_tuple_est()` at `src/box/sql.c:1505`;
- `whereRangeScanEst()` in `src/box/sql/where.c:1004` uses the 1/4 and 1/64
  fallbacks documented in `statistics_implementation_plan.md`.

The 12 disabled `analyze*.test.lua` files in `test/sql-tap/` (5100 LoC) are the
single most valuable artifact: real SQLite-derived regression coverage that
S1.8 can re-enable once persistence and grammar exist.

## S0.1 — Disabled analyze tests + `OP_LoadAnalysis` body inventory

### Disabled `analyze*.test.lua` (test/sql-tap/suite.ini)

Twelve files are listed under `disabled = ...` in
`test/sql-tap/suite.ini:11-22`:

| File | LoC | Likely topic (from SQLite lineage) |
|------|-----|------------------------------------|
| `analyze1.test.lua` | 564 | basic ANALYZE grammar, `_sql_stat1` shape |
| `analyze3.test.lua` | 647 | stat4 range estimation |
| `analyze4.test.lua` | 158 | small-table sampling thresholds |
| `analyze5.test.lua` | 327 | composite index NDV |
| `analyze6.test.lua` | 197 | LIKE / GLOB selectivity |
| `analyze7.test.lua` | 190 | system-table interactions |
| `analyze8.test.lua` | 206 | covering-index estimates |
| `analyze9.test.lua` | 1677 | stat4 sample-record decoding |
| `analyzeC.test.lua` | 275 | corrupt-stat4 robustness |
| `analyzeD.test.lua` | 200 | partial-stat updates |
| `analyzeE.test.lua` | 523 | sqlite_stat4 binary loading |
| `analyzeF.test.lua` | 136 | regression tickets |

Total: **5100 lines** of disabled test scaffolding. `test/sql/` and
`test/sql-luatest/` have **no** analyze-related test files (grepped both
suite.ini files and recursive `*analyz*`/`*stat[14]*` filename matches).

The header of `analyze1.test.lua:35` still queries `_sql_stat1` directly:

```lua
SELECT count(*) FROM "_space" WHERE "name"='_sql_stat1'
```

This will need rewriting against `_sql_stats_relation` when re-enabled — the
test expects the space to exist, but in the new design `_sql_stat1` is gone.

### `OP_LoadAnalysis` handler bodies — confirmed no-op

Three implementations, all no-op:

1. **`src/box/sql/vdbe.c:3477-3491`** — original dispatcher path. Comment still
   claims it "reads the sql_stat1 table"; body is:

   ```c
   assert(P1 == 0);
   /* TODO: Enable analysis. */
   /* if (sql_analysis_load(db) != 0) goto abort_due_to_error; */
   DISPATCH();
   ```

   The commented-out `sql_analysis_load(db)` references a function that does
   not exist in the tree (no `sql_analysis_load` symbol anywhere).

2. **`src/box/sql/vdbe_ops_inline_medium_8.c:428-446`** — extracted handler
   `vdbe_op_loadanalysis_inline()`. Identical no-op behaviour, slightly
   updated comment ("Currently a no-op pending full statistics
   implementation"). Returns 0 unconditionally.

3. **`src/box/sql/vdbe_dispatch_wrapper.c:1368-1372`** — dispatcher case
   calling `vdbe_op_loadanalysis_inline()`. Mechanically forwards to (2).

Cross-cutting registrations:

- `src/box/sql/vdbe_ops.h:267` — function prototype.
- `src/box/sql/vdbe_cnp.c:1408` — CnP JIT thunk pointer.
- `src/box/sql/vdbe_jit.c:234` — `JIT_MODE_UNSUPPORTED` entry (forces
  fallback to interpreter).
- `tools/vdbe_dsl/opcodes.yaml:972-976` — DSL entry, `id: 129`,
  `handler_type: external_inline`.

**Finding:** `OP_LoadAnalysis` is currently unreachable in practice. There is
no parser path that emits it (no ANALYZE grammar); the SQL compiler does not
generate it anywhere in the tree. Six files carry registration weight for an
opcode that nothing emits.

### Other ANALYZE-related TAP tests

None. The 12 analyze*.test.lua files in `test/sql-tap/` are the complete set.
No analyze test exists under `test/sql/` or `test/sql-luatest/`.

## S0.2 — `_sql_stat1` / `_sql_stat4` reference inventory

Recursive grep for `_sql_stat1|_sql_stat4|sqlite_stat1|sqlite_stat4|sql_stat1|sql_stat4`
across `src/box/sql/` returns four files. None contain executable code that
reads or writes the historical spaces:

| File | Line | Reference | Classification |
|------|------|-----------|----------------|
| `src/box/sql/vdbe.c` | 3479 | comment in `OP_LoadAnalysis` opcode doc: "Read the sql_stat1 table..." | dead comment (lying about what the body does) |
| `src/box/sql/where.c` | 999 | comment in `whereRangeScanEst()` rationale: "In the absence of `_sql_stat4` ANALYZE data..." | informative — explains *why* the 1/4 + 1/64 fallback exists |
| `src/box/sql/whereexpr.c` | 1211 | comment in IS NOT NULL rewrite: "When `sql_stat4` histogram data is available..." | informative — flags a future optimization gate |
| `src/box/sql/vdbe_ops_inline_medium_8.c` | 431 | doc-comment header copy of (1) | dead comment |

Adjacent (analyze-token / structural) references — wider grep for
`stat4|analyze|sql_analysis`:

| File | Line | Reference | Classification |
|------|------|-----------|----------------|
| `src/box/sql/whereInt.h` | 393 | `UnpackedRecord *pRec; /* Probe for stat4 (if required) */` in `WhereLoopBuilder` | unused struct field; nothing populates `pRec` because no stat4 sampler exists |
| `src/box/sql/whereInt.h` | 469 | `void sqlWhereExprAnalyze(SrcList *, WhereClause *);` | NOT statistics — expression-analysis helper, generic naming collision |
| `src/box/sql/expr.c` | 5275, 5470, 5491-5492 | `analyzeAggregate`, `analyzeAggregatesInSelect` | NOT statistics — aggregate resolver, generic naming |
| `src/box/sql/select.c`, `resolve.c` | various | "analyze the expression" prose in comments | NOT statistics — generic English |

`sql_analysis_load` (the function the `vdbe.c:3487` comment references): **zero
hits** in the entire tree. The reference is a vestigial pointer to deleted
SQLite code.

### `index_field_tuple_est()` — confirmed default-only

`src/box/sql.c:1505-1522` is exactly as the design doc claims:

```c
int16_t
index_field_tuple_est(const struct index_def *idx_def, uint32_t field)
{
    ...
    if (field == idx_def->key_def->part_count && idx_def->opts.is_unique)
        return 0;
    return default_tuple_est[field + 1 >= 6 ? 6 : field];
}
```

Inputs are ignored apart from "is this the unique-key terminal column?" The
seven-element constant array `default_tuple_est[] = {DEFAULT_TUPLE_LOG_COUNT,
33, 32, 30, 28, 26, 23}` at `src/box/sql.c:1487` is the only data source.

Callers (5 sites): `src/box/sql/pragma.c:152`, `src/box/sql/where.c:1767, 1920,
1921, 2212`.

Adjacent function `sql_space_tuple_log_count()` at `src/box/sql.c:1490` uses
the *primary index size* (`pk->vtab->size(pk)`) for actual base tables, which
is the only piece of "real" cardinality the cost model currently sees. **This
is reusable** in S1 as the fast path before the snapshot is consulted.

### `whereRangeScanEst()` — confirmed 1/4 + 1/64 heuristic

`src/box/sql/where.c:1004-1039`. The function applies:

- `whereRangeAdjust(pLower, ...)` and `whereRangeAdjust(pUpper, ...)` — these
  honor application-supplied `likelihood()` hints on the term;
- if both bounds are present and neither has an explicit likelihood,
  subtracts a fixed `20` from the LogEst (the "75% reduction" claim);
- floor at `nNew = 10`.

This is exactly the fallback the design doc names. The body is small and
self-contained, ripe for the S1.7 compatibility adapter (call into snapshot
if available; fall back to this exact code path otherwise).

## Per-file disposition table

| File | Lines of interest | Disposition | Rationale |
|------|------------------|-------------|-----------|
| `test/sql-tap/suite.ini` | 11-22 | **REWRITE** | re-enable analyze*.test.lua list in S1.8 once grammar exists; possibly drop `analyzeE.test.lua` (sqlite_stat4-binary specific) |
| `test/sql-tap/analyze1.test.lua` | full file (564) | **REWRITE** (re-enable + adapt) | basic ANALYZE coverage; references to `_sql_stat1` must move to `_sql_stats_relation` |
| `test/sql-tap/analyze3..analyze9, analyzeC, analyzeD, analyzeF.test.lua` | 5050 lines | **REWRITE** (selective) | re-enable in S1.8/S2.6 in waves: analyze1/4/5/8 first (basics), analyze3/6/9 with S2 histograms, analyzeC/D as robustness, analyzeF as regression |
| `test/sql-tap/analyzeE.test.lua` | 523 | **DELETE** | tests the binary `sqlite_stat4` blob format which the new design explicitly does not adopt (`statistics_implementation_plan.md` §Persistence) |
| `src/box/sql/vdbe.c` (`OP_LoadAnalysis` body, 3477-3491) | 15 | **REWRITE** | repurpose as `OP_LoadSqlStats` body that populates `SqlStatsSnapshot`; OR delete if S1 loads stats at prepare time rather than via opcode — see "Open questions" |
| `src/box/sql/vdbe_ops_inline_medium_8.c` (`vdbe_op_loadanalysis_inline`, 428-446) | 19 | **REWRITE** | same fate as (above); body and prototype move together |
| `src/box/sql/vdbe_ops.h` (line 267 prototype) | 1 | **REWRITE** | rename + reshape with the handler |
| `src/box/sql/vdbe_dispatch_wrapper.c` (case at 1368-1372) | 5 | **REWRITE** | dispatcher case follows handler rename |
| `src/box/sql/vdbe_cnp.c` (line 1408) | 1 | **REWRITE** | CnP thunk follows handler rename |
| `src/box/sql/vdbe_jit.c` (line 234) | 1 | **REWRITE** | promote from `JIT_MODE_UNSUPPORTED` to `JIT_MODE_CALL` once handler does real work; or DELETE if opcode is removed entirely |
| `tools/vdbe_dsl/opcodes.yaml` (lines 972-976) | 5 | **REWRITE** | rename `OP_LoadAnalysis` → `OP_LoadSqlStats` and regenerate; or DELETE entry if loading moves out of bytecode |
| `src/box/sql/where.c` (comment at line 999) | 1 line | **REWRITE** | update to mention `_sql_stats_index` instead of `_sql_stat4` when S1.7 lands |
| `src/box/sql/where.c` (`whereRangeScanEst`, 1004-1039) | 36 | **EXTRACT-HELPER** | keep the 1/4+1/64 logic as the explicit "no snapshot available" branch of S1.7's adapter; useful exactly as written |
| `src/box/sql/whereexpr.c` (comment at line 1211) | 1 line | **REWRITE** | update comment to reference `_sql_stats_column` histograms (S2.4) |
| `src/box/sql/whereInt.h` (line 393, `pRec` field) | 2 | **DELETE** | `UnpackedRecord *pRec` for stat4 probing is dead; no producer in the tree |
| `src/box/sql.c` (`default_tuple_est[]`, 1487-1488) | 2 | **KEEP-AS-REFERENCE** | retain as the explicit fallback when snapshot is absent; S1.7 calls this when no relation stats are loaded |
| `src/box/sql.c` (`index_field_tuple_est`, 1505-1522) | 18 | **REWRITE** | function name preserved, body changed: consult `SqlStatsSnapshot` first, fall back to `default_tuple_est[]` |
| `src/box/sql.c` (`sql_space_tuple_log_count`, 1490-1502) | 13 | **KEEP-AS-REFERENCE** | already uses real primary-index size; the new snapshot supersedes it but the function shape stays usable as a fast path for stale/missing stats |

## Useful pieces to carry over

1. **`test/sql-tap/analyze{1,4,5,8,C,D,F}.test.lua`** — 1700+ LoC of
   independent-of-stat4 coverage (basic ANALYZE, small-table sampling,
   covering-index estimates, robustness, regression). Re-enable in S1.8 after
   grammar and persistence. Mechanical edits only: substitute system-space
   names.
2. **`whereRangeScanEst()` 1/4+1/64 fallback** at `where.c:1004` — exact
   shape for the "no snapshot / stale snapshot" branch of S1.7. Lift, do not
   rewrite.
3. **`sql_space_tuple_log_count()`** at `sql.c:1490` — uses real primary index
   size; keep as the "fast path for relation cardinality when no analyzed
   snapshot exists." Already engine-aware via `vtab->size`.
4. **`default_tuple_est[]`** at `sql.c:1487` — the only place in the tree
   that encodes "what's a reasonable guess for first-column / second-column
   NDV reduction." Use as the floor of the S1.7 fallback ladder.
5. **DSL slot for `OP_LoadAnalysis`** (`tools/vdbe_dsl/opcodes.yaml:972`) —
   if S1 needs a bytecode-level stats-loading opcode, the slot id (`129`),
   dispatcher case, CnP thunk, and JIT mode entry are already plumbed.
   Repurpose by renaming rather than allocating a new opcode id.

## Dead weight (clean removals)

1. **`vdbe.c:3486-3489`** — commented-out call to non-existent
   `sql_analysis_load(db)`. Delete the comment block in S1 since the
   function never existed in this fork.
2. **`whereInt.h:393`** — `UnpackedRecord *pRec; /* Probe for stat4 ... */`
   in `WhereLoopBuilder`. No producer, no consumer. Removing it also removes
   any `nRecValid` companion logic that currently sits inert.
3. **`analyzeE.test.lua`** — tests `sqlite_stat4` binary blob loading,
   explicitly out of scope per the design doc's persistence section.
4. **Test scaffold "must materialize `_sql_stat1`" expectations** in
   `analyze1.test.lua:33-40` and similar — these probe the historical
   space name; rewriting them is mandatory before re-enable.

## Open questions for S1

1. **Should `OP_LoadAnalysis` be repurposed as `OP_LoadSqlStats`, or removed
   entirely?** The design doc says snapshots are "built once per prepare"
   (statistics_implementation_plan.md §Planner-Facing Contract), implying
   loading happens in the prepare frontend, not in bytecode. If so, the
   opcode is dead and should be deleted from all six registration sites
   (`vdbe.c`, `vdbe_ops_inline_medium_8.c`, `vdbe_ops.h`,
   `vdbe_dispatch_wrapper.c`, `vdbe_cnp.c`, `vdbe_jit.c`, `opcodes.yaml`).
   If a bytecode-level explicit-refresh path is desired (e.g. for
   `ANALYZE ... RECOMPUTE` future syntax), the opcode slot is convenient.
   **Recommend: delete.** S1 should load via prepare-time path, not VDBE.
2. **Does `ANALYZE` grammar belong in S1.2 or earlier?** Currently there is
   *no* token, *no* rule. S1.2 in the roadmap says "Re-enable `ANALYZE`
   grammar; remove the `unsupported ANALYZE` rejection path" — but the
   rejection is implicit (syntax error from missing rule), not an explicit
   path. S1.2 is really "introduce `ANALYZE` grammar" with no removal step.
3. **`whereexpr.c:1211` `IS NOT NULL` → `> NULL` rewrite** — currently
   unconditional, predicated on "when histograms exist." Should S2 gate this
   on `SqlStatsSnapshot.has_histograms`, or keep it unconditional? Affects
   selectivity estimation correctness when histograms arrive.
4. **`sql_space_tuple_log_count()` vs S1 snapshot** — when both report
   cardinality, who wins? Snapshot is collection-time stale; live PK size is
   instantaneous but not column-aware. Recommend snapshot for selectivity,
   live PK size for empty-table detection.
5. **Should the disabled `_sql_stat1` create-table expectations in
   analyze1.test.lua be preserved as a compat fixture?** The design doc says
   "Do not make the new planner depend on `_sql_stat1` text parsing." So no
   — adapt the tests to query `_sql_stats_relation` instead.

## Recommendations for S1.1 (new system space definition)

1. **Do not allocate space names that collide with `_sql_stat1` /
   `_sql_stat4`.** The new names `_sql_stats_relation` /
   `_sql_stats_index` / `_sql_stats_column` already avoid the collision —
   keep them. No compatibility importer is needed (no fresh instance has
   `_sql_stat1` data; the comment at `where.c:999` confirms it never
   existed in practice in this fork).
2. **Define the snapshot loader as a prepare-time call**, not a VDBE
   opcode. This lets `OP_LoadAnalysis` be deleted cleanly (open question 1).
3. **Inherit `sql_space_tuple_log_count()` as the no-snapshot fast path.**
   It already does the right thing for empty tables and views; the new
   snapshot just overrides it when a relation has been analyzed.
4. **Inherit `default_tuple_est[]` as the bottom of the fallback ladder.**
   Order:
   `SqlStatsSnapshot.indexes[i].prefix_ndv`
   → `sql_space_tuple_log_count(space)` heuristic split
   → `default_tuple_est[field]`.
5. **Inherit `whereRangeScanEst`'s 1/4+1/64 reduction as the
   no-histogram branch** for S1.7's range-selectivity adapter. Body stays
   as-is; the new code wraps it.
6. **Tag the snapshot with the schema version** (already in
   `SqlStatsSnapshot.schema_version`) so prepared statements can invalidate
   on DDL. This is consistent with how `index_field_tuple_est()` currently
   re-resolves `space_by_id` every call.

## Disposition counts

| Disposition | Files |
|-------------|-------|
| **DELETE** | 3 (`analyzeE.test.lua`; `vdbe.c:3486-89` comment-block fragment; `whereInt.h:393` `pRec` field) |
| **KEEP-AS-REFERENCE** | 2 (`default_tuple_est[]` array; `sql_space_tuple_log_count()`) |
| **EXTRACT-HELPER** | 1 (`whereRangeScanEst()` 1/4+1/64 fallback) |
| **REWRITE** | 11 (suite.ini; analyze{1,3,4,5,6,7,8,9,C,D,F}.test.lua group counted as one rewrite class; `OP_LoadAnalysis` registration cluster spanning `vdbe.c`, `vdbe_ops_inline_medium_8.c`, `vdbe_ops.h`, `vdbe_dispatch_wrapper.c`, `vdbe_cnp.c`, `vdbe_jit.c`, `opcodes.yaml`; `index_field_tuple_est()`; two stale comments in `where.c` and `whereexpr.c`) |

Total touched: 17 file-level items (plus the 11 surviving analyze tests).

If `OP_LoadAnalysis` is deleted per Open Question #1, the REWRITE count
drops by 7 (the registration cluster becomes DELETE) and DELETE rises to 10.
That is the recommended path.
