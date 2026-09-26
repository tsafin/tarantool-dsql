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

Both limits are hard upper bounds: no more than `max_rows` callbacks and no
more than `max_bytes` total tuple bytes may be delivered. A tuple that would
exceed the remaining byte budget is not delivered. `max_rows == 0`,
`max_bytes == 0`, or missing required pointers is an invalid request. An
absent primary index and engines without a sampler report unsupported. A
nonzero callback return aborts collection and propagates failure; callbacks
that fail should leave a diagnostic for the caller. Engines must release any
resources on every exit path.

The result reports delivered callback count and tuple bytes. Repeated tuples
are separate draws and are counted separately. The seed makes the sequence of
draw inputs reproducible for an unchanged relation and request. The current
memtx prototype uses the primary index's random-access API repeatedly, with
replacement, in the caller's active transaction (an active transaction is
required). It holds no tuple copies;
each borrowed tuple is consumed synchronously. If the next tuple would exceed
the remaining byte budget, collection stops successfully with a partial
sample, so callers must use the returned counts and lower confidence as
appropriate. Sampling must not be implemented as “take the first N tuples”:
primary-key order is correlated with common data distributions.

## Current prototype boundary

The in-memory bounded-loop unit tests exercise request validation, exact row
and byte limits, deterministic seed behavior, repeated draw accounting, and
sink abort. The memtx adapter uses the engine's transaction-aware random index
operation; a runtime engine-dispatch test verifies active-transaction
requirements, visibility of uncommitted tuples, deterministic draws, hard row
and byte caps, sink/result accounting, and unsupported/invalid inputs. It does
not create an independent read view. The API is not wired to ANALYZE or
planner preparation. Vinyl must independently choose a bounded strategy that
avoids pathological full-LSM reads.

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

The generic S1.5 request/sink/result contract and memtx callback now exist.
The runtime dispatch test also verifies that Vinyl's currently missing
callback fails closed with `ER_UNSUPPORTED` without delivering rows. S1.6 can
extend the contract with Vinyl-specific work bounds and partial or unsupported
result semantics. Until that strategy is implemented and tested, S1.6 remains
open; this status is a feasibility finding, not a completed sampler.
