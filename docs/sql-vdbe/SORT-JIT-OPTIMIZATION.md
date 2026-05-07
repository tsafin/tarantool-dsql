# Sort Comparator JIT Optimization Plan

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
- those wrappers are normal functions, not inlined bodies, so the generated ASM
  still has a call into the chosen wrapper;
- the win comes from compile-time wrapper selection and fixed-count compare
  shapes, not from pasting the whole comparator into the fragment;
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

Current baseline `sort_window` rerun with this selector in place:

| shape | median |
|---|---:|
| generated prepared | `68.32 us` |
| generated automatic | `63.36 us` |
| CnP prepared | **`59.39 us`** |
| CnP automatic | **`52.58 us`** |

Delta vs generated:

| shape | delta |
|---|---:|
| prepared | `-8.93 us` (`-13.1%`) |
| automatic | `-10.78 us` (`-17.0%`) |

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
7. Return to `OP_Column` / field-ref work once sorter-local writer costs stop
   being a large lever.
8. Only after C fast paths on compare and write prove out, add emitted/JIT
   comparator generation.

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

- keep the new direct `SorterInsert` path for sorter-only sites;
- keep the one-pass integer-like direct writer for supported sorter-key shapes;
- re-profile `sort_window`;
- with `OP_Column` still dominant, move back to row-local `OP_Column` /
  field-ref planning for the next code change.
