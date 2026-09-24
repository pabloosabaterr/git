/* We need this macro to access core_apply_sparse_checkout */
#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "git-compat-util.h"
#include "config.h"
#include "parse-options.h"
#include "repository.h"
#include "commit.h"
#include "dir.h"
#include "environment.h"
#include "hex.h"
#include "tree.h"
#include "tree-walk.h"
#include "object.h"
#include "odb.h"
#include "oid-array.h"
#include "oidset.h"
#include "promisor-remote.h"
#include "strmap.h"
#include "string-list.h"
#include "revision.h"
#include "trace2.h"
#include "progress.h"
#include "packfile.h"
#include "path-walk.h"
#include "transport.h"
#include "remote.h"
#include "fetch-object-info.h"

static const char *const builtin_backfill_usage[] = {
	N_("git backfill [--min-batch-size=<n>] [--[no-]sparse] [--[no-]include-edges] [--dry-run] [<revision-range>]"),
	NULL
};

struct backfill_context {
	struct repository *repo;
	struct oid_array current_batch;
	size_t min_batch_size;
	int sparse;
	int include_edges;
	int dry_run;
	int object_info_enabled;
	size_t total_batch_size;
	size_t total_batch_nr;
	struct transport *object_info_transport;
	struct fetch_object_info_results object_info_results;
	struct rev_info revs;
};

static void backfill_context_clear(struct backfill_context *ctx)
{
	oid_array_clear(&ctx->current_batch);
}

static void download_batch(struct backfill_context *ctx)
{
	promisor_remote_get_direct(ctx->repo,
				   ctx->current_batch.oid,
				   ctx->current_batch.nr);
	oid_array_clear(&ctx->current_batch);

	/*
	 * We likely have a new packfile. Add it to the packed list to
	 * avoid possible duplicate downloads of the same objects.
	 */
	odb_reprepare(ctx->repo->objects);
}

static void dry_run_batch(struct backfill_context *ctx)
{
	struct fetch_object_info_results *results = &ctx->object_info_results;
	enum object_info_fetch_result status;

	if (!ctx->current_batch.nr)
		return;

	ctx->total_batch_nr += ctx->current_batch.nr;

	if (!ctx->object_info_enabled)
		goto cleanup;

	if (!ctx->object_info_transport) {
		struct promisor_remote *promise =
			repo_promisor_remote_find(ctx->repo, NULL);
		struct remote *remote = NULL;

		if (!promise || !(remote = remote_get(promise->name)))
			die(_("--dry-run requires a promisor remote"));

		ctx->object_info_transport = transport_get(remote, NULL);

		if (!ctx->object_info_transport->smart_options)
			die(_("failed to get object info: smart options required"));
	}

	results->wants_size = 1;
	status = transport_fetch_object_info(ctx->object_info_transport,
					     &ctx->current_batch,
					     results);

	if (status == OBJECT_INFO_NOT_ENABLED ||
	    !results->sizes) {
		ctx->object_info_enabled = 0;
		goto cleanup;
	}

	for (size_t i = 0; i < results->nr; i++)
		ctx->total_batch_size += results->sizes[i];

cleanup:
	free_fetch_object_info_results(&ctx->object_info_results);
	oid_array_clear(&ctx->current_batch);
}

static int fill_missing_blobs(const char *path UNUSED,
			      struct oid_array *list,
			      enum object_type type,
			      void *data)
{
	struct backfill_context *ctx = data;

	if (type != OBJ_BLOB)
		return 0;

	for (size_t i = 0; i < list->nr; i++) {
		if (!odb_has_object(ctx->repo->objects, &list->oid[i], 0))
			oid_array_append(&ctx->current_batch, &list->oid[i]);
	}

	if (ctx->current_batch.nr >= ctx->min_batch_size) {
		if (ctx->dry_run)
			dry_run_batch(ctx);
		else
			download_batch(ctx);
	}

	return 0;
}

static void reject_unsupported_rev_list_options(struct rev_info *revs)
{
	if (revs->diffopt.pickaxe)
		die(_("'%s' cannot be used with 'git backfill'"),
		    (revs->diffopt.pickaxe_opts & DIFF_PICKAXE_REGEX) ? "-G" : "-S");
	if (revs->diffopt.filter || revs->diffopt.filter_not)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "--diff-filter");
	if (revs->diffopt.flags.follow_renames)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "--follow");
	if (revs->line_level_traverse)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "-L");
	if (revs->explicit_diff_merges)
		die(_("'%s' cannot be used with 'git backfill'"),
		    "--diff-merges");
	if (!path_walk_filter_compatible(&revs->filter))
		die(_("cannot backfill with these filter options"));
	if (revs->filter.blob_limit_value)
		die(_("cannot backfill with blob size limits"));
}

static int do_backfill(struct backfill_context *ctx)
{
	struct path_walk_info info = PATH_WALK_INFO_INIT;
	int ret;

	if (ctx->sparse) {
		CALLOC_ARRAY(info.pl, 1);
		info.pl_sparse_trees = 1;
		if (get_sparse_checkout_patterns(info.pl)) {
			path_walk_info_clear(&info);
			return error(_("problem loading sparse-checkout"));
		}
	}

	/* Walk from HEAD if otherwise unspecified. */
	if (!ctx->revs.pending.nr)
		add_head_to_pending(&ctx->revs);
	if (ctx->include_edges)
		ctx->revs.edge_hint = 1;

	info.blobs = 1;
	info.tags = info.commits = info.trees = 0;

	info.revs = &ctx->revs;
	info.path_fn = fill_missing_blobs;
	info.path_fn_data = ctx;

	ret = walk_objects_by_path(&info);

	if (ret)
		goto end;

	/* Download the objects that did not fill a batch. */
	if (!ctx->dry_run) {
		download_batch(ctx);
		goto end;
	}

	dry_run_batch(ctx);

	if (ctx->object_info_enabled) {
		struct strbuf size = STRBUF_INIT;

		strbuf_humanise_bytes(&size, ctx->total_batch_size);
		fprintf(stderr,
			Q_("After backfill, %" PRIuMAX " blob would be fetched "
			   "(total size of: %s).\n",
			   "After backfill, %" PRIuMAX " blobs would be fetched "
			   "(total size of: %s).\n",
			   (unsigned long)ctx->total_batch_nr),
			(uintmax_t)ctx->total_batch_nr, size.buf);
		strbuf_release(&size);
	} else {
		fprintf(stderr,
			Q_("After backfill, %" PRIuMAX " blob would be fetched.\n",
			   "After backfill, %" PRIuMAX " blobs would be fetched.\n",
			   (unsigned long)ctx->total_batch_nr),
			(uintmax_t)ctx->total_batch_nr);
	}

end:
	if (ctx->object_info_transport)
		transport_disconnect(ctx->object_info_transport);
	path_walk_info_clear(&info);
	return ret;
}

int cmd_backfill(int argc, const char **argv, const char *prefix, struct repository *repo)
{
	int result;
	struct backfill_context ctx = {
		.repo = repo,
		.current_batch = OID_ARRAY_INIT,
		.min_batch_size = 50000,
		.sparse = -1,
		.revs = REV_INFO_INIT,
		.include_edges = 1,
		.object_info_results = FETCH_OBJECT_INFO_RESULTS_INIT,
		.object_info_enabled = 1,
	};
	struct option options[] = {
		OPT_UNSIGNED(0, "min-batch-size", &ctx.min_batch_size,
			     N_("Minimum number of objects to request at a time")),
		OPT_BOOL(0, "sparse", &ctx.sparse,
			 N_("Restrict the missing objects to the current sparse-checkout")),
		OPT_BOOL(0, "include-edges", &ctx.include_edges,
			 N_("Include blobs from boundary commits in the backfill")),
		OPT__DRY_RUN(&ctx.dry_run, N_("Preview the number of blobs and their total "
					      "size to be fetched")),
		OPT_END(),
	};
	struct repo_config_values *cfg = repo_config_values(the_repository);

	show_usage_with_options_if_asked(argc, argv,
					 builtin_backfill_usage, options);

	argc = parse_options(argc, argv, prefix, options, builtin_backfill_usage,
			     PARSE_OPT_KEEP_UNKNOWN_OPT |
			     PARSE_OPT_KEEP_ARGV0 |
			     PARSE_OPT_KEEP_DASHDASH);

	repo_init_revisions(repo, &ctx.revs, prefix);
	argc = setup_revisions(argc, argv, &ctx.revs, NULL);

	if (argc > 1)
		die(_("unrecognized argument: %s"), argv[1]);
	reject_unsupported_rev_list_options(&ctx.revs);

	repo_config(repo, git_default_config, NULL);

	if (ctx.sparse < 0)
		ctx.sparse = cfg->apply_sparse_checkout;

	result = do_backfill(&ctx);
	backfill_context_clear(&ctx);
	release_revisions(&ctx.revs);
	return result;
}
