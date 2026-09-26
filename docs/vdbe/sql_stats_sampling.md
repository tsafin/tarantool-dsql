# SQL statistics sampling engine contract

This document specifies the in-memory engine boundary for S1 sampling. It
does not define a persistent catalog, system-space IDs, or a serialized
statistics format; those remain subject to the separate human approval gate.

## Proposed boundary

```c
struct sql_stats_sample_request {
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

For memtx, `max_rows` and `max_bytes` cap delivered draws and tuple payload
bytes. A draw that would exceed the remaining byte budget is not delivered.
The result counts repeated draws and reports `with_replacement=true`. Memtx
also reports `population_known=true` and the primary-index size as the
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
prototype uses primary-index random access with replacement inside the caller's
active transaction, includes that transaction's uncommitted writes, and holds
no tuple copies; callbacks consume each borrowed tuple synchronously. A byte
limit can end memtx sampling successfully with a partial sample, so consumers
must use returned counts and confidence.

Vinyl performs an exhaustive `ITER_ALL` scan of the primary index in key
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
existing primary-index read iterator and a no-replacement reservoir, but is
not wired to ANALYZE or a statistics collection job. Its bounded-work design
is:

* `vinyl_index_vtab` in `src/box/vinyl.c` installs
  `generic_index_random()` for `.random`; `src/box/index.cc` implements that
  helper by returning `UnsupportedIndexFeature`. The public random-index API
  therefore is not a Vinyl sampling facility.
* `max_disk_sources`, `max_page_reads`, `max_iterator_keys`, and
  `max_tuples_examined` are caller supplied per-operation caps. Work-budget
  exhaustion is sticky and returns an error; no buffered rows are delivered.
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
from its tuple-population basis. It builds a detached candidate through the
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
exposes the integer sample-average serialized tuple size with sampled rows as
its denominator. It rejects unknown/inconsistent populations, empty samples,
and impossible byte totals rather than inventing width for an empty relation.
This is only a width observation: the helper does not decode tuple fields,
derive per-index populations or prefix NDVs, calibrate confidence, or build a
complete candidate.

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
relation/index summaries, a shared visibility/generation mechanism is defined,
and an independent publication step proves that failed construction preserves
the installed snapshot. Nothing here enables `ANALYZE` or persistent
statistics.
