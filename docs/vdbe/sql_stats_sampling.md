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
