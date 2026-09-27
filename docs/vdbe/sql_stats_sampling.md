# SQL statistics sampling engine contract

This document specifies the in-memory engine boundary for S1 sampling. It
does not define a persistent catalog, system-space IDs, or a serialized
statistics format; those remain subject to the separate human approval gate.

## Proposed boundary

```c
struct sql_stats_sample_request {
	uint32_t index_id;                /* 0 is the primary index */
	uint64_t max_rows;
	uint64_t max_bytes;
	uint64_t seed;
	uint64_t max_buffer_bytes;       /* Vinyl reservoir allocation cap */
	uint64_t max_tuples_examined;    /* Vinyl visible tuple cap */
	uint64_t max_disk_sources;       /* Vinyl first-probe cap */
	uint64_t max_page_reads;         /* Vinyl uncached-page cap */
	uint64_t max_iterator_keys;      /* Vinyl key-advance cap */
	const uint32_t *field_ids;
	size_t field_count;
};

struct sql_stats_sample_sink {
	void *context;
	int (*consume)(void *context, const char *tuple, size_t tuple_size,
		       const uint32_t *field_ids, size_t field_count);
};

struct sql_stats_sample_result {
	uint64_t rows;
	uint64_t bytes;
	bool population_known;
	uint64_t visible_population;
	bool with_replacement;
};

int
engine_sql_stats_sample(struct space *space,
			const struct sql_stats_sample_request *request,
			struct sql_stats_sample_sink *sink,
			struct sql_stats_sample_result *result);
```

`tuple` is a borrowed MessagePack tuple valid only for the duration of the
callback. The sink must copy anything it retains. `field_ids` selects the
fields the common SQL layer should decode; engines return the raw tuple so no
MsgPack parsing policy leaks into the storage layer. A zero field count is a
valid cardinality-only request.

The request's `index_id` selects the SQL index whose visible population is
sampled; index id zero denotes the primary index. Both adapters pass complete
relation tuples to the sink. Vinyl secondary-index entries can be key-only, so
the sampler resolves each selected entry through the primary index using the
same read view before retaining it. Those lookups consume the same key, source,
and page budgets as the secondary scan.

For memtx, `max_rows` and `max_bytes` cap delivered draws and tuple payload
bytes. A draw that would exceed the remaining byte budget is not delivered.
The result counts repeated draws and reports `with_replacement=true`. Memtx
also reports `population_known=true` and the requested index size as the
transaction-visible population; its `size()` implementation subtracts tuples
invisible to the active transaction. This is the count observed during the
synchronous sample operation, not a separately pinned read view. For Vinyl,
the same fields cap retained sample rows
and retained tuple payload bytes, while `max_buffer_bytes` separately caps all
reservoir-owned memory, including metadata, slot array, and tuple copies.
Both Vinyl bounds are checked before retaining a tuple. Zero row, payload,
buffer, or tuple-examination limits and missing required pointers are invalid;
zero source/page/key work limits are valid and immediately reject the
corresponding work. Engines without a sampler report unsupported. A nonzero
callback return aborts delivery and propagates failure; callbacks that fail
should leave a diagnostic for the caller. Engines release all resources on
every exit path.

The result reports delivered callback count and tuple payload bytes. The seed
makes draws reproducible for an unchanged relation and request. The memtx
prototype uses requested-index random access with replacement inside the caller's
active transaction, includes that transaction's uncommitted writes, and holds
no tuple copies; callbacks consume each borrowed tuple synchronously. A byte
limit can end memtx sampling successfully with a partial sample, so consumers
must use returned counts and confidence.

Vinyl performs an exhaustive `ITER_ALL` scan of the requested index in key
order. In an active transaction the iterator uses that transaction's `vy_tx`
and read view, so it sees its own writes and preserves snapshot visibility;
as with any normal Vinyl iterator, it adds read tracking to that transaction.
This can affect later conflict outcomes and incurs read-set work proportional
to the scan, so callers must account for that transaction side effect. Outside
a transaction the sampler creates a short-lived autocommit `vy_tx` and uses
its pinned view. A stable SplitMix64-based Algorithm R reservoir selects up to
`max_rows` tuples without replacement; the seed reproduces the selected
sequence for an unchanged visible population. The result reports the complete
`visible_population`, `population_known=true`, and `with_replacement=false`
only after successful EOF.

Vinyl defers every sink callback until the scan has reached successful EOF.
Any tuple, buffer, source, page, iterator-key, visible-tuple, allocation, or
iterator error fails the whole operation and invokes zero callbacks. Reaching
`max_tuples_examined` before observing EOF is an error, even if the last
returned tuple might have been the relation's final row; callers need headroom
for the terminal EOF probe. The independent `max_iterator_keys` counter is
charged at each read-iterator key-advance loop, including invisible tombstone
keys and the terminal EOF probe. `max_disk_sources` counts first probes of
individual disk sources, and `max_page_reads` counts uncached disk-page read
attempts. Together these cap tuple/key progression, first-source amplification,
and uncached I/O. They do not cap resident page-cache hits or the number of
versions merged within one key/source; those remain covered only by the
iterator's surrounding key/source/page behavior, not by a separate history-
statement counter.

Because Vinyl must discover EOF before publishing an exhaustive sample, it
does not return a successful partial sample. A successful sample whose
population is smaller than `max_rows` returns the whole population. Taking
the first N tuples is not sampling: primary-key order is correlated with
common data distributions.

## Current prototype boundary

The in-memory bounded-loop unit tests exercise request validation, exact row
and byte limits, deterministic seed behavior, repeated draw accounting, and
sink abort. The memtx adapter uses the engine's transaction-aware random index
operation; a runtime engine-dispatch test verifies active-transaction
requirements, visibility of uncommitted tuples, deterministic draws, hard row
and byte caps, sink/result accounting, and unsupported/invalid inputs. It does
not create an independent read view. Neither engine callback is wired to
ANALYZE or planner preparation. Vinyl uses the separate bounded strategy
described below.

## Vinyl feasibility status (S1.6)

The Vinyl callback now has a bounded exhaustive-scan prototype. It uses the
requested index's read iterator and a no-replacement reservoir, but is
not wired to ANALYZE or a statistics collection job. Its bounded-work design
is:

* `vinyl_index_vtab` in `src/box/vinyl.c` installs
  `generic_index_random()` for `.random`; `src/box/index.cc` implements that
  helper by returning `UnsupportedIndexFeature`. The public random-index API
  therefore is not a Vinyl sampling facility.
* `max_disk_sources`, `max_page_reads`, `max_iterator_keys`, and
  `max_tuples_examined` are caller supplied per-operation caps. Work-budget
  exhaustion is sticky and returns an error; no buffered rows are delivered.
  Secondary-to-primary point lookups share the source/page/key budget.
* The sampler allocates its maximum slot array only if it fits under
  `max_buffer_bytes`; tuple copies must fit both `max_bytes` and the remaining
  total buffer budget. A copy/allocation failure discards the reservoir.
* The iterator uses the caller's `vy_tx`/read view when active (including its
  write set); outside a transaction it creates and destroys an autocommit
  `vy_tx`. The LSM is referenced for iterator lifetime. In an active
  transaction, ordinary Vinyl read tracking applies and may affect later
  conflict outcomes; this is part of the prototype API contract.

The sampler does not use `.random`, stop at `max_rows`, or sample raw runs.
It traverses the logical visible population to EOF; source/page/key/tuple caps
make this strategy fail closed when exhaustive work is too large. This is
bounded work, not bounded work that always succeeds: users may need a larger
budget or receive no sample.

The ordinary iterator cannot safely return a partial sample after a
source/page/key cap: an unvisited source may hold a newer version or tombstone,
and key-order prefixes are biased. Therefore callbacks happen only after
successful EOF. Runtime coverage verifies zero delivery for transaction,
source, page, iterator-key, visible-tuple, payload-byte, and buffer-budget
rejections; active-transaction coverage verifies that the transaction's own
uncommitted tuple is visible to the exhaustive scan. Other cases cover
deterministic no-replacement sampling and visibility after
update/delete/compaction. A fixed-fixture seed sweep is a quality smoke test,
not a statistical proof. The test module and C/unit tests compile, and the
standalone Vinyl runtime guard passes against the configured CnP-enabled
server build. Cancellation is propagated through the iterator's existing error
path; synchronous recovery-time reads are rejected while a
sampling work budget is attached. The sampler remains a prototype: no
ANALYZE/collection integration, confidence calibration, cross-engine shared
snapshot, or workload-level latency/read-amplification evaluation exists.

## Collection contract still open (S1.3)

The sampler is not itself a collection job. A collector must define which
population its summaries describe and keep counts and sampled tuples on the
same visibility basis. Memtx index `size()` subtracts tuples invisible to the
active transaction, while the sampler draws through that transaction; this is
a possible exact population input, and the current sampler reports it; the
collector must still handle empty indexes, errors, and a concurrent/schema-
generation boundary. Vinyl
`index_size()` is explicitly an approximate count of LSM statements and may
include obsolete versions or tombstones, so it is not a visible-row count.
Vinyl's visible population is known only after a successful exhaustive scan;
bounded reservoir exhaustion is therefore a collection failure, not a partial
publication.

Before implementing ANALYZE, define a non-persistent candidate-snapshot
builder that consumes one collection result and validates row count, average
width, confidence, index tuple populations, and prefix NDVs as a complete
unit. It must build off to the side and publish only after all relation/index
summaries are valid; a failure must leave the currently installed snapshot
unchanged. This contract does not choose system-space IDs, tuple layouts, or
payload formats, which remain in the separate DRAFT schema review.

### S1.3a volatile candidate builder (2026-09)

The new `sql_stats_collection_result` is a volatile, normalized result and
`sql_stats_collection_build_candidate()` is a pure completeness gate. The
caller supplies the expected relation/index definitions and expected
catalog/schema/visibility generation. The builder rejects missing or extra
relations and indexes, missing leading-prefix NDVs, definition-version,
modification-epoch, catalog/schema/visibility mismatches, zero/unknown
visibility or index-definition tokens, and an index whose NDV basis differs
from its tuple-population basis. Duplicate relation or index IDs are rejected
before lookup, so duplicate result entries cannot conceal a missing expected
definition. It builds a detached candidate through the
snapshot constructor; it does not install or globally publish it.

The result carries per-relation row-count semantics, opaque caller-defined
population and width-basis tokens, an explicit width denominator count, and a
confidence-source token. Each index
entry carries tuple-population semantics/basis, an NDV-basis token, and the
captured index-definition version. These token values are copied verbatim
into the immutable in-memory snapshot (including the visibility token and
width denominator count); their
meaning, estimation, and confidence calibration remain producer-owned. The
builder chooses no width denominator, sampler, confidence calibration, or
persistence encoding. It requires each index's tuple count and NDV vector to
declare the same population basis, while retaining relation and per-index
population tags separately; it does not assume a sparse index has the same
population as its relation. A common visibility token and schema/catalog
generation are validated, and the width denominator count must be present and
nonzero. The engine mechanism that establishes that boundary remains
undefined.

`sql_stats_collection_population_from_sample()` is a narrow producer bridge:
when the engine reports a known population, it yields that visible population
as relation cardinality rather than confusing it with delivered draws. It
rejects unknown populations, a nonempty sample from an empty population, and a
no-replacement sample larger than the population. This covers memtx
replacement draws and Vinyl's successful exhaustive scan/reservoir result.
The helper does not invent a visibility token or prove that catalog/schema
capture spans the sampling call; the producer must still establish one common
generation boundary. `sql_stats_collection_width_from_sample()` separately
exposes the fractional sample-average serialized tuple size as a floating-
point value with sampled rows as its denominator. It rejects unknown/
inconsistent populations, empty samples, and impossible byte totals rather
than inventing width for an empty relation.
This is only a width observation: the helper does not decode tuple fields,
derive per-index populations or prefix NDVs, calibrate confidence, or build a
complete candidate.

`sql_stats_index_summary` is a separate sampled-tuple consumer for one index.
Its generic callback path requires typed, SQL-canonical values from the
caller; raw MessagePack encodings are not treated as SQL values. The native
`sql_stats_index_summary_new_for_index()` adapter instead retains a tuple
format and copies the index key definition, reconstructs native Tarantool
tuples from delivered full-row bytes, then hashes each prefix using
`tuple_hash_key_part()` and the key-part type/collation. It initially accepts
only TREE/HASH indexes with STRING, DOUBLE, BOOLEAN, or UNSIGNED parts and
rejects multikey and functional key definitions. BOOLEAN is supported because its
accepted MessagePack domain has exactly two canonical boolean encodings and
the comparator decodes those values before comparing. Native mode reports
`hash_bits=32`: because HLL receives the engine's 32-bit index hash, distinct
SQL keys can collide before sketching,
so estimates can be biased for high cardinalities. It does not claim exact or
canonical NDV. Both modes count and sketch only delivered sample tuples, do
not extrapolate to population, and cannot produce a
`sql_stats_collected_index` accepted as population-matched candidate data.
Memory limits cover HLL registers and sketch-pointer storage but not
producer-owned temporary canonical-value storage, native tuple reconstruction,
or the temporary prefix-hash vector. The caller must supply a format and key
definition from the same captured schema version. This is an aggregation
building block, not a complete S1.3a producer. Native-adapter checks were added
to `key_def.test` for leading prefixes, unsupported type rejection, the
DOUBLE hash normalization of integer/floating encodings, collation equality,
and BOOLEAN distinctness/deduplication. After the BOOLEAN extension, the
configured CMake target rebuilt successfully and all 52 top-level tests passed,
including the 12-check native-adapter subtest.

No common cross-engine visibility mechanism has been established. A viable
collector boundary must atomically capture catalog/schema/index definitions
and a data watermark that every included engine can honor, then pin reads (or
validate monotonic per-space modification generations) against that exact
boundary. The core `read_view` has an assigned ID and filtered index ownership,
but Vinyl index read-view creation currently fails. The transaction sampler
uses Vinyl's transaction read view when present, but `READ_CONFIRMED` does not
freeze confirmed writes between calls. Neither identifier can currently be
combined across indexes or engines into the candidate generation token. A
future box-level collector snapshot API must coordinate memtx visibility and
Vinyl's VLSN read view; before/after schema and index-definition checks are
necessary additional guards, not substitutes for a pinned data-time boundary.
Until then, the producer must not mint a shared visibility ID.

### Publication is a separate, currently blocked slice

Do not implement a global pointer swap as a substitute for this contract.
The builder checks equality of caller-provided tokens, but it cannot prove
that those tokens identify a single read view. In particular, memtx samples
are transaction-visible and Vinyl iterators use a transaction read view (or
an internally-created autocommit transaction); sampling different relations
does not itself pin them to one common view. A producer must use an engine
mechanism that captures/pins a common view or validates a generation boundary
before and after all reads, including catalog, schema, relation modification,
and index-definition generations. If that mechanism cannot guarantee a
consistent view, collection must fail closed rather than mint a token.

Publication additionally needs a collector-owned install transaction with
explicit snapshot retain/release rules for concurrent readers. It must expose
only a complete detached candidate, atomically replace the installed snapshot,
and leave the previous snapshot installed on every allocation, validation, or
generation-check failure. Tests must cover concurrent readers across a swap,
candidate-build failure, stale generation at the install boundary, and
rollback preserving both the old pointer and its lifetime. `sql_set_stats_snapshot()`
already retains/releases an immutable snapshot and expires prepared
statements, but `sql_stats_collection_build_candidate()` returns an
uninstalled snapshot and the collection path does not call the setter or
revalidate its generation at the install boundary. The read-view token source
is also not complete across engines. The existing setter alone therefore does
not establish a safe collection publication transaction and does not enable
`ANALYZE`.

The first reusable runtime slice now exists as
`sql_stats_collection_context`: it owns one filtered core `read_view`, records
that view's engine-assigned ID and the schema version captured around open,
rejects missing requested indexes/schema drift, and can exhaustively scan a
pinned index into the existing bounded reservoir. Unit tests cover context
ownership, fail-closed open cases, exhaustive population reporting, budget
failure, and withholding sink delivery on stale/incomplete scans. This is only
a building block, not a complete collector or publisher. Core
`read_view_open()` does not expose a
memory/work-budget argument and creates engine-wide read-view state, so
filtering bounds the requested space/index views but does not cap the engine's
read-view resource cost. Moreover, Vinyl indexes
currently use `generic_index_create_read_view()`, which rejects consistent
read views; a requested Vinyl index consequently fails context creation.
Until a bounded Vinyl read-view path and a complete stats producer exist,
collection remains disabled. The context does not install snapshots, so the
previously specified publication/rollback tests are still required.

The separate `sql_stats_tx_context` runtime slice can begin an owned box
transaction, set `READ_CONFIRMED` before any read, validate the target indexes
and schema, and call `engine_sql_stats_sample()` for each requested index
under that same transaction ID. A per-index bounded staging buffer prevents
an engine error from forwarding only part of that index's sample. `finish`
commits only when every requested index succeeded; otherwise it rolls back.
`abort` only rolls back if the calling fiber still owns the captured
transaction ID. The context also captures the local commit-vclock signature
as a volatile visibility token and rejects sampling/finish if it changes.
This is a local commit-generation guard, not a durable identity or a
cross-node snapshot ID; callers must still pair it with catalog/schema/index
definition provenance before using it in `sql_stats_collection_generation`.
The focused `sql_stats_collection.test` CMake target builds and passes,
including transaction lifecycle, isolation/schema/visibility drift, staging,
all-target finish checks. This is unit-stub coverage; no live memtx/Vinyl
integration run has validated this context.

This transaction context is not a frozen database snapshot. `READ_CONFIRMED`
excludes prepared/unconfirmed writes, but does not itself freeze confirmed
writes between calls. The local commit-vclock check detects such commits and
fails closed, but is not an MVCC snapshot handle or a durable/cross-node
identity. A successful earlier index can already have delivered to
the caller's sink if a later index fails, so sinks must target disposable
off-side staging and callers must discard it unless all requested work and
candidate validation succeed. The context neither builds nor publishes a
candidate; `READ_CONFIRMED` is not authorization for global publication and
does not enable `ANALYZE`.

The in-memory snapshot API version is now 2 so the new provenance and width
denominator metadata are explicit. Existing designated/zero-initialized
snapshot callers may omit metadata and it stays unset, with no inferred
default; positional initializers must be updated for the versioned input
struct change.

Unit tests cover complete construction and deep-copying, missing relations and
indexes, missing prefixes, mismatched index definition/visibility/generation,
mixed comparable population bases, invalid cardinality semantics and numeric
values for relations and indices, and snapshot allocation-budget rejection.
Unit coverage verifies the engine-population-to-relation-count bridge,
including replacement draws, no-replacement bounds, empty populations, and
unknown/inconsistent results. Allocation failures are injected at snapshot
deep-copy and collection staging points. There is still no publication in
this slice, so publication rollback is not covered. S1.3a remains incomplete
until a producer consumes sampled tuples to populate the remaining
relation/index summaries, a shared candidate-generation contract is defined,
and an independent publication step proves that failed construction preserves
the installed snapshot. Nothing here enables `ANALYZE` or persistent
statistics.
