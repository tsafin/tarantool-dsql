#include "sql_stats_analyze.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "box.h"
#include "diag.h"
#include "index.h"
#include "index_def.h"
#include "schema.h"
#include "space.h"
#include "space_cache.h"
#include "sql.h"
#include "sqlInt.h"
#include "sql_stats_analyze_budget.h"
#include "sql_stats_collection.h"
#include "sql_stats_snapshot.h"
#include "vdbeInt.h"

struct analyze_space {
	struct space *space;
	size_t index_offset;
	uint32_t index_count;
};

struct analyze_discovery {
	struct analyze_space *spaces;
	size_t space_count;
	size_t index_count;
	bool too_many;
};

static int
analyze_discover_space(struct space *space, void *arg)
{
	struct analyze_discovery *discovery = arg;
	if (space_is_system(space) || space->def->opts.is_view ||
	    space_is_data_temporary(space))
		return 0;
	if (space->index_count == 0 ||
	    space->index_count > SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS -
				     discovery->index_count ||
	    discovery->space_count >= SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS) {
		discovery->too_many = true;
		return -1;
	}
	struct analyze_space *entry =
		&discovery->spaces[discovery->space_count++];
	entry->space = space;
	entry->index_offset = discovery->index_count;
	entry->index_count = space->index_count;
	discovery->index_count += space->index_count;
	return 0;
}

static void
analyze_budget_failure(void)
{
	diag_set(ClientError, ER_SQL_EXECUTE,
		 "ANALYZE exceeded a fixed collection budget or encountered an "
		 "unsupported target; no statistics were published");
}

int
sql_stats_analyze_execute(const char *space_name)
{
	struct analyze_space spaces[SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS];
	struct sql_stats_collection_target targets[
		SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS];
	struct sql_stats_expected_index expected_indexes[
		SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS];
	struct sql_stats_tx_index_spec specs[
		SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS];
	struct sql_stats_collection_relation_spec relations[
		SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS];
	struct sql_stats_expected_relation expected_relations[
		SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS];
	memset(spaces, 0, sizeof(spaces));
	memset(targets, 0, sizeof(targets));
	memset(expected_indexes, 0, sizeof(expected_indexes));
	memset(specs, 0, sizeof(specs));
	memset(relations, 0, sizeof(relations));
	memset(expected_relations, 0, sizeof(expected_relations));
	struct analyze_discovery discovery = {
		.spaces = spaces,
	};
	uint64_t catalog_version = box_catalog_version();
	uint64_t schema_version = box_schema_version();
	if (space_name != NULL) {
		struct space *space = space_by_name0(space_name);
		if (space == NULL) {
			diag_set(ClientError, ER_NO_SUCH_SPACE, space_name);
			return -1;
		}
		if (space->def->opts.is_view) {
			diag_set(ClientError, ER_SQL_ANALYZE_ARGUMENT, space_name);
			return -1;
		}
		if (space_is_system(space))
			return 0;
		if (space_is_data_temporary(space)) {
			analyze_budget_failure();
			return -1;
		}
		if (space->index_count == 0 || space->index_count >
					 SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS) {
			analyze_budget_failure();
			return -1;
		}
		spaces[0] = (struct analyze_space) {
			.space = space,
			.index_count = space->index_count,
		};
		discovery.space_count = 1;
		discovery.index_count = space->index_count;
	} else {
		if (space_foreach(analyze_discover_space, &discovery) != 0) {
			analyze_budget_failure();
			return -1;
		}
		/* The empty eligible set is a successful no-op. */
		if (discovery.space_count == 0)
			return 0;
	}
	if (box_catalog_version() != catalog_version ||
	    box_schema_version() != schema_version) {
		analyze_budget_failure();
		return -1;
	}
	for (size_t r = 0; r < discovery.space_count; r++) {
		struct analyze_space *planned = &spaces[r];
		struct space *space = planned->space;
		if (planned->index_count == 0 || space->index == NULL)
			goto fail;
		expected_relations[r] = (struct sql_stats_expected_relation) {
			.space_id = space->def->id,
			/* There is no per-space DML epoch in the volatile prototype. */
			.modification_epoch = 0,
			.indexes = &expected_indexes[planned->index_offset],
			.index_count = planned->index_count,
		};
		for (size_t j = 0; j < planned->index_count; j++) {
			struct index *index = space->index[j];
			if (index == NULL || index->def == NULL ||
			    index->def->key_def == NULL ||
			    index->def->key_def->part_count == 0 ||
			    index->def->iid > space->index_id_max ||
			    space->index_map[index->def->iid] != index)
				goto fail;
			size_t i = planned->index_offset + j;
			uint64_t work_limit = SQL_STATS_ANALYZE_MAX_TUPLES_EXAMINED;
			if (index->def->iid == 0) {
				ssize_t population = index_size(index);
				if (population >= 0 && (uint64_t)population < work_limit)
					work_limit = (uint64_t)population + 1;
			}
			if (work_limit == 0)
				work_limit = 1;
			expected_indexes[i] = (struct sql_stats_expected_index) {
				.index_id = index->def->iid,
				.definition_version = index->unique_id,
				.part_count = index->def->key_def->part_count,
			};
			targets[i] = (struct sql_stats_collection_target) {
				.space_id = space->def->id,
				.index_id = index->def->iid,
			};
			uint64_t seed = schema_version ^
				((uint64_t)space->def->id << 32) ^
				((uint64_t)index->unique_id << 1) ^ 0x9e3779b97f4a7c15ULL;
			specs[i] = (struct sql_stats_tx_index_spec) {
				.target = targets[i],
				.expected = &expected_indexes[i],
				.request = {
					.index_id = index->def->iid,
					.max_rows = SQL_STATS_ANALYZE_MAX_SAMPLE_ROWS,
					.max_bytes = SQL_STATS_ANALYZE_MAX_SAMPLE_BYTES,
					.seed = seed,
					.max_buffer_bytes =
						SQL_STATS_ANALYZE_MAX_RESERVOIR_BYTES,
					.max_tuples_examined = work_limit,
					.max_disk_sources =
						SQL_STATS_ANALYZE_MAX_DISK_SOURCES,
					.max_page_reads =
						SQL_STATS_ANALYZE_MAX_PAGE_READS,
					.max_iterator_keys =
						SQL_STATS_ANALYZE_MAX_ITERATOR_KEYS,
				},
				.hll_precision = SQL_STATS_ANALYZE_HLL_PRECISION,
				.hll_seed = seed,
				.summary_max_bytes =
					SQL_STATS_ANALYZE_INDEX_SUMMARY_BYTES,
				.use_native_index_hash = true,
			};
		}
		relations[r] = (struct sql_stats_collection_relation_spec) {
			.expected = &expected_relations[r],
			.indexes = &specs[planned->index_offset],
			.index_count = planned->index_count,
			.relation_index_id = 0,
			.relation_confidence = 0.5,
			.confidence_source = "sql-analyze-fixed-sample-v1",
		};
	}
	struct sql_stats_collection_context *context =
		sql_stats_collection_context_new(targets, discovery.index_count);
	if (context == NULL ||
	    sql_stats_collection_context_catalog_version(context) !=
		    catalog_version ||
	    sql_stats_collection_context_schema_version(context) != schema_version)
		goto fail_context;
	struct sql_stats_collection_build_budget budget = {
		.max_index_requests = SQL_STATS_ANALYZE_MAX_INDEX_REQUESTS,
		.max_staging_bytes = SQL_STATS_ANALYZE_MAX_STAGING_BYTES,
		.max_candidate_bytes = SQL_STATS_ANALYZE_MAX_CANDIDATE_BYTES,
		.max_temp_bytes = SQL_STATS_ANALYZE_MAX_TEMP_BYTES,
		.max_work = SQL_STATS_ANALYZE_MAX_WORK,
	};
	struct sql_stats_snapshot *candidate =
		sql_stats_collection_context_build_sample_candidates(context,
			relations, discovery.space_count, &budget);
	if (candidate == NULL)
		goto fail_context;
	int rc;
	if (space_name == NULL) {
		rc = sql_stats_collection_context_publish_candidate(&context,
								    candidate);
	} else {
		struct sql_stats_snapshot *base = sql_get_stats_snapshot();
		rc = sql_stats_collection_context_publish_replacement(&context,
			base, spaces[0].space->def->id,
			SQL_STATS_ANALYZE_MAX_CANDIDATE_BYTES);
		sql_stats_snapshot_release(base);
	}
	sql_stats_snapshot_release(candidate);
	if (rc == 0)
		return 0;
	sql_stats_collection_context_delete(context);
	analyze_budget_failure();
	return -1;
fail_context:
	if (context != NULL)
		sql_stats_collection_context_delete(context);
fail:
	analyze_budget_failure();
	return -1;
}

void
sqlAnalyze(struct Parse *parse, const struct Token *name)
{
	struct Vdbe *vdbe = sqlGetVdbe(parse);
	if (vdbe == NULL)
		return;
	char *space_name = name == NULL ? NULL : sql_name_from_token(name);
	sqlVdbeAddOp4(vdbe, OP_Analyze, name != NULL, 0, 0, space_name,
		      name != NULL ? P4_TRANSIENT : P4_NOTUSED);
	sql_xfree(space_name);
}
