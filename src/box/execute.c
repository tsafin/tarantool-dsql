/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "execute.h"

#include "assoc.h"
#include "bind.h"
#include "iproto_constants.h"
#include "sql/sqlInt.h"
#include "sql/sqlLimit.h"
#include "errcode.h"
#include "small/region.h"
#include "diag.h"
#include "sql.h"
#include "xrow.h"
#include "schema.h"
#include "port.h"
#include "tuple.h"
#include "sql/vdbe.h"
#include "sql/vdbeInt.h"
#include "box/lua/execute.h"
#include "box/sql_stmt_cache.h"
#include "session.h"
#include "rmean.h"
#include "box/sql/port.h"
#include "tweaks.h"
#include "box/schema.h"
#include <string.h>

/*
 * Automatic SQL statement cache for sql_prepare_and_execute.
 *
 * A direct-mapped cache (open addressing, power-of-2 capacity) that stores
 * compiled Vdbe* instances keyed by (sql_hash, sql_flags).  On a cache hit
 * the full parse + codegen + JIT compile cycle is bypassed; only execution
 * runs.  This makes box.execute("SELECT ...") as fast as box.prepare() +
 * stmt:execute() on the second and subsequent calls for the same SQL text.
 *
 * Design choices:
 *  - Direct-mapped: no per-entry allocation, O(1) lookup/eviction.
 *  - 256 slots: covers typical workloads without consuming significant memory.
 *  - Keys include sql_flags (session compilation flags) so that stmts compiled
 *    under different flag sets are not mixed up.
 *  - Staleness is detected by comparing stmt->schema_ver with the live
 *    box_schema_version() at lookup time.  sqlVdbeReset clears stmt->expired
 *    but preserves schema_ver, so the version comparison is the authoritative
 *    staleness check.
 *  - SQL text is verified on hit (strlen + memcmp) to handle 32-bit hash
 *    collisions.
 */
#define AUTO_CACHE_CAPACITY 256
#define AUTO_CACHE_MASK     (AUTO_CACHE_CAPACITY - 1)

struct auto_cache_entry {
	uint32_t sql_hash;
	uint32_t sql_flags;
	struct Vdbe *stmt;
};

static struct auto_cache_entry auto_stmt_cache[AUTO_CACHE_CAPACITY];

/*
 * Compute the slot index for (sql_hash, sql_flags).
 * Mix the two 32-bit values so that distinct sql_flags map to different slots
 * for the same SQL text.
 */
static inline uint32_t
auto_cache_slot(uint32_t sql_hash, uint32_t sql_flags)
{
	return (sql_hash ^ (sql_flags * 2654435761u)) & AUTO_CACHE_MASK;
}

/*
 * Evict the entry at slot, releasing the Vdbe.  No-op if slot is empty.
 */
static void
auto_cache_evict_slot(uint32_t slot)
{
	struct auto_cache_entry *e = &auto_stmt_cache[slot];
	if (e->stmt != NULL) {
		sql_stmt_finalize(e->stmt);
		e->stmt = NULL;
	}
}

/*
 * Look up a cached stmt for (sql_hash, sql_flags, sql text).
 *
 * Returns a valid, idle Vdbe* on hit, or NULL on miss/stale.  A stale entry
 * (schema changed or SQL text mismatch) is evicted before returning NULL.
 */
static struct Vdbe *
auto_cache_lookup(uint32_t sql_hash, uint32_t sql_flags,
		  const char *sql, size_t sql_len)
{
	uint32_t slot = auto_cache_slot(sql_hash, sql_flags);
	struct auto_cache_entry *e = &auto_stmt_cache[slot];
	if (e->stmt == NULL)
		return NULL;

	if (e->sql_hash != sql_hash || e->sql_flags != sql_flags)
		return NULL;

	/* Verify SQL text to rule out 32-bit hash collisions. */
	const char *cached_sql = sql_stmt_query_str(e->stmt);
	if (cached_sql == NULL)
		goto evict;
	size_t cached_len = strlen(cached_sql);
	if (cached_len != sql_len || memcmp(cached_sql, sql, sql_len) != 0)
		goto evict;

	/* Reject stale stmts (schema changed since compile). */
	if (sql_stmt_schema_version(e->stmt) != box_schema_version())
		goto evict;

	/* Reject stmts that are mid-execution (should not happen in practice). */
	if (sql_stmt_busy(e->stmt))
		return NULL;

	return e->stmt;
evict:
	auto_cache_evict_slot(slot);
	return NULL;
}

/*
 * Insert stmt into the auto cache, evicting any existing occupant of the slot.
 * The cache takes ownership; the caller must NOT finalize stmt afterwards.
 */
static void
auto_cache_insert(uint32_t sql_hash, uint32_t sql_flags, struct Vdbe *stmt)
{
	uint32_t slot = auto_cache_slot(sql_hash, sql_flags);
	struct auto_cache_entry *e = &auto_stmt_cache[slot];
	/* Evict whatever is currently in this slot (if anything). */
	if (e->stmt != NULL && e->stmt != stmt)
		sql_stmt_finalize(e->stmt);
	e->sql_hash = sql_hash;
	e->sql_flags = sql_flags;
	e->stmt = stmt;
}

const char *sql_info_key_strs[] = {
	"row_count",
	"autoincrement_ids",
};

/** Whether to enable access checks for SQL requests. */
static bool sql_access_check_is_enabled = true;
TWEAK_BOOL(sql_access_check_is_enabled);

/** Checks if the current user may execute an SQL request. */
static int
access_check_sql(void)
{
	if (!sql_access_check_is_enabled)
		return 0;
	struct credentials *cr = effective_user();
	user_access_t access = PRIV_X | PRIV_U;
	access &= ~cr->universal_access;
	if (access == 0)
		return 0;
	access &= ~universe.access_sql[cr->auth_token].effective;
	if (access == 0)
		return 0;
	struct user *user = user_find(cr->uid);
	if (user != NULL)
		diag_set(AccessDeniedError, priv_name(PRIV_X),
			 schema_object_name(SC_SQL), "", user->def->name);
	return -1;
}

/**
 * Convert sql row into a tuple and append to a port.
 * @param stmt Started prepared statement. At least one
 *        sql_step must be done.
 * @param region Runtime allocator for temporary objects.
 * @param port Port to store tuples.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
static inline int
sql_row_to_port(struct Vdbe *stmt, struct region *region, struct port *port)
{
	uint32_t size;
	size_t svp = region_used(region);
	char *pos = sql_stmt_result_to_msgpack(stmt, &size, region);
	struct tuple *tuple =
		tuple_new(box_tuple_format_default(), pos, pos + size);
	if (tuple == NULL)
		goto error;
	region_truncate(region, svp);
	return port_c_add_tuple(port, tuple);

error:
	region_truncate(region, svp);
	return -1;
}

static bool
sql_stmt_schema_version_is_valid(struct Vdbe *stmt)
{
	return sql_stmt_schema_version(stmt) == box_schema_version();
}

/**
 * Re-compile statement and refresh global prepared statement
 * cache with the newest value.
 */
static int
sql_reprepare(struct Vdbe **stmt)
{
	const char *sql_str = sql_stmt_query_str(*stmt);
	struct Vdbe *new_stmt;
	if (sql_stmt_compile(sql_str, strlen(sql_str), NULL,
			     &new_stmt, NULL, true) != 0)
		return -1;
	if (sql_stmt_cache_update(*stmt, new_stmt) != 0)
		return -1;
	*stmt = new_stmt;
	return 0;
}

/**
 * Compile statement and save it to the global holder;
 * update session hash with prepared statement ID (if
 * it's not already there).
 */
int
sql_prepare(const char *sql, int len, struct port *port)
{
	uint32_t stmt_id = sql_stmt_calculate_id(sql, len);
	struct Vdbe *stmt = sql_stmt_cache_find(stmt_id);
	rmean_collect(rmean_box, IPROTO_PREPARE, 1);
	if (stmt == NULL) {
		if (sql_stmt_compile(sql, len, NULL, &stmt, NULL, true) != 0)
			return -1;
		if (sql_stmt_cache_insert(stmt) != 0) {
			sql_stmt_finalize(stmt);
			return -1;
		}
	} else {
		if (!sql_stmt_schema_version_is_valid(stmt) &&
		    !sql_stmt_busy(stmt)) {
			if (sql_reprepare(&stmt) != 0)
				return -1;
		}
	}
	assert(stmt != NULL);
	/* Add id to the list of available statements in session. */
	if (!session_check_stmt_id(current_session(), stmt_id))
		session_add_stmt_id(current_session(), stmt_id);
	enum sql_serialization_format format = sql_column_count(stmt) > 0 ?
					   DQL_PREPARE : DML_PREPARE;
	port_sql_create(port, stmt, format, false);

	return 0;
}

/**
 * Deallocate prepared statement from current session:
 * remove its ID from session-local hash and unref entry
 * in global holder.
 */
int
sql_unprepare(uint32_t stmt_id)
{
	if (!session_check_stmt_id(current_session(), stmt_id)) {
		diag_set(ClientError, ER_WRONG_QUERY_ID, stmt_id);
		return -1;
	}
	session_remove_stmt_id(current_session(), stmt_id);
	sql_stmt_unref(stmt_id);
	return 0;
}

/**
 * Execute prepared SQL statement.
 *
 * This function uses region to allocate memory for temporary
 * objects. After this function, region will be in the same state
 * in which it was before this function.
 *
 * @param db SQL handle.
 * @param stmt Prepared statement.
 * @param port Port to store SQL response.
 * @param region Region to allocate temporary objects.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
static inline int
sql_execute(struct Vdbe *stmt, struct port *port, struct region *region)
{
	int rc, column_count = sql_column_count(stmt);
	rmean_collect(rmean_box, IPROTO_EXECUTE, 1);
	if (column_count > 0) {
		/* Either ROW or DONE or ERROR. */
		while ((rc = sql_step(stmt)) == SQL_ROW) {
			if (sql_row_to_port(stmt, region, port) != 0)
				return -1;
		}
		assert(rc == SQL_DONE || rc != 0);
	} else {
		/* No rows. Either DONE or ERROR. */
		rc = sql_step(stmt);
		assert(rc != SQL_ROW && rc != 0);
	}
	if (rc != SQL_DONE)
		return -1;
	return 0;
}

int
sql_execute_prepared(uint32_t stmt_id, const struct sql_bind *bind,
		     uint32_t bind_count, struct port *port,
		     struct region *region)
{

	if (!session_check_stmt_id(current_session(), stmt_id)) {
		diag_set(ClientError, ER_WRONG_QUERY_ID, stmt_id);
		return -1;
	}
	struct Vdbe *stmt = sql_stmt_cache_find(stmt_id);
	assert(stmt != NULL);
	if (!sql_stmt_schema_version_is_valid(stmt)) {
		diag_set(ClientError, ER_SQL_EXECUTE, "statement has expired");
		return -1;
	}
	if (sql_stmt_busy(stmt)) {
		const char *sql_str = sql_stmt_query_str(stmt);
		return sql_prepare_and_execute(sql_str, strlen(sql_str), bind,
					       bind_count, port, region);
	}
	/*
	 * Clear all set from previous execution cycle values to be bound and
	 * remove autoincrement IDs generated in that cycle.
	 */
	sql_unbind(stmt);
	if (sql_bind(stmt, bind, bind_count) != 0)
		return -1;
	sql_reset_autoinc_id_list(stmt);
	enum sql_serialization_format format = sql_column_count(stmt) > 0 ?
					       DQL_EXECUTE : DML_EXECUTE;
	port_sql_create(port, stmt, format, false);
	if (sql_execute(stmt, port, region) != 0) {
		port_destroy(port);
		sql_stmt_reset(stmt);
		return -1;
	}
	sql_stmt_reset(stmt);

	return 0;
}

int
sql_prepare_and_execute(const char *sql, int len, const struct sql_bind *bind,
			uint32_t bind_count, struct port *port,
			struct region *region)
{
	size_t sql_len = len >= 0 ? (size_t)len : strlen(sql);
	uint32_t sql_flags = current_session()->sql_flags;
	uint32_t stmt_id = sql_stmt_calculate_id(sql, sql_len);

	/* Try the auto cache — skips parse + codegen + JIT compile on hit. */
	struct Vdbe *stmt = auto_cache_lookup(stmt_id, sql_flags, sql, sql_len);
	bool from_cache = (stmt != NULL);

	if (stmt == NULL) {
		if (sql_stmt_compile(sql, len, NULL, &stmt, NULL, false) != 0)
			return -1;
		assert(stmt != NULL);
		/*
		 * Hand ownership to the cache.  The cache manages the stmt
		 * lifetime from here on; we must not finalize it ourselves.
		 */
		auto_cache_insert(stmt_id, sql_flags, stmt);
	} else {
		/* Clear state left over from the previous execution cycle. */
		sql_unbind(stmt);
		sql_reset_autoinc_id_list(stmt);
	}

	enum sql_serialization_format format = sql_column_count(stmt) > 0 ?
					       DQL_EXECUTE : DML_EXECUTE;
	/*
	 * Never auto-destroy: the cache owns the stmt.
	 * We reset it manually after execution.
	 */
	port_sql_create(port, stmt, format, false);
	int rc = 0;
	if (sql_bind(stmt, bind, bind_count) != 0 ||
	    sql_execute(stmt, port, region) != 0) {
		port_destroy(port);
		rc = -1;
	}
	sql_stmt_reset(stmt);
	(void)from_cache;
	return rc;
}

int
box_process_sql(const struct sql_request *request, struct port *port)
{
	if (access_check_sql() != 0)
		return -1;
	struct region *region = &fiber()->gc;
	struct sql_bind *bind = NULL;
	int bind_count = 0;
	if (request->bind != NULL) {
		bind_count = sql_bind_list_decode(request->bind, &bind);
		if (bind_count < 0)
			return -1;
	}
	/*
	 * There are four options:
	 * 1. Prepare SQL query (IPROTO_PREPARE + SQL string);
	 * 2. Unprepare SQL query (IPROTO_PREPARE + stmt id);
	 * 3. Execute SQL query (IPROTO_EXECUTE + SQL string);
	 * 4. Execute prepared query (IPROTO_EXECUTE + stmt id).
	 */
	if (request->execute) {
		if (request->sql_text != NULL) {
			assert(request->stmt_id == NULL);
			const char *sql = request->sql_text;
			uint32_t len;
			sql = mp_decode_str(&sql, &len);
			return sql_prepare_and_execute(sql, len,
						       bind, bind_count,
						       port, region);
		} else {
			assert(request->stmt_id != NULL);
			const char *data = request->stmt_id;
			uint32_t stmt_id = mp_decode_uint(&data);
			return sql_execute_prepared(stmt_id, bind, bind_count,
						    port, region);
		}
	} else {
		if (request->sql_text != NULL) {
			assert(request->stmt_id == NULL);
			const char *sql = request->sql_text;
			uint32_t len;
			sql = mp_decode_str(&sql, &len);
			return sql_prepare(sql, len, port);
		} else {
			assert(request->stmt_id != NULL);
			const char *data = request->stmt_id;
			uint32_t stmt_id = mp_decode_uint(&data);
			if (sql_unprepare(stmt_id) != 0)
				return -1;
			port_sql_create(port, NULL, UNPREPARE, false);
			return 0;
		}
	}
}
