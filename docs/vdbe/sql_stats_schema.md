# SQL statistics persistence schema (DRAFT)

> **Status: DRAFT — not approved for implementation.** This proposal assigns
> no system-space IDs and changes no allocation or bootstrap code. Persistent
> IDs, tuple keys, payload versions, and recovery behavior are compatibility
> decisions; obtain human sign-off before implementing them.

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

| Space | Proposed primary key | One row represents |
| --- | --- | --- |
| `_sql_stats_relation` | `(space_id)` | The currently published stats generation for one relation |
| `_sql_stats_index` | `(space_id, index_id)` | The currently published stats generation for one index |

Index identity is the pair `(space_id, index_id)` because index IDs are only
unique within a space. Names are diagnostic metadata and must not be used as
keys: rename should not detach stats from the object. Drop/recreate behavior
must ensure old rows cannot be read as stats for a new object with reused IDs;
the generation/publication rules below need to be integrated with schema
change handling to guarantee this.

## Common envelope

Every row carries an explicit unsigned `format_version` and `generation_id`.
The format version describes the row's payload contract; it is independent of
the SQL schema/catalog version and of the in-memory snapshot API version.
Unknown versions are ignored as unavailable statistics, with the normal
planner fallback, rather than interpreted partially.

The proposed logical envelope fields are:

| Field | Type | Meaning |
| --- | --- | --- |
| `format_version` | unsigned integer | Payload version; initial proposal is version 1, subject to approval |
| `generation_id` | unsigned integer or opaque binary token | Collection publication identity shared by relation and its index rows |
| `collected_at` | unsigned integer | Collection timestamp in a documented clock unit |
| `catalog_version` | unsigned integer | Catalog generation observed by collection |
| `sampled_rows` | unsigned integer | Number of rows actually inspected |
| `sampled_bytes` | unsigned integer | Approximate bytes inspected under the collection budget |
| `confidence` | finite number in `[0, 1]` | Normalized confidence; precise calibration remains open |
| `payload` | MsgPack map | Versioned relation- or index-specific summary |

These are logical field names, not an approved Tarantool tuple format. Before
implementation, choose fixed positional tuple fields versus a map field,
required-field behavior, numeric encoding/precision, nullability, and whether
the envelope itself is encoded as MsgPack or represented as ordinary tuple
columns. Persistent writers must reject NaN/infinity and out-of-range values;
readers must treat malformed rows as unavailable stats and report a
diagnostic without failing ordinary query preparation.

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
cardinality-semantics definition. Whether the vector belongs in a compact
binary payload or ordinary tuple columns is open.

The relation and index payloads intentionally exclude column MCVs, NDV
sketches, histograms, multi-column groups, and dependency statistics. Those
belong to S2 and require their own space/schema review; this draft does not
pre-approve `_sql_stats_column` or `_sql_stats_multicolumn` IDs or layouts.

## Publication, consistency, and stale data

The implementation must make a collection generation atomic from a reader's
perspective. A prepare-time loader must see either the previously committed
generation or the newly committed relation plus all corresponding index
rows. It must never combine rows across generations. The collection job
should write new rows and publish the generation in one transaction, then
remove superseded rows only after the new generation is visible. If this
cannot fit the transaction limits, an explicit catalog/manifest row and a
two-phase publication protocol need design review before implementation.

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

## Decisions requiring human sign-off

1. **Space IDs and registration:** choose IDs only after confirming all
   currently reserved/assigned IDs and documenting fresh-bootstrap,
   snapshot, WAL, replication, and upgrade behavior. No ID is proposed here.
2. **Tuple representation:** positional tuple fields or a MsgPack map payload;
   decide indexing/format validation and user-visible introspection policy.
3. **Generation model:** generation token type, relation/index atomicity,
   cleanup, and transaction-size strategy.
4. **Cardinality semantics:** define what memtx and Vinyl counts mean under
   deletes, MVCC visibility, compaction, and sampling; choose the canonical
   `row_count` meaning and conversion rules.
5. **Index summaries:** confirm prefix NDV semantics, empty-index encoding,
   unique-index exactness, and whether physical bytes/average key width are
   required in S1.
6. **Staleness:** define catalog/modification epoch sources, thresholds,
   invalidation behavior, and compatibility on DDL or ID reuse.
7. **Clock and confidence:** define timestamp unit/source and calibration of
   confidence before values are persisted.
8. **Version policy:** decide supported-reader behavior, version migration,
   downgrade behavior, and whether format version belongs in every row or
   in a space-level schema marker.

## Relationship to other contracts

This is a proposed persistence contract only. The planner consumes a
separately versioned immutable `SqlStatsSnapshot`; collection uses the
engine-specific sampling APIs; `ANALYZE` publishes stats; and S2 adds column
statistics only after a separate schema review. See
`statistics_implementation_plan.md` for the wider collection and algorithm
plan and `roadmap.md` for task ownership and gates.
