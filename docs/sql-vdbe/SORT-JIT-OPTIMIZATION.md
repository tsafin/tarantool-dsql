# Sort Comparator JIT Optimization Plan

## 2026-05-19 update

The latest sorter checkpoint replaces the weak runtime-generic mixed writer
prototype with the same bounded static-template policy already used for hot
mixed comparators.

What changed:

- sorter initialization now selects a static mixed-key writer template when the
  sorter shape is benchmark-proven and fully described by
  `fastCmpPartKind[]`;
- the current handled writer layouts are:
  - `[str, intlike, str, intlike]`;
  - `[str, str, intlike, str, intlike, str, intlike, str, intlike, str]`;
- those writers are instantiated in C++ templates and use compile-time kind
  recursion to:
  - size the MsgPack row;
  - encode each field;
  - populate the optional offset cache;
- unsupported mixed shapes stay on the generic writer path:
  `mem_mp_size()` plus `mem_to_mp_buf()`.

Why the earlier runtime-generic writer was rejected:

- it still executed a per-part runtime `kind` loop;
- it only replaced one dynamic encode loop with another;
- its profile movement was too small and too noisy to justify keeping it.

Why the template writer is the right generic form:

- it matches the comparator-side static tier structurally;
- the compiler can drop per-kind branching entirely inside the writer body;
- entrypoints stay bounded and explicit instead of growing another list of ad
  hoc mixed helpers.

Current focused results on the current build:

| workload | generated | CnP |
|---|---:|---:|
| `sort_text_wide_probe` | `321.74 us` | **`312.61 us`** |
| `sort_payload` | `171.65 us` | **`154.33 us`** |
| `sort_text_window` | `188.27 us` | **`156.47 us`** |

Interpretation:

- the wide mixed probe is now slightly ahead in CnP on like-for-like reruns;
- generated also improves, because sorter write is shared below the dispatcher
  layer;
- the write-path gain is real, but scan-side field-ref work still dominates the
  remaining profile.

Current `perf` readout on `sort_text_wide_probe` after the writer-template
change:

- `vdbe_field_ref_preload_group_fast` `6.33%`
- `mem_to_mp_buf` `5.11%`
- `vdbe_field_ref_fetch_data_offset_slot_fast` `5.07%`
- `vdbe_op_column_string_offset_slot_static_fast` `3.91%`
- `mem_mp_size` `3.84%`
- `vdbeSorterCompareCnpFieldString` `3.74%`
- `sqlVdbeSorterWriteFromMems` `3.63%`

Current near-term plan:

1. Keep the bounded static writer-template tier for hot stable shapes.
2. Extend it only when a benchmark proves a new layout is both hot and common.
3. Keep the generic writer as the semantic fallback for the long tail.
4. If long-tail mixed writer work becomes necessary later, follow the same
   copy-and-patch fragment mechanics used on the compare side instead of
   adding another runtime-generic mixed writer loop.

## Current status

The first sorter-local raw MsgPack comparator pass is implemented in
`src/box/sql/vdbesort.c`.

What is in place now:

- sorter-key classification during `sqlVdbeSorterInit()`;
- table-driven `field_type -> compare kind` mapping for supported small simple
  keys;
- dedicated `vdbeSorterCompareIntLikeFast()` for all-integer-like keys;
- fixed-count integer specializations for the hot `sort_window` shape family;
- wider `vdbeSorterCompareSimpleFast()` for mixed small simple keys;
- a raw-key prefix compare in `sqlVdbeSorterCompare()` for supported static
  shapes;
- type-specific `FIELD_TYPE_NUMBER` column stencils in the CnP path;
- a threaded CnP fragment entry for `OP_SorterCompare`, so the sort-window
  compare opcode can now participate in the fragment pilot instead of falling
  back to the generic dispatch path;
- direct sorter-only write path via `OP_SorterInsert P3 != 0`, so sorter sites
  can encode `Mem[]` values straight into sorter-owned storage without an
  intermediate `MakeRecord` blob;
- one-pass int-like direct writer for sorter shapes already classified as
  all-integer-like, avoiding the generic `mem_mp_size()` plus `mem_to_mp_buf()`
  double walk on those rows;
- fallback to the existing unpack-based comparator on unsupported plan shapes
  or runtime MsgPack mismatch.

Measured `sort_window/prepared_execute` medians across the recent steps:

| shape | median |
|---|---:|
| generic unpack comparator | `74.31 us` |
| first integer-only fast path | `55.51 us` |
| widened generic simple fast path | `57.90 us` |
| split comparator family with table-driven classification | **`54.28 us`** |
| direct sorter write from `Mem[]` | **`50.82 us`** |
| one-pass int-like direct writer | **`50.70 us`** |
| dedicated integer exact `OP_Column` fast path | **`49.82 us`** |
| fixed-count 2/3/4 integer specializations | **`49.89 us`** |

The current conclusion is:

- raw sorter compare is clearly worthwhile;
- integer-heavy keys need a dedicated comparator family;
- avoiding `MakeRecord` + blob copy on sorter-only sites is also worthwhile;
- removing the generic direct-writer double walk is structurally correct but
  only a small additional lever on `sort_window`;
- the next meaningful target after sorter work really was the dominant
  `OP_Column` / field-ref path;
- splitting the shared exact-fast helper by hot field type is worthwhile for
  integer-heavy workloads like `sort_window`.

The current `generated` dispatcher result on the same workload is
`60.43 us` median. This path shares the same specialized handler body, but the
remaining gap is still dominated by generated-dispatch overhead rather than the
sorter-local helpers themselves.

Latest `perf` profile on `sort_window/prepared_execute`:

- generated: `vdbeSorterCompareIntLike3Fast` is the top sorter symbol, with
  `sqlVdbeSorterWriteFromMems` and `vdbe_field_ref_fetch_data` next in line;
- CnP: `vdbeSorterCompareIntLike3Fast` and
  `vdbe_op_column_integer_exact_fast` lead, followed by
  `sqlVdbeSorterWriteFromMems`, `vdbeSorterMerge`, and
  `vdbe_field_ref_prepare_tuple`;
- the remaining sorter-local cost is now mostly compare/write/merge, while the
  CnP-specific gap is also carrying `vdbe_op_column_*` and bridge overhead.

The next comparator step is to keep the raw fallback in place while widening the
typed sorter compare family to more fixed integer shapes.

## 1. Goal

Reduce the cost of sorter-heavy SQL workloads, especially shapes like
`sort_window`, by replacing the current generic sorter record comparison path
with specialized raw MsgPack comparators that avoid full key unpacking on the
hot merge path.

This document focuses on the sorter comparator only. It does not try to JIT
the entire sorter subsystem.

## 2. Current hot path

Today the in-memory and PMA merge paths in `src/box/sql/vdbesort.c` compare
sorter records via:

1. `vdbeSorterCompare()`
2. `sqlVdbeRecordUnpackMsgpack()` for the right-hand key
3. `sqlVdbeRecordCompareMsgpack()` for left-vs-unpacked comparison
4. `mem_cmp_msgpack()` per field
5. `mem_cmp_scalar()` after building a temporary `Mem`

For sorter-heavy ORDER BY workloads, this means:

- the right-hand key is unpacked to `UnpackedRecord` repeatedly;
- each compared field is decoded through the generic `Mem` path;
- part metadata (`type`, `coll`, `sort_order`) is consulted dynamically;
- early exit exists, but the field compare body is still generic.

This is correct and flexible, but it is not shaped for CnP/JIT-style
specialization.

## 3. What is statically knowable

At prepare time, the sorter `key_def` already contains:

- exact part count;
- exact field types per part;
- exact sort order per part;
- exact collation choice per part;
- whether desc parts exist at all;
- whether nullable handling is needed for the key shape;
- whether the sort key is a small homogeneous integer/scalar sequence.

For many SQL ORDER BY workloads, especially generated expression keys, this is
enough to generate a dedicated comparator with no loop-carried metadata lookups.

Example shape:

- `ORDER BY score DESC, b ASC, id DESC`

The comparator structure is statically known:

1. skip MsgPack array header;
2. compare field 0 as integer, invert sign;
3. if equal, compare field 1 as integer;
4. if equal, compare field 2 as integer, invert sign;
5. otherwise return 0.

## 4. Optimization hypothesis

The next profitable sorter optimization is not:

- storing fewer sorter payload columns for `sort_window`;
- or routing the sorter merge through the generic tuple/key comparator layer.

The next profitable optimization is:

- keep both sorter records in raw MsgPack form;
- decode only until the first non-equal field;
- generate or select a comparator specialized to the sorter key shape;
- avoid `UnpackedRecord` and per-field `Mem` materialization on the hot merge
  path whenever the key shape allows it.

This is a sorter-local form of "zerocopy":

- strings/binary values can stay as raw slices;
- integers/unsigned values can be decoded directly from MsgPack and compared as
  machine integers;
- composite key structure is traversed sequentially, not materialized up front.

## 4.1 Existing MsgPack skip facilities

There is already an efficient MsgPack skip primitive in the tree:

- `src/lib/msgpuck/msgpuck.h`: `mp_next(const char **data)`

This is the same facility used in iproto/xrow-style parsing when the code wants
to preserve a raw MsgPack slice or skip a whole value without decoding it into
native structures.

Representative uses:

- `src/box/xrow.c`
  - decode request map key;
  - preserve the value pointer;
  - advance over the whole value with `mp_next(&data)`.
- `src/box/lua/merger.c`
  - skip MP_TUPLE framing with `mp_decode_extl()` and `mp_decode_uint()`, then
    keep the raw tuple payload.
- `src/box/lua/tuple.c`
  - same top-level MP_TUPLE "skip framing, keep payload" pattern.

Important implementation detail:

- `mp_next()` is already optimized and table-driven.
- In `src/lib/msgpuck/msgpuck.h` it uses `mp_parser_hint` to skip many MsgPack
  values in a compact inline loop before falling back to a slow path.

So if sorter code needs "skip one whole field/object", the correct primitive is
normally `mp_next()`, not a new custom walker.

What this is useful for in sorter work:

- skip the outer array header once with `mp_decode_array()`;
- skip whole raw fields in fallback/bridge paths;
- preserve raw field slices for string/bin comparison;
- advance over unsupported or not-yet-specialized fields cheaply.

What it does not give us:

- random access to field `N`;
- a comparison result by itself;
- a replacement for typed decode on fields that actually decide ordering.

So the intended sorter strategy is:

- use `mp_next()` wherever we want to skip a whole MsgPack field;
- use direct typed decode for fields participating in the sort decision;
- avoid `Mem` materialization when raw compare can decide the result.

## 5. Important constraint: sequential traversal, not random access

Sorter records are plain MsgPack arrays. Field sizes are variable, so there is
no cheap constant-time jump to field `N` without extra metadata.

That means the first implementation should optimize for:

- sequential left-to-right field traversal;
- early exit at the first mismatch;
- zero-copy string/bin comparison where possible;
- typed direct compare for integer/unsigned/float-friendly cases.

It should not start with:

- per-record offset tables;
- random field access metadata;
- full tuple-format-style offset caching for sorter blobs.

Those may become worthwhile later for very wide sort keys, but they are not the
right first step.

## 6. Proposed staged implementation

### Stage 1: Add sorter-key shape classification

Introduce a small sorter-local classification derived from `key_def`.

Candidate structure:

```c
enum sorter_cmp_kind {
	SORTER_CMP_GENERIC = 0,
	SORTER_CMP_INT_SMALL,
	SORTER_CMP_UINT_SMALL,
	SORTER_CMP_SCALAR_NO_COLL_SMALL,
	SORTER_CMP_MIXED_TYPED_SMALL,
};

struct sorter_cmp_plan {
	enum sorter_cmp_kind kind;
	uint8_t part_count;
	bool has_desc;
	bool has_collation;
	bool has_nullable;
	enum field_type part_type[8];
	enum sort_order sort_order[8];
};
```

Initial scope:

- classify only small keys, for example `part_count <= 8`;
- prefer fully homogeneous integer/unsigned cases first;
- reject anything with collation or awkward type combinations to generic path.

Where to compute:

- during sorter initialization in `sqlVdbeSorterInit()`;
- store on `VdbeSorter`.

### Stage 2: Add non-JIT specialized C comparators

Before emitting machine code, add a few direct C comparators that stay in raw
MsgPack space.

Examples:

- `vdbeSorterCompareInt2()`
- `vdbeSorterCompareInt3DescMask()`
- `vdbeSorterCompareScalarNoColl4()`

Design requirements:

- input is raw sorter record pointer for both sides;
- skip the array header once on entry;
- compare only the number of parts in the plan;
- apply desc sign inversion statically or with a compact mask;
- do not allocate or populate `UnpackedRecord`;
- do not call `mem_cmp_msgpack()` on the fast path.
- use `mp_next()` for whole-field skipping in fallback/bridge code rather than
  ad hoc skip logic.

Fallback:

- if type/header validation fails, or the shape is not supported, use the
  current generic `vdbeSorterCompare()` path.

Purpose of this stage:

- validate the performance model before adding JIT machinery;
- confirm that "raw sequential compare" wins on `sort_window`;
- keep debugging simpler than a first implementation directly in emitted code.

### Stage 3: Refactor generic comparator into reusable field walkers

Extract reusable raw-compare helpers that can be shared by:

- hand-specialized C comparators;
- future JIT/CnP comparator emission;
- optional SQL-only fast paths.

Suggested helper granularity:

- skip MsgPack array header and return part count;
- skip one raw field with `mp_next()` when the fast path intentionally does not
  decode it;
- decode-and-compare integer/int-or-uint field;
- decode-and-compare unsigned field;
- decode-and-compare scalar field without collation;
- decode-and-compare string/bin field without copying;
- generic slow-path compare for collation or special types.

The goal is to avoid forcing JIT codegen to duplicate large generic decoding
logic from scratch.

### Stage 4: Add sorter-specific comparator thunk selection

Extend `VdbeSorter` with a comparator function pointer, for example:

```c
typedef int (*SorterRecordCompare)(struct SortSubtask *,
				   bool *key2_cached,
				   const void *key1,
				   const void *key2);
```

Then:

- generic path uses the current unpack-based comparator;
- classified shapes use a selected fast comparator;
- PMA merge and in-memory merge both continue to call through one sorter-local
  comparator slot.

This isolates the optimization to sorter internals and avoids changing SQL
opcode semantics.

### Stage 5: Add JIT/CnP comparator emission

Once the non-JIT fast path proves useful, add emitted comparators for supported
shapes.

Recommended scope for first emitted version:

- `part_count <= 4`;
- all parts integer or unsigned;
- no collation;
- no nullable/optional parts;
- desc order represented by static per-field sign handling.

Emission model:

1. decode array header for both records;
2. for each field:
   - decode integer-like field from both sides;
   - compare;
   - branch to return block on mismatch;
3. return 0 if all parts equal.

This can be emitted as:

- a small CnP comparator stencil family;
- or a dedicated generated/JIT helper, separate from opcode stencils.

The sorter does not need one opcode-sized fragment per field. It needs one
small comparator blob per key shape.

### Stage 6: Optional emitted mixed-field support

After integer-only shapes are stable, extend to:

- small scalar keys without collation;
- mixed signed/unsigned integer keys;
- string/bin keys with binary collation only.

Do not add locale/collation-sensitive generated compare first. That belongs in
slow path until the simple shapes are clearly paying off.

### Stage 7: Emit a single shape-specialized sorter fragment

Move the proven C comparator shapes into the threaded fragment pilot, but keep
the fragment granularity at one opcode-sized comparator body per shape.

Planned fragment inputs:

- prepare-time `key_def` summary;
- `part_count`;
- per-part `field_type`;
- per-part `sort_order`;
- collation / nullable rejection flags.

Planned emitted shapes:

- all-integer-like keys, unrolled for hot part counts such as 2/3/4;
- small mixed scalar keys with a fixed per-part decode sequence;
- binary string / varbinary keys where payload can be compared directly;
- generic fallback fragment for unsupported shapes.

Fragment body rules:

1. decode the MsgPack array header once;
2. compare fields left-to-right;
3. invert only the DESC parts;
4. return immediately on the first mismatch;
5. fall back to the existing unpack-based comparator when runtime MsgPack
   values do not match the prepared shape.

The fragment should not be split into per-field runtime subfragments. The
efficient form is a single straight-line fragment with type-specific helper
calls or inline decode sequences.

### Stage 8: Wire the planner to fragment selection

After the shape-specific fragment bodies exist, connect them to a sorter-local
plan selector so `OP_SorterCompare` can choose between:

- the generic unpack-based comparator;
- the current raw C fast-path comparator family;
- a generated fragment for a supported shape.

Selection rules:

1. only choose fragment mode when the `key_def` shape is fully classified;
2. prefer the smallest shape that matches the exact part/type/signature vector;
3. keep DESC/nullable/collation rejection explicit rather than inferred later;
4. preserve the current generic fallback when runtime MsgPack validation fails.

This keeps the fragment pipeline narrow and makes it easy to extend shape by
shape without changing SQL semantics.

Current implementation note:

- `OP_SorterCompare` now resolves to raw-sorter wrappers instead of the generic
  handler when the sorter key is a supported intlike shape;
- 2-, 3- and 4-part integer keys get dedicated raw compare wrappers;
- the current branch intentionally does not keep an extra signed-vs-unsigned
  wrapper family on top of that split;
- those wrappers are normal functions, not inlined bodies, so the generated ASM
  still has a call into the chosen wrapper;
- the current win comes from compile-time wrapper selection and fixed-count
  compare shapes, not from pasting the whole comparator into the fragment;
- other supported sorter shapes stay on the generic raw wrapper, which still
  falls back to the unpack-based comparator on runtime mismatch.

Assembler-level effect:

| item | before | after |
|---|---|---|
| compare target | generic `vdbe_op_sortercompare` | `vdbe_op_sortercompare_fast` / `..._intlike2` / `..._intlike3` / `..._intlike4` |
| shape choice | checked per execution | fixed from `key_def` at fragment compile time |
| control flow | generic handler + runtime shape checks | direct call to the selected wrapper |
| multi-column intlike path | loop-based helper body | fixed-count 2/3/4-part helper |
| win source | dispatch and selection overhead | earlier specialization and fewer branches |

Pseudo-asm shape:

```asm
; before
call vdbe_op_sortercompare
test eax, eax
jne  jump_target

; after
call vdbe_op_sortercompare_intlike3
test eax, eax
jne  jump_target
```

The fragment still contains a call, but the target is now shape-specific.
The main reduction is in dispatch and shape-selection code around the compare,
not inlining of the whole comparator body.

That is also the current limitation:

- selecting a better wrapper is still weaker than emitting the actual
  comparator body from the static shape;
- the wrapper-level approach does not change the hotter sorter merge compare
  path in `vdbesort.c`;
- extra CnP-only wrapper families quickly duplicate semantics without moving the
  real hotspot enough.

The first emitted-shape follow-up is now in place too:

- the sorter records a prepare-time CnP-only equality shape for 2/3/4-part
  signed and unsigned prefixes;
- `OP_SorterCompare` can bind exact equality handlers from that shape;
- those handlers intentionally ignore ASC/DESC because the opcode only needs
  equality, not full ordering;
- generic SQL `INTEGER` / `UNSIGNED` affinities are intentionally excluded
  from those exact shapes, because runtime sorter keys for those affinities may
  still serialize as either `MP_INT` or `MP_UINT`;
- only fixed-width integer field types are safe for exact signed/unsigned
  equality-shape binding.

Measured outcome on `sort_window/prepared_execute`:

- the change is functionally correct but only moves the focused benchmark by a
  noise-level amount;
- that indicates opcode-side compare specialization is nearly exhausted here;
- the next meaningful sorter-specific gain should come from the hotter
  merge-path comparator in `vdbesort.c`, not another `OP_SorterCompare`
  selector refinement;
- a follow-up attempt to add a same-type `MP_UINT` / `MP_INT` subpath inside
  `vdbeSorterCompareIntLike3Fast()` was reverted after a small regression on
  `sort_window`, so adding more per-compare type checks inside that helper is
  not the preferred direction.

Current discard-mode `sort_window` rerun with this selector in place:

| shape | median |
|---|---:|
| generated prepared | `61.49 us` |
| CnP prepared | **`53.33 us`** |

Current discard-mode profile on `sort_window/prepared_execute`:

- `vdbeSorterCompareIntLike3Fast` remains the sorter hotspot;
- `vdbeSorterCompareIntLikeValuesFast` now covers the common same-type integer
  path before falling back to the mixed-type comparator;
- `vdbe_op_column_integer_exact_fast` and `sqlVdbeSorterWriteFromMems` remain
  the next largest SQL-side costs.

Next comparator direction:

- capture a tiny runtime type mask per sorter row at
  `sqlVdbeSorterWriteFromMems()` time, while the exact `Mem.type` values are
  still available;
- use that cached mask in the merge comparator to choose exact integer decode
  paths without re-reading MsgPack tags on every compare;
- keep the raw MsgPack row as the canonical sorter storage format, so PMA spill
  and merge behavior stay unchanged;
- keep the existing mixed-intlike and generic unpacked fallbacks for any row
  shape that does not match the cached fast mask.

That first runtime-mask slice is now implemented:

- sorter rows now carry a one-byte intlike runtime type mask alongside the raw
  MsgPack row in both in-memory `SorterRecord` objects and PMA records;
- the mask is produced once from exact `Mem.type` values during
  `sqlVdbeSorterWriteFromMems()`;
- `vdbeSorterCompareIntLike{2,3,4}Fast()` and the generic intlike fast compare
  path now use the cached mask when both compared rows have it, and fall back
  to the old `mp_typeof()`-driven logic otherwise;
- prebuilt sorter rows and non-intlike shapes still carry mask `0`, which keeps
  them on the old path with unchanged behavior.

Measured outcome of the first runtime-mask slice on `sort_window`:

| shape | before | after |
|---|---:|---:|
| generated prepared | `61.49 us` | `55.58 us` |
| CnP prepared | `53.33 us` | **`52.54 us`** |

So this first slice improves the last good CnP baseline by about `0.79 us`
(`~1.5%`) on `sort_window/prepared_execute`.

Delta vs generated:

| shape | delta |
|---|---:|
| prepared | `-8.16 us` (`-13.3%`) |

Sorter/CnP retrospective:

| change | area | data structure impact | measured result on `sort_window` | status |
|---|---|---|---:|---|
| Prepare-time `OP_SorterCompare` equality shapes | `vdbe_cnp.c`, `vdbe_ops_sorter.c`, `vdbesort.c` | add `VdbeSorter.cnpEqShapeByCount[5]` prepare-time metadata only | small/noise-level opcode-side gain; real hotspot stayed in merge compare | kept |
| Restrict exact signed/unsigned equality binding to fixed-width integer field types | same | no row-format change | correctness fix; avoided over-specializing generic SQL `INTEGER`/`UNSIGNED` keys | kept |
| Cached runtime intlike row mask | `vdbesort.c` merge/write/read paths | `SorterRecord.typeMask`, `PmaReader.typeMask`, PMA row format adds `1` byte after record length | `53.33 us -> 52.54 us` for CnP (`~1.5%`) | kept |
| Recoverable key-part offset cache | `vdbesort.c` merge/write/read paths | add transient `partOffsets[]` / `offsetPartCount` to in-memory `SorterRecord` and current `PmaReader`; PMA row format unchanged | `52.54 us -> 52.57 us` on the confirming rerun (`~flat`) | kept |
| Same-type `MP_INT` / `MP_UINT` branch inside `vdbeSorterCompareIntLike3Fast()` | merge comparator | none | regressed focused benchmark | reverted |
| Out-of-line C++ 3-part pair-mask helper | separate `.cc` helper | no lasting row-format change | reruns around `60.14 us` and `61.80 us`; helper itself showed up in `perf` | reverted |
| Local same-TU 3-part pair-mask table | `vdbesort.c` only | none | one rerun at `55.04 us`, next at `57.48 us`; too unstable and not better than last proven baseline | reverted |

Surviving sorter data-flow / storage changes:

- sorter rows now carry a one-byte cached runtime intlike mask in both
  in-memory `SorterRecord` objects and PMA records;
- PMA row layout is now `varint(length) + 1-byte mask + raw MsgPack row`;
- in-memory sorter rows and current PMA-reader rows now also cache recoverable
  offsets of the hot key parts; these offsets are rebuilt from the raw row
  bytes after spill and are not serialized into PMAs;
- sorter merge comparator callbacks now see both row masks in addition to the
  raw row pointers and optional per-row part-offset arrays;
- `OP_SorterCompare` CnP specialization keeps only prepare-time shape metadata
  in `cnpEqShapeByCount[]`; it does not depend on the per-row runtime mask.

What the experiments clarified:

- the opcode-side `OP_SorterCompare` JIT/CnP work is useful but not the main
  remaining source of sorter cost on `sort_window`;
- the runtime-mask merge comparator work is the only sorter-specific change so
  far with a clear, repeatable positive delta;
- a recoverable offset cache does remove repeated left-to-right field walking
  for the same sorter row, but by itself it is essentially neutral on
  `sort_window`; the remaining cost is still in per-field decode and compare
  logic, not only in rescanning to field boundaries;
- the bad result from the out-of-line C++ helper does not disprove a
  specialization matrix in general, only that helper/bridge shape;
- the local same-translation-unit matrix removed the visible helper-boundary
  cost in `perf`, but its benchmark result was still too unstable to keep.

Nearest-term plan from the current runtime-mask baseline:

- keep the one-byte per-row runtime mask and the current masked merge compare
  path as the proven sorter baseline;
- keep the recoverable per-row offset cache as cheap infrastructure, but do not
  treat it as a proven standalone speedup;
- do not treat the runtime mask as a new prepare-time JIT selector; it remains
  runtime data;
- keep the high-level opcode-side CnP selector coarse and separate from merge
  comparator work;
- do not add a heavier decoded-value cache next; the offset-cache result says
  the next sorter-specific gain, if any, needs to reduce compare/decode logic
  itself rather than only field-boundary rescans;
- if we revisit specialization matrices, target only the hot 3-part intlike
  merge comparator first;
- for an `N`-part intlike key the exact row-pair matrix has `4^N` shapes, so
  the practical first target remains the 3-part `sort_window` case (`64`
  exact bodies);
- any future JIT-shaped matrix attempt has to avoid a helper/bridge cost per
  compare and should look more like a direct jump into an exact body than a
  normal out-of-line helper call;
- other high-value unexplored areas remain `sqlVdbeSorterWriteFromMems()`,
  `vdbe_op_column_integer_exact_fast`, and sustained-shape dispatch designs
  that move the runtime shape choice farther out of the hottest compare loop.

## 7. Why generic `key_compare()` is not the first backend

There is an existing raw key comparator in the tuple layer, but it is not the
best first implementation target here.

Reasons:

- it is still designed as a generic engine-wide key comparator;
- it carries more abstraction than a sorter-local fixed-shape SQL case needs;
- it expects the caller to already be in raw-key field space, not "sorter blob"
  framing;
- earlier experimentation showed it can shift cost into generic compare helpers
  and MsgPack decode overhead rather than reduce it.

So the intended direction is:

- sorter-local fast path first;
- generic tuple comparator remains fallback/reference behavior.

## 8. Zerocopy strategy

"Zerocopy" here should mean:

- no full `UnpackedRecord` construction on the hot path;
- no full per-record field extraction;
- no copying string/bin payloads into owned buffers;
- only decode the minimal prefix needed to decide ordering.

Existing MsgPuck support already aligns with this:

- `mp_decode_strl()` / `mp_decode_binl()` expose payload slices directly;
- `mp_next()` advances over whole values cheaply when we do not need them;
- `mp_decode_extl()` plus one or two direct decoders already supports
  "skip framing, keep payload" patterns elsewhere in the tree.

It should not mean:

- literally zero decoding work.

For integer-like fields, direct decode from MsgPack is still necessary, but it
is much cheaper than materializing `Mem` and walking the generic comparison
machinery.

## 9. Benchmark targets

Primary benchmark:

- `sort_window/prepared_execute`

Secondary validation:

- `sort_window/automatic_execute`
- `sort_payload/prepared_execute`
- `sort_text_window/prepared_execute`
- other sorter-using ORDER BY workloads if available
- regression checks for scan-heavy and non-sorter workloads

Perf goals:

- reduce `sqlVdbeRecordUnpackMsgpack`
- reduce `mem_from_mp_ephemeral`
- reduce `mem_cmp_msgpack`
- shift top sorter-compare cost into a small specialized compare symbol

Correctness checks:

- mixed ASC/DESC ordering
- ties on prefix fields
- negative integers and uint/int mixtures
- NULL handling fallback correctness
- binary string compare fallback correctness
- non-ASCII substring fallback correctness

## 10. Suggested implementation order

1. Keep the existing sorter-local plan on `VdbeSorter`.
2. Keep key-shape classification table-driven from `key_def`.
3. Preserve the dedicated integer-like comparator family for integer-heavy
   workloads.
4. Use the wider mixed simple comparator only when needed.
5. Keep the direct sorter-only write path instead of going through
   `MakeRecord` where possible.
6. Keep the one-pass int-like direct writer for small all-integer-like sorter
   keys.
7. Add prepare-time shape objects for comparator emission, with per-column type
   and direction instead of whole-key family buckets.
8. Keep the equality-only `OP_SorterCompare` shape binding as the opcode-side
   endpoint for now.
9. Return to `OP_Column` / field-ref work if sorter work stops paying.
10. If sorter compare is still leading after opcode-side emission, target the
    merge-path comparator in `vdbesort.c` next.

## 11. Recommended first concrete milestone

The first concrete milestone was:

- implement a sorter-local raw comparator for
  `part_count <= 4`, all `FIELD_TYPE_INTEGER`/`FIELD_TYPE_UNSIGNED`,
  no collation, no nullable parts;
- keep the current unpack-based path as fallback;
- measure on `sort_window`;
- confirm that the fast path improves the sorter merge profile before adding
  any CnP/JIT emission work.

That milestone is small enough to validate the idea and large enough to answer
the real question:

- does sorter-local raw typed compare beat unpack-and-compare on our workload?

That milestone is complete. It answered the main question positively: sorter-
local raw typed compare beats unpack-and-compare on the target workload.

The next concrete milestone should be:

- keep the committed direct `SorterInsert` path for sorter-only sites;
- keep the current raw sorter compare family as the reference behavior;
- keep the new prepare-time comparator shape descriptor derived from `key_def`;
- keep the equality-only `OP_SorterCompare` handlers as the narrow opcode-side
  specialization layer;
- re-profile `sort_window/prepared_execute`;
- if sorter compare is still on top, move the next effort to the merge-path
  comparator instead of adding more opcode-side wrapper variants.

Text-sort checkpoint:

- `sort_text_window` is now the useful validation case for computed text keys;
- the current win there came from narrowing the producer path before sorter
  insert, not from another sorter comparator family;
- the current CnP-side `SUBSTR(3)` specialization keeps generic behavior by
  falling back for non-string, NULL, aliased-output, or non-ASCII-prefix
  cases;
- the mixed text sorter now also has an exact raw comparator for the current
  4-part `[str, intlike, str, intlike]` key shape, so it no longer pays the
  generic per-part kind switch in `vdbeSorterCompareSimpleFast`;
- latest discard-mode medians on `sort_text_window/prepared_execute`:
  generated `46.49 us`, MCJIT `51.69 us`, CnP `42.38 us`;
- latest validation reruns on the integer-heavy sorter cases remained healthy:
  `sort_window` CnP `50.17 us`, `sort_payload` CnP `65.14 us`.

Hybrid specialization plan:

- do not keep growing a hand-written list of exact mixed-shape comparator
  helpers;
- instead, preinstantiate a small bounded set of compile-time specializations
  for shapes that are both hot and structurally stable;
- the current examples are the all-intlike `2/3/4` families and the mixed
  `[str, intlike, str, intlike]` sorter shape from `sort_text_window`;
- use C++ templates or an equivalent compile-time generator so those fixed
  shapes share one implementation scheme instead of many ad hoc bodies;
- for shapes outside that bounded set, prefer exact JIT/CnP-emitted comparator
  bodies when prepare-time shape analysis says they are specialization-worthy;
- keep the existing generic mixed fast comparator and unpacked fallback as the
  semantic safety net.

Recommended next architecture:

1. precompiled tier:
   a small global matrix of hot sorter comparator layouts;
2. JIT tier:
   exact per-statement comparator emission for the long tail of clean mixed
   scalar shapes;
3. fallback tier:
   the current generic mixed comparator and final generic unpack path.

Current hybrid checkpoint:

- the static tier now routes the current `[str, intlike, str, intlike]`
  comparator through a template-instantiated C++ body instead of a handwritten
  one-off implementation;
- two focused fallback workloads now exist:
  - `sort_text_shape_fallback` keeps the text top-K shape but uses a mixed key
    layout outside the static template set, so compare falls back to the
    generic mixed fast comparator;
  - `sort_text_substr_fallback` keeps the comparator shape but switches to
    `substr(s3, 7)`, so the producer falls back to the generic builtin path;
- and a wide probe workload now exists too:
- `sort_text_wide_probe` uses a ten-part mixed key and now exercises the
  stitched long-tail mixed comparator path built from generated
  string/intlike fragments;
- it now also runs with a much heavier default execute loop and a much wider
  text window, so profiler runs spend multiple seconds in steady-state
  compare/write/column work instead of mostly in setup noise;
- latest wide-probe checkpoints after gating stitched sorter code to
  `VDBE_DISPATCHER=cnp` only:
  - pre-template specialized `OP_Column` split:
    - `generated`: `341.85 us`;
    - `CnP`: `354.82 us`;
  - after the C++ `if constexpr` offset-slot refactor:
    - focused rerun: `generated` `332.53 us`, `CnP` `324.60 us`;
    - perf-backed rerun: `generated` `348.12 us`, `CnP` `336.29 us`;
- after narrowing column-group formation to only the sites that still need
  sequential field-ref cache seeding:
  - clean CnP-only wide-probe rerun improved from about `353.05 us` to
    `336.40 us`;
  - the corresponding CnP `perf` run dropped
    `vdbe_field_ref_preload_group_fast` from `16.27%` to `6.22%`;
- refreshed heavy sort matrix, `prepared_execute`, discard mode, `runs=3`:

| workload | generated | MCJIT | CnP |
| --- | ---: | ---: | ---: |
| `sort_window` | `135.16 us` | `126.33 us` | `119.43 us` |
| `sort_payload` | `181.17 us` | `184.13 us` | `161.44 us` |
| `sort_text_window` | `200.49 us` | `220.91 us` | `167.91 us` |
| `sort_text_shape_fallback` | `212.60 us` | `203.24 us` | `177.29 us` |
| `sort_text_substr_fallback` | `214.29 us` | `216.54 us` | `187.52 us` |
| `sort_text_wide_probe` | `342.42 us` | `343.92 us` | `335.06 us` |

- the current template-backed static tier is architecturally correct, but on
  the handled case it is roughly neutral versus the earlier handwritten helper,
  not a fresh speedup by itself;
- the long-tail mixed comparator now follows the same copy-and-patch mechanics
  as the rest of CnP:
  - build-time generated preserve-none fragment bodies;
  - extracted reloc metadata;
  - one shared ABI bridge;
  - stitched next/fallback/helper relocations at runtime;
  - no raw x86 byte emission in the sorter path.

Annotated wide mixed disassembly:

- The current `sort_text_wide_probe` stitched body begins like this:

```asm
0x...c000: push   %rax
0x...c001: movabs $vdbeSorterCompareCnpFieldString,%rax
0x...c00b: mov    %r12,%rdi
0x...c00e: mov    %r13,%rsi
0x...c011: callq  *%rax
0x...c013: test   %eax,%eax
0x...c015: je     0x...c029
0x...c017: cmp    $0xfffffffe,%eax
0x...c01a: jne    0x...c036
0x...c01c: movabs $fallback_entry,%rax
0x...c026: pop    %rcx
0x...c027: jmpq   *%rax
0x...c029: movabs $next_fragment,%rax
0x...c033: pop    %rcx
0x...c034: jmpq   *%rax
0x...c036: pop    %rcx
0x...c037: retq
```

- Meaning of the three exits:
  - `rc == 0`: this key part is equal, jump to the next stitched fragment;
  - `rc == -2`: runtime type/value shape is outside the specialized path, jump
    to the generic fallback comparator;
  - any other `rc`: ordering is decided, return immediately from the whole
    comparator.

- Why there are many `retq`:
  - each key-part fragment is an early-exit point for lexicographic compare;
  - once one part decides ordering, the rest of the key must not execute.

- Why there are many `push %rax` / `pop %rcx` pairs:
  - they are emitted by clang from the preserve-none fragment templates;
  - they are only stack-balance glue around the helper-call shape and the
    patched tail jumps;
  - `pop %rcx` is just a scratch discard to undo the push before `jmp`/`ret`,
    not a logical restore of `rcx`.

- Why the debugger was used instead of `EXPLAIN (disassembly = true)`:
  - `EXPLAIN` currently disassembles VDBE bytecode and the main VDBE CnP
  fragments attached to opcode PCs;
  - the sorter merge comparator is a separate runtime-stitched artifact owned
  by `VdbeSorter`, so it is not visible to `EXPLAIN` today;
  - the live runtime bytes therefore have to be inspected from memory.

Heavy-run profile split after gating:

- generated `sort_text_wide_probe` now spends most of its time in the generic
  decode and compare stack:
  - `vdbe_field_ref_fetch_data` `12.44%`
  - `mem_from_mp_ephemeral` `10.89%`
  - `vdbe_op_column` `6.79%`
  - `mem_to_mp_buf` `5.71%`
  - `vdbeSorterCompareSimpleFast` `5.31%`
  - `vdbe_exec_generated_dispatcher` `4.29%`
- CnP before the template refactor spent that time differently:
  - `vdbe_op_column_typed_offset_slot_fast` `25.61%`
  - `mem_to_mp_buf` `4.86%`
  - `mem_mp_size` `4.02%`
  - `vdbeSorterCompareCnpFieldString` `3.64%`
  - `sqlVdbeSorterWriteFromMems` `3.34%`
  - `vdbeSorterCompareMixedCnpFast` `1.91%`
- CnP after the template refactor no longer pays through one monolithic
  offset-slot helper. The largest remaining buckets are:
  - `vdbe_field_ref_preload_group_fast` `16.07%`
  - `mem_to_mp_buf` `5.55%`
  - `vdbe_op_column_string_offset_slot_static_fast` `4.17%`
  - `mem_mp_size` `3.97%`
  - `vdbeSorterCompareCnpFieldString` `3.73%`
  - `sqlVdbeSorterWriteFromMems` `3.40%`
  - `vdbe_op_column_integer_offset_slot_static_group_fast` `2.16%`
  - `vdbe_op_column_typed_exact_fast` `2.03%`
- after teaching the compiler to form dense/prefetch groups only for the
  fields that still fall back to sequential `vdbe_field_ref_fetch_data_inline()`
  work:
  - `vdbe_field_ref_preload_group_fast` `6.22%`
  - `mem_to_mp_buf` `5.10%`
  - `vdbe_field_ref_fetch_data_offset_slot_fast` `4.83%`
  - `mem_mp_size` `4.09%`
  - `vdbe_op_column_string_offset_slot_static_fast` `3.88%`
  - `sqlVdbeSorterWriteFromMems` `3.87%`
  - `vdbeSorterCompareCnpFieldString` `3.67%`
  - `vdbe_op_column_typed_exact_fast` `3.59%`
  - `vdbe_op_column_integer_offset_slot_static_fast` `2.36%`
  - `vdbe_cnp_column_group_get` `2.12%`

Why `mem_from_mp_ephemeral` disappears on the CnP side:

- generated still reaches many text fields through the generic `OP_Column`
  decode path, so `mem_from_mp_ephemeral()` remains visible;
- CnP typed column helpers bypass that generic decoder and materialize the
  result directly with exact typed setters such as string-ephemeral and
  integer-fast paths;
- so the missing `mem_from_mp_ephemeral` symbol is not a regression: it is a
  sign that the generic decode path was successfully avoided.

Current reading of the wide mixed case:

- the template refactor is mechanically doing the right thing:
  `static` / `path` / `runtime` and `group` / `no-group` shapes now compile to
  distinct bodies, and the runtime nav/group branches are gone from the
  corresponding entrypoints;
- the next necessary step was to stop forming broad dense/preload envelopes for
  sites that already have direct static-slot or anchor+hops navigation;
- that refinement is now in place, and it materially reduced the row-local
  preload cost on `sort_text_wide_probe`;
- the next largest scan-side buckets are now the actual direct navigation and
  encode pieces, not the old monolithic helper or the broad preload loop.

Next step for later: expose sorter JIT disassembly in `EXPLAIN`

1. Store persistent sorter-compare compile metadata on the statement side.
   Needed fields:
   - comparator mode: static template / stitched mixed / generic;
   - part count, part kinds, DESC mask;
   - fragment sequence for the stitched case;
   - code pointer and size after stitching.
2. Register sorter comparator code spans in the same debug/disasm registry used
   by the main CnP path.
   This lets tooling address a sorter comparator as a named compiled artifact,
   not as anonymous executable memory.
3. Extend `EXPLAIN (disassembly = true)` with an auxiliary compiled-artifacts
   section.
   For sorters, print:
   - key shape summary;
   - whether static or stitched path was selected;
   - fragment order for stitched mixed comparators;
   - final native disassembly.
4. Keep fragment-boundary annotations.
   A flat instruction dump is not enough for review; we want `part 0`, `part 1`
   and `fallback` boundaries visible in the `EXPLAIN` output.
