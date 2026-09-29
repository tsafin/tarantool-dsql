#ifndef TARANTOOL_SQL_STATS_SPACESAVING_H
#define TARANTOOL_SQL_STATS_SPACESAVING_H

#include <stddef.h>
#include <stdint.h>

/**
 * Bounded SpaceSaving summary for SQL most-common-value candidates.
 * Keys are caller-encoded byte strings (include SQL type/NULL distinctions).
 * This is an in-memory API; it defines no persistent format or SQL catalog ID.
 */
struct sql_stats_spacesaving;

struct sql_stats_spacesaving_entry {
	uint64_t estimate;
	uint64_t error;
};

/** Allocate a summary with capacity in [1, UINT32_MAX], or return NULL. */
struct sql_stats_spacesaving *
sql_stats_spacesaving_new(uint32_t capacity);

/** Free a summary; NULL is accepted. */
void
sql_stats_spacesaving_delete(struct sql_stats_spacesaving *summary);

/** Add one key. data may be NULL only when size is zero. */
int
sql_stats_spacesaving_add(struct sql_stats_spacesaving *summary,
			  const void *data, size_t size);

/**
 * Merge source into destination. It is safe for dst == src. Returns -1 on
 * invalid arguments or allocation failure; destination is unchanged on error.
 */
int
sql_stats_spacesaving_merge(struct sql_stats_spacesaving *dst,
			    const struct sql_stats_spacesaving *src);

/**
 * Look up a tracked key. Returns 1 if present, 0 if absent, -1 on invalid
 * arguments. For a present key, true frequency is in
 * [entry->estimate - entry->error, entry->estimate].
 */
int
sql_stats_spacesaving_query(const struct sql_stats_spacesaving *summary,
			    const void *data, size_t size,
			    struct sql_stats_spacesaving_entry *entry);

uint32_t
sql_stats_spacesaving_capacity(const struct sql_stats_spacesaving *summary);

/* Number of currently retained candidates. */
uint32_t
sql_stats_spacesaving_count(const struct sql_stats_spacesaving *summary);

/* Borrow a retained key by slot; key storage remains summary-owned. */
int
sql_stats_spacesaving_at(const struct sql_stats_spacesaving *summary,
			 uint32_t slot, const void **data, size_t *size,
			 struct sql_stats_spacesaving_entry *entry);

/* Worst-case owned bytes for capacity entries with keys up to max_key_size. */
int
sql_stats_spacesaving_storage_bytes(uint32_t capacity, size_t max_key_size,
				    size_t *bytes);

#endif /* TARANTOOL_SQL_STATS_SPACESAVING_H */
