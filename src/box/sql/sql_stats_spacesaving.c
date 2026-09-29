#include "sql_stats_spacesaving.h"

#include <stdlib.h>
#include <string.h>

struct ss_entry {
	unsigned char *key;
	size_t size;
	uint64_t count;
	uint64_t error;
};

struct sql_stats_spacesaving {
	uint32_t capacity;
	uint32_t count;
	struct ss_entry entries[];
};

static int
key_cmp(const void *a, size_t a_size, const void *b, size_t b_size)
{
	size_t common = a_size < b_size ? a_size : b_size;
	int cmp = common == 0 ? 0 : memcmp(a, b, common);
	if (cmp != 0)
		return cmp;
	return (a_size > b_size) - (a_size < b_size);
}

static int
find_key(const struct sql_stats_spacesaving *s, const void *key, size_t size)
{
	for (uint32_t i = 0; i < s->count; i++) {
		if (s->entries[i].size == size &&
		    key_cmp(s->entries[i].key, size, key, size) == 0)
			return (int)i;
	}
	return -1;
}

/* Lowest count is the victim; ties evict the lexicographically first key. */
static uint32_t
victim_index(const struct sql_stats_spacesaving *s)
{
	uint32_t victim = 0;
	for (uint32_t i = 1; i < s->count; i++) {
		if (s->entries[i].count < s->entries[victim].count ||
		    (s->entries[i].count == s->entries[victim].count &&
		     key_cmp(s->entries[i].key, s->entries[i].size,
			     s->entries[victim].key,
			     s->entries[victim].size) < 0))
			victim = i;
	}
	return victim;
}

struct sql_stats_spacesaving *
sql_stats_spacesaving_new(uint32_t capacity)
{
	if (capacity == 0)
		return NULL;
	size_t entries_size = (size_t)capacity * sizeof(struct ss_entry);
	if (entries_size / sizeof(struct ss_entry) != capacity ||
	    entries_size > SIZE_MAX - sizeof(struct sql_stats_spacesaving))
		return NULL;
	size_t size = sizeof(struct sql_stats_spacesaving) + entries_size;
	struct sql_stats_spacesaving *s = calloc(1, size);
	if (s != NULL)
		s->capacity = capacity;
	return s;
}

void
sql_stats_spacesaving_delete(struct sql_stats_spacesaving *s)
{
	if (s == NULL)
		return;
	for (uint32_t i = 0; i < s->count; i++)
		free(s->entries[i].key);
	free(s);
}

static int
add_weight(struct sql_stats_spacesaving *s, const void *key, size_t size,
	   uint64_t weight, uint64_t inherited_error)
{
	int found = find_key(s, key, size);
	if (found >= 0) {
		struct ss_entry *e = &s->entries[found];
		if (UINT64_MAX - e->count < weight ||
		    UINT64_MAX - e->error < inherited_error)
			return -1;
		e->count += weight;
		e->error += inherited_error;
		return 0;
	}
	unsigned char *copy = malloc(size == 0 ? 1 : size);
	if (copy == NULL)
		return -1;
	if (size != 0)
		memcpy(copy, key, size);
	if (s->count < s->capacity) {
		s->entries[s->count++] = (struct ss_entry){copy, size, weight,
							 inherited_error};
		return 0;
	}
	uint32_t i = victim_index(s);
	struct ss_entry *e = &s->entries[i];
	if (UINT64_MAX - e->count < weight ||
	    UINT64_MAX - e->error < e->count ||
	    UINT64_MAX - e->error - e->count < inherited_error) {
		free(copy);
		return -1;
	}
	uint64_t floor = e->count;
	free(e->key);
	*e = (struct ss_entry){copy, size, floor + weight,
			       floor + e->error + inherited_error};
	return 0;
}

int
sql_stats_spacesaving_add(struct sql_stats_spacesaving *s, const void *data,
			  size_t size)
{
	if (s == NULL || (data == NULL && size != 0))
		return -1;
	return add_weight(s, data, size, 1, 0);
}

int
sql_stats_spacesaving_merge(struct sql_stats_spacesaving *dst,
			    const struct sql_stats_spacesaving *src)
{
	if (dst == NULL || src == NULL)
		return -1;
	if (dst == src)
		return 0;
	uint64_t dst_floor = dst->count == dst->capacity ?
		dst->entries[victim_index(dst)].count : 0;
	uint64_t src_floor = src->count == src->capacity ?
		src->entries[victim_index(src)].count : 0;
	if (dst->count + src->count > UINT32_MAX)
		return -1;
	uint32_t max_count = dst->count + src->count;
	struct ss_entry *candidates = calloc(max_count == 0 ? 1 : max_count,
					     sizeof(*candidates));
	if (candidates == NULL)
		return -1;
	uint32_t candidate_count = 0;
	for (uint32_t side = 0; side < 2; side++) {
		const struct sql_stats_spacesaving *from = side == 0 ? dst : src;
		const struct sql_stats_spacesaving *other = side == 0 ? src : dst;
		uint64_t other_floor = side == 0 ? src_floor : dst_floor;
		for (uint32_t i = 0; i < from->count; i++) {
			const struct ss_entry *e = &from->entries[i];
			if (find_key(other, e->key, e->size) >= 0)
				continue;
			if (UINT64_MAX - e->count < other_floor ||
			    UINT64_MAX - e->error < other_floor)
				goto merge_error;
			candidates[candidate_count++] = (struct ss_entry){
				.key = e->key, .size = e->size,
				.count = e->count + other_floor,
				.error = e->error + other_floor,
			};
		}
	}
	for (uint32_t i = 0; i < dst->count; i++) {
		const struct ss_entry *a = &dst->entries[i];
		int j = find_key(src, a->key, a->size);
		if (j < 0)
			continue;
		const struct ss_entry *b = &src->entries[j];
		if (UINT64_MAX - a->count < b->count ||
		    UINT64_MAX - a->error < b->error)
			goto merge_error;
		candidates[candidate_count++] = (struct ss_entry){
			.key = a->key, .size = a->size,
			.count = a->count + b->count,
			.error = a->error + b->error,
		};
	}
	struct sql_stats_spacesaving *merged =
		sql_stats_spacesaving_new(dst->capacity);
	if (merged == NULL)
		goto merge_error;
	/* Keep highest upper estimates; lexical order resolves every tie. */
	for (uint32_t n = 0; n < dst->capacity && n < candidate_count; n++) {
		uint32_t best = n;
		for (uint32_t i = n + 1; i < candidate_count; i++) {
			if (candidates[i].count > candidates[best].count ||
			    (candidates[i].count == candidates[best].count &&
			     key_cmp(candidates[i].key, candidates[i].size,
				     candidates[best].key,
				     candidates[best].size) < 0))
				best = i;
		}
		struct ss_entry tmp = candidates[n];
		candidates[n] = candidates[best];
		candidates[best] = tmp;
		struct ss_entry *e = &candidates[n];
		unsigned char *key = malloc(e->size == 0 ? 1 : e->size);
		if (key == NULL) {
			sql_stats_spacesaving_delete(merged);
			goto merge_error;
		}
		if (e->size != 0)
			memcpy(key, e->key, e->size);
		merged->entries[merged->count++] = (struct ss_entry){
			.key = key, .size = e->size, .count = e->count,
			.error = e->error,
		};
	}
	/* Swap only after the full merge succeeded. */
	for (uint32_t i = 0; i < dst->count; i++)
		free(dst->entries[i].key);
	dst->count = merged->count;
	for (uint32_t i = 0; i < merged->count; i++) {
		dst->entries[i] = merged->entries[i];
		merged->entries[i].key = NULL;
	}
	sql_stats_spacesaving_delete(merged);
	free(candidates);
	return 0;

merge_error:
	free(candidates);
	return -1;
}

int
sql_stats_spacesaving_query(const struct sql_stats_spacesaving *s,
			    const void *data, size_t size,
			    struct sql_stats_spacesaving_entry *entry)
{
	if (s == NULL || entry == NULL || (data == NULL && size != 0))
		return -1;
	int i = find_key(s, data, size);
	if (i < 0)
		return 0;
	entry->estimate = s->entries[i].count;
	entry->error = s->entries[i].error;
	return 1;
}

uint32_t
sql_stats_spacesaving_capacity(const struct sql_stats_spacesaving *s)
{
	return s == NULL ? 0 : s->capacity;
}

uint32_t
sql_stats_spacesaving_count(const struct sql_stats_spacesaving *s)
{
	return s == NULL ? 0 : s->count;
}

int
sql_stats_spacesaving_at(const struct sql_stats_spacesaving *s, uint32_t slot,
			 const void **data, size_t *size,
			 struct sql_stats_spacesaving_entry *entry)
{
	if (s == NULL || data == NULL || size == NULL || entry == NULL ||
	    slot >= s->count)
		return -1;
	const struct ss_entry *e = &s->entries[slot];
	*data = e->key;
	*size = e->size;
	*entry = (struct sql_stats_spacesaving_entry) {
		.estimate = e->count,
		.error = e->error,
	};
	return 0;
}

int
sql_stats_spacesaving_storage_bytes(uint32_t capacity, size_t max_key_size,
				    size_t *bytes)
{
	if (capacity == 0 || bytes == NULL)
		return -1;
	size_t key_bytes = max_key_size == 0 ? 1 : max_key_size;
	if (capacity > SIZE_MAX / sizeof(struct ss_entry))
		return -1;
	size_t entries = (size_t)capacity * sizeof(struct ss_entry);
	if (entries > SIZE_MAX - sizeof(struct sql_stats_spacesaving))
		return -1;
	size_t base = sizeof(struct sql_stats_spacesaving) + entries;
	if (capacity > (SIZE_MAX - base) / key_bytes)
		return -1;
	*bytes = base + (size_t)capacity * key_bytes;
	return 0;
}
