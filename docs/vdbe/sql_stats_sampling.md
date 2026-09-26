# SQL statistics sampling engine contract

This document specifies the in-memory engine boundary for S1 sampling. It
does not define a persistent catalog, system-space IDs, or a serialized
statistics format; those remain subject to the separate human approval gate.

## Proposed boundary

```c
struct SqlStatsSampleRequest {
	uint64_t max_rows;
	uint64_t max_bytes;
	uint64_t seed;
	const uint32_t *field_ids;
	size_t field_count;
};

struct SqlStatsSampleSink {
	void *context;
	int (*consume)(void *context, const char *tuple, size_t tuple_size,
		       const uint32_t *field_ids, size_t field_count);
};

int
engine_sql_stats_sample(struct space *space,
			const struct SqlStatsSampleRequest *request,
			struct SqlStatsSampleSink *sink);
```

`tuple` is a borrowed MessagePack tuple valid only for the duration of the
callback. The sink must copy anything it retains. `field_ids` selects the
fields the common SQL layer should decode; engines return the raw tuple so no
MsgPack parsing policy leaks into the storage layer. A zero field count is a
valid cardinality-only request.

Both limits are hard upper bounds: no more than `max_rows` callbacks and no
more than `max_bytes` total tuple bytes may be delivered. A tuple that would
exceed the remaining byte budget is not delivered. `max_rows == 0`,
`max_bytes == 0`, missing required pointers, or an absent primary index is an
invalid request. A nonzero callback return aborts collection and propagates
failure; the engine must release its iterator on every exit path.

The seed makes a sample reproducible for an unchanged relation and request.
Sampling must not be implemented as “take the first N tuples”: primary-key
order is correlated with common data distributions. The memtx implementation
should use a bounded reservoir over visible tuples, retain only selected
tuple copies (or equivalent stable references), and account for retained tuple
bytes before invoking the sink. If the byte limit prevents a full reservoir,
the returned sample is partial and callers must lower confidence; collection
must not block ordinary query preparation.

## Current prototype boundary

The preceding interface and limits are the proposed S1.5 contract. The code
currently has the SQL summary primitives and immutable snapshot API, but no
memtx or Vinyl implementation of this contract. In particular, this document
does not claim a read-view guarantee for a multi-pass sampler, transactional
ANALYZE, or production wiring. Those require engine-specific implementation
and tests before S1.5 can be marked complete. Vinyl must independently choose
a bounded strategy that avoids pathological full-LSM reads.

## Vinyl feasibility status (S1.6)

S1.6 is **not implemented**. The current engine APIs do not provide a safe
bounded Vinyl sampling primitive:

* `vinyl_index_vtab` in `src/box/vinyl.c` installs
  `generic_index_random()` for `.random`; `src/box/index.cc` implements that
  helper by returning `UnsupportedIndexFeature`. The public random-index API
  therefore is not a Vinyl sampling facility.
* The ordinary Vinyl read iterator (`vy_read_iterator_open()` /
  `vy_read_iterator_next()`) walks key order while merging transaction,
  cache, mem and disk sources. Stopping after `max_rows` bounds delivered
  tuples, not the cost to locate them or the number of LSM sources/pages
  involved. Taking the first N rows is also order-biased.
* The current request has row and delivered-byte limits, but no explicit work
  budget or partial/unsupported result semantics. Those limits alone cannot
  substantiate the required claim that statistics collection will not trigger
  pathological full-LSM work.

Do not implement Vinyl sampling by calling `.random`, by scanning the primary
index and stopping at `max_rows`, or by independently sampling raw runs: the
last option would need to resolve duplicate versions, deletes, and visibility
consistently with Vinyl's read view. A viable follow-up needs an engine-owned
design for bounded range/run-aware candidate selection, an explicit measure
of bounded work (including I/O/source amplification), and a defined partial
sample/confidence result. It must test both read amplification and sample
quality under multiple ranges, compaction states, updates, and deletes.

Dependencies: S1.5 must first settle and wire the generic request/sink/result
contract; then S1.6 can extend that contract with Vinyl-specific work bounds
and semantics before adding an engine entrypoint. Until then S1.6 remains
open; this status is a feasibility finding, not a completed sampler.
