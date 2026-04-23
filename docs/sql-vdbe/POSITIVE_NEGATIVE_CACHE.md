# VDBE JIT Caching Strategy

The JIT compiler in `src/box/sql/vdbe_jit.c` uses three independent caches to
avoid redundant LLVM compilation.  Understanding their interaction is essential
for correctness.

---

## Cache Types

### 1. `jit_negative_cache` — per-statement-id negative cache

| Property   | Value |
|------------|-------|
| Key        | `(stmt_id, schema_version)` (64-bit combined) |
| Value      | tombstone (value not used, only presence matters) |
| Purpose    | "This exact query text never compiles profitably" |
| Populated  | When profitability check fails for a non-oneshot statement, or when the program is too large (`nOp > VDBE_JIT_MAX_OPS`), or when `vdbe_jit_note_fallback()` observes a JIT fallback at `OP_Halt` |
| Invalidated | On schema change (`schema_version` changes) |

**Important caveat:** `vdbe_jit_note_fallback()` adds the stmt-id to this cache
when the JIT falls back at `OP_Halt` (query returns zero rows on an empty
table).  This is intentional — such programs don't benefit from JIT.  However,
it creates a trap: after an `auto_stmt_cache` eviction, the **new** Vdbe for
the same query has no `jit_func` yet, but the stmt-id negative cache blocks
`vdbe_jit_compile` from even trying.  The positive shape cache check (see
below) must happen **before** this check to break the deadlock.

---

### 2. `jit_shape_negative_cache` — per-opcode-shape negative cache

| Property   | Value |
|------------|-------|
| Key        | `(shape_hash, schema_version)` where `shape_hash = hash(nOp, opcode[], p4type[], p5[])` — excludes operands and jump targets |
| Value      | tombstone |
| Purpose    | "Programs with this opcode structure don't pass the profitability filter" |
| Populated  | When profitability check fails for an `is_literal_oneshot` program, or when program is too large |
| Invalidated | On schema change |

The shape hash deliberately **excludes** `p1`, `p2` (jump targets), and `p3`
so that different parameterisations of the same query (e.g. `SELECT 1` vs
`SELECT 2`, or correlated jumps) share the same shape entry.

---

### 3. `jit_shape_positive_cache` — per-full-shape positive cache

| Property   | Value |
|------------|-------|
| Key        | `(full_shape_hash, schema_version)` where `full_shape_hash = hash(nOp, opcode[], p2[], p3[], p4type[], p5[])` — includes jump targets, excludes literals in `p1` |
| Value      | `jit_func` pointer (native function compiled by LLVM) |
| Purpose    | "Programs with this exact opcode+jump structure already have a compiled function — reuse it directly" |
| Populated  | By `vdbe_jit_compile_cached()` after a successful force-compile |
| Invalidated | On schema change |

The full shape hash **includes** `p2` and `p3` (jump targets) but **excludes**
`p1` (literal operands like `SELECT 42`).  This means two queries that differ
only in literal constants share a compiled function, which is correct because
the generated native code loads `p1` at runtime.

---

## Decision Flow in `vdbe_jit_compile`

```
vdbe_jit_compile(p)
│
├─ is_literal_oneshot && nOp > 0?
│    └─ YES → check jit_shape_positive_cache(full_shape_hash)
│                  HIT  → set p->jit_func, return 0          ← FAST PATH
│                  MISS → continue
│
├─ can_cache_negative (stmt_id != 0, not force-compiled)?
│    └─ YES → check jit_negative_cache(stmt_id)
│                  HIT  → jit_compiled=0, return 0            ← SKIP
│                  MISS → continue
│
├─ is_literal_oneshot?
│    └─ YES → check jit_shape_negative_cache(shape_hash)
│                  HIT  → jit_compiled=0, return 0            ← SKIP
│                  MISS → continue
│
├─ nOp > VDBE_JIT_MAX_OPS?
│    └─ YES → add to negative caches, return 0
│
├─ Profitability check (inline_count >= threshold, etc.)
│    └─ FAIL → [is_literal_oneshot] add to jit_shape_negative_cache
│              return 0
│
└─ LLVM compile → set p->jit_func, p->jit_module, jit_compiled=1
```

**Critical ordering rule:** The positive shape cache check must come
**before** the stmt-id negative cache check.  Without this ordering, a query
that was force-compiled via `vdbe_jit_compile_cached` (and therefore added to
the positive cache) will be silently skipped because `vdbe_jit_note_fallback`
may have already recorded its stmt-id in the negative cache.

---

## `vdbe_jit_compile_cached` — force-compile for cached statements

When `auto_stmt_cache` stores a statement (cache miss path), `execute.c` calls
`vdbe_jit_compile_cached(stmt)` to give the JIT a second chance to compile the
statement as if it were a prepared statement (`is_prepared_stmt = 1`).

After a successful compile, `vdbe_jit_compile_cached` does two extra things:

1. **Populate the positive shape cache** with `full_shape_hash → jit_func`.
   This allows any future Vdbe created for the same query (after an eviction)
   to reuse the compiled function without a new LLVM compilation.

2. **Clear `p->jit_module`** to `NULL`.  The LLVM execution engine takes
   ownership of the module after `LLVMAddModule`; setting `jit_module = NULL`
   prevents `vdbe_jit_cleanup` from calling `LLVMRemoveModule` on a module it
   no longer owns (double-free).

---

## `vdbe_jit_note_fallback` — recording fallback reasons

Called by the VDBE execution loop when the JIT function returns early (at an
unsupported opcode).  Adds the stmt-id to `jit_negative_cache` only for
`OP_Halt` fallbacks (meaning the whole program ran to completion but returned
no rows).  Other opcodes do not add to the negative cache; the query may become
profitable again after the table is populated.

---

## Hash Functions

| Hash | Fields included | Used for |
|------|----------------|----------|
| `jit_stmt_shape_hash` | `nOp, opcode[], p4type[], p5[]` | shape negative cache key (`shape_hash`) |
| `jit_stmt_full_shape_hash` | `nOp, opcode[], p2[], p3[], p4type[], p5[]` | positive cache key (`full_shape_hash`); also used as `pos_hash` in the early positive-cache check |

Both hashes use a Fibonacci mixing step:
```c
hash = hash * 6364136223846793005ULL + value + 1442695040888963407ULL;
```

---

## Interaction with `auto_stmt_cache`

The `auto_stmt_cache` (256-slot direct-mapped cache in `execute.c`) stores
compiled `Vdbe *` objects keyed by `(sql_text_hash, schema_ver, flags)`.  A
slot collision evicts the previous entry via `sql_stmt_finalize`.

After eviction:

1. A new `Vdbe` is created for the same query text.
2. `vdbe_jit_compile` is called on the new Vdbe.
3. If the positive shape cache has an entry for `full_shape_hash`, the new
   Vdbe gets `jit_func` immediately — **zero LLVM compilations**.
4. If the positive cache misses (first-time compile), `vdbe_jit_compile_cached`
   runs the force-compile and populates the positive cache for next time.

This ensures that frequently evicted queries (those whose cache slots collide)
incur at most **one** LLVM compilation each, not one per eviction cycle.

---

## Bug History

Three interacting bugs (fixed in commit `sql/jit: fix shape cache bugs ...`)
caused per-pass LLVM recompilation of evicted queries:

| # | Bug | Symptom | Fix |
|---|-----|---------|-----|
| 1 | Positive cache check ordered **after** stmt-id negative cache | `vdbe_jit_note_fallback` + eviction → force-compiled function silently skipped every pass | Move positive cache check to top of `vdbe_jit_compile` |
| 2 | `vdbe_jit_compile_cached` never populated positive shape cache | No positive cache entry → LLVM re-compile after every eviction | Add `jit_shape_positive_cache_add` after force-compile; set `jit_module = NULL` |
| 3 | Profitability fail did not add shape to shape negative cache | Redundant profitability re-analysis on every eviction | Add `jit_shape_negative_cache_add` in profitability fail path for `is_literal_oneshot` |

Combined effect before fix: MCJIT hot-loop 397 µs/query (59× slower than
interpreter).  After fix: 7.2 µs/query (on par with interpreter at 7.1 µs/q
and CnP at 6.9 µs/q).
