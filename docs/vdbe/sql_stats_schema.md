# SQL statistics persistence schema (v1)

> **Status: APPROVED FOR IMPLEMENTATION (2026-10-05).** This document fixes
> the v1 system-space IDs, positional tuple representation, generation token,
> publication model, and reader compatibility policy. Bootstrap/upgrade code
> must implement this contract exactly; incompatible changes require a new
> format version and migration review.

## Scope and audit findings

This contract covers the first persisted relation and index summaries needed
by S1. It does not define column sketches or histograms (S2), collection
algorithms, SQL grammar, or the `SqlStatsSnapshot` in-memory ABI. The active
tree currently has no `_sql_stats_relation` or `_sql_stats_index`, no
`ANALYZE` grammar, and no stats snapshot loader. Existing estimates come from
`default_tuple_est[]` and live primary-index size; historical `_sql_stat1` /
`_sql_stat4` support is vestigial (see `s0_audit_report.md`).

System space IDs are declared in `src/box/schema_def.h`. The reserved system
range is 256–511; existing data dictionary spaces occupy selected IDs in
that range, while ordinary user spaces are allocated above 511. System space
IDs are durable catalog identities, not implementation-local constants.
`schema_init()` in `src/box/schema.cc` creates bootstrap spaces with recovery
ordering constraints: `_schema` must be recovered first. Any registration
proposal must preserve that ordering and correctly handle initial bootstrap,
snapshot recovery, WAL recovery, replication, and upgrade from catalogs that
predate the new spaces. ID availability and a viable registration path still
need a dedicated source audit at implementation time.

The planner-facing design in `statistics_implementation_plan.md` calls for an
immutable snapshot built once per prepare and versioned persistence. This
document makes the minimum relation/index row shape explicit while retaining
the contract's open choices.

## Proposed spaces and keys

Use separate system spaces named `_sql_stats_relation` and
`_sql_stats_index`; do not reuse `_sql_stat1` or SQLite's text/blob encodings.
Both spaces are keyed by stable catalog identities, not names:

| Space | System ID | Primary key | One row represents |
| --- | ---: | --- | --- |
| `_sql_stats_relation` | `382` (`BOX_SQL_STATS_RELATION_ID`) | `(space_id)` | The currently published stats generation for one relation |
| `_sql_stats_index` | `383` (`BOX_SQL_STATS_INDEX_ID`) | `(space_id, index_id)` | The currently published stats generation for one index |

Index identity is the pair `(space_id, index_id)` because index IDs are only
unique within a space. Names are diagnostic metadata and must not be used as
keys: rename should not detach stats from the object. Drop/recreate behavior
must ensure old rows cannot be read as stats for a new object with reused IDs;
the generation/publication rules below need to be integrated with schema
change handling to guarantee this.

## Tuple representation and common envelope

Both spaces use ordinary positional tuples; only the last field is a MsgPack
map, so payload additions can be versioned without moving envelope fields.
Unsigned values use canonical MessagePack unsigned encoding; `confidence` is
a finite IEEE-754 double; `generation_id` is exactly 16 bytes of UUID binary
data. `collected_at_ns` is the collector's UTC Unix epoch in nanoseconds and
is diagnostic only: it is not a freshness or ordering input.

`_sql_stats_relation` has this exact v1 layout:

| Field | Position | Type |
| --- | ---: | --- |
| `space_id` | 0 | unsigned |
| `format_version` | 1 | unsigned (`1`) |
| `generation_id` | 2 | binary UUID (16 bytes) |
| `collected_at_ns` | 3 | unsigned |
| `catalog_version` | 4 | unsigned |
| `modification_epoch` | 5 | unsigned |
| `sampled_rows` | 6 | unsigned |
| `sampled_bytes` | 7 | unsigned |
| `confidence` | 8 | finite double in `[0, 1]` |
| `payload` | 9 | MsgPack map |

`_sql_stats_index` has `space_id` and `index_id` at positions 0 and 1, then
the same envelope from `format_version` through `payload` at positions 2–10.
Its primary key is `(space_id, index_id)`. No envelope field is nullable in
v1; absent optional payload values are omitted from the map. Writers reject an
invalid value rather than serializing a partial row, and readers treat a
malformed row as unavailable statistics.

Every row carries an explicit unsigned `format_version` and `generation_id`.
The format version describes the row's payload contract; it is independent of
the SQL schema/catalog version and of the in-memory snapshot API version.
Unknown versions are ignored as unavailable statistics, with the normal
planner fallback, rather than interpreted partially.

The proposed logical envelope fields are:

| Field | Type | Meaning |
| --- | --- | --- |
| `format_version` | unsigned integer | Payload version; v1 is `1` |
| `generation_id` | 16-byte UUID binary token | Collection publication identity shared by relation and its index rows |
| `collected_at_ns` | unsigned integer | UTC Unix epoch nanoseconds; diagnostic only |
| `catalog_version` | unsigned integer | Catalog generation observed by collection |
| `sampled_rows` | unsigned integer | Number of rows actually inspected |
| `sampled_bytes` | unsigned integer | Approximate bytes inspected under the collection budget |
| `confidence` | finite number in `[0, 1]` | Normalized confidence; precise calibration remains open |
| `payload` | MsgPack map | Versioned relation- or index-specific summary |

Persistent writers must reject NaN/infinity and out-of-range values; readers
must treat malformed rows as unavailable stats and report a diagnostic without
failing ordinary query preparation.

## Version 1 payload proposal

Relation `payload`:

```text
{
  row_count: unsigned integer,
  average_row_width: finite non-negative number,
  cardinality_semantics: "visible_rows" | "physical_tuples" | "estimate",
  modification_epoch: optional unsigned integer
}
```

Index `payload`:

```text
{
  index_kind: string or stable enum,
  tuple_count: unsigned integer,
  distinct_prefixes: [unsigned integer, ...],
  average_key_width: optional finite non-negative number,
  physical_bytes: optional unsigned integer
}
```

`distinct_prefixes[i]` is the estimated number of distinct keys in the first
`i + 1` index parts, clamped to `[1, tuple_count]` for a non-empty index and
zero for an empty index. The array length must equal the index's part count at
collection time; index-definition mismatch makes the row stale/unavailable.
Exact-key uniqueness may provide an exact prefix distinct count where the
index definition proves it, but all estimates retain an explicit
cardinality-semantics definition. In v1 it belongs in the payload map.

The relation and index payloads intentionally exclude column MCVs, NDV
sketches, histograms, multi-column groups, and dependency statistics. Those
belong to S2 and require their own format/schema review; v1 does not allocate
`_sql_stats_column` or `_sql_stats_multicolumn` IDs or layouts.

## Publication, consistency, and stale data

The implementation replaces one relation row and its complete index-row set
in one transaction. The writer generates one UUID for that transaction,
writes it into the relation and every index row, and deletes stale index rows
for the relation before commit. A prepare-time loader reads the relation first
and accepts index rows only when each has the same UUID, format version,
catalog version, and modification epoch. It never mixes generations.
Transactional visibility supplies the publication barrier: a reader sees
either the prior complete set or the new complete set. V1 has no manifest
space or two-phase publication protocol.

Before use, validate that the space and index still exist, the index
definition matches, the format is supported, and the generation/catalog
version is compatible with the current schema. Missing, stale, malformed, or
unsupported stats fall back to existing estimates. Query preparation must
not trigger collection synchronously. DDL invalidation, object ID reuse,
replication ordering, and upgrade/downgrade behavior need tests in the storage
implementation review.

## Constraints

- Per-relation and per-database persisted bytes must be bounded.
- Relation/index rows must not contain SQL text or rely on object names.
- Counters are non-negative integers; floating summaries are finite and
  non-negative; confidence is finite and bounded.
- Readers skip unsupported payload versions and preserve normal query
  correctness through fallback.
- Writers replace a complete generation transactionally and never update a
  subset of one generation in place.
- Schema, catalog, payload format, and snapshot API versions are distinct
  concepts and must not be overloaded into one version number.

## Implementation obligations

1. **Bootstrap and upgrade:** register IDs 382/383 in `schema_def.h`, create
   the spaces and primary indexes during fresh bootstrap, and add an idempotent
   upgrade step for existing catalogs. Test snapshot/WAL recovery and
   replication ordering before declaring persistence production-ready.
2. **Reader/writer validation:** enforce the exact tuple layouts above,
   bounded payload bytes, finite numeric fields, and complete same-generation
   replacement. Unknown future `format_version`s are unavailable, not errors.
3. **Cardinality semantics:** define what memtx and Vinyl counts mean under
   deletes, MVCC visibility, compaction, and sampling; choose the canonical
   `row_count` meaning and conversion rules.
4. **Index summaries:** confirm prefix NDV semantics, empty-index encoding,
   unique-index exactness, and whether physical bytes/average key width are
   required in S1.
5. **Staleness:** define catalog/modification epoch sources, thresholds,
   invalidation behavior, and compatibility on DDL or ID reuse.
6. **Confidence:** calibrate confidence before values are persisted; the clock
   representation and version-reader policy are fixed above.

## Relationship to other contracts

This is a proposed persistence contract only. The planner consumes a
separately versioned immutable `SqlStatsSnapshot`; collection uses the
engine-specific sampling APIs; `ANALYZE` publishes stats; and S2 adds column
statistics only after a separate schema review. See
`statistics_implementation_plan.md` for the wider collection and algorithm
plan and `roadmap.md` for task ownership and gates.
