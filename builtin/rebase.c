/*
 * "git rebase" builtin command
 *
 * Copyright (c) 2018 Pratik Karki
 */

#include "builtin.h"
#include "run-command.h"
#include "exec-cmd.h"
#include "argv-array.h"
#include "dir.h"
#include "packfile.h"
#include "refs.h"
#include "quote.h"
#include "config.h"
#include "cache-tree.h"
#include "unpack-trees.h"
#include "lockfile.h"
#include "parse-options.h"
#include "commit.h"
#include "diff.h"

static char const * const builtin_rebase_usage[] = {
	N_("git rebase [-i] [options] [--exec <cmd>] [--onto <newbase>] "
		"[<upstream>] [<branch>]"),
	N_("git rebase [-i] [options] [--exec <cmd>] [--onto <newbase>] "
		"--root [<branch>]"),
	N_("git rebase --continue | --abort | --skip | --edit-todo"),
	NULL
};

static GIT_PATH_FUNC(apply_dir, "rebase-apply")
static GIT_PATH_FUNC(merge_dir, "rebase-merge")

enum rebase_type {
	REBASE_UNSPECIFIED = -1,
	REBASE_AM,
	REBASE_MERGE,
	REBASE_INTERACTIVE,
	REBASE_PRESERVE_MERGES
};

static int use_builtin_rebase(void)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;
	int ret;

	argv_array_pushl(&cp.args,
			 "config", "--bool", "rebase.usebuiltin", NULL);
	cp.git_cmd = 1;
	if (capture_command(&cp, &out, 6)) {
		strbuf_release(&out);
		return 0;
	}

	strbuf_trim(&out);
	ret = !strcmp("true", out.buf);
	strbuf_release(&out);
	return ret;
}

static int apply_autostash(void)
{
	warning("TODO");
	return 0;
}

struct rebase_options {
	enum rebase_type type;
	const char *state_dir;
	struct commit *upstream;
	const char *upstream_name;
	const char *upstream_arg;
	char *head_name;
	struct object_id orig_head;
	struct commit *onto;
	const char *onto_name;
	const char *revisions;
	int root;
	struct commit *restrict_revision;
	int dont_finish_rebase;
	enum {
		REBASE_NO_QUIET = 1<<0,
		REBASE_VERBOSE = 1<<1,
		REBASE_DIFFSTAT = 1<<2,
	} flags;
	struct strbuf git_am_opt;
};

/* Returns the filename prefixed by the state_dir */
static const char *state_dir_path(const char *filename, struct rebase_options *opts)
{
	static struct strbuf path = STRBUF_INIT;
	static size_t prefix_len;

	if (!prefix_len) {
		strbuf_addf(&path, "%s/", opts->state_dir);
		prefix_len = path.len;
	}

	strbuf_setlen(&path, prefix_len);
	strbuf_addstr(&path, filename);
	return path.buf;
}

static int finish_rebase(struct rebase_options *opts)
{
	struct strbuf dir = STRBUF_INIT;
	const char *argv_gc_auto[] = { "gc", "--auto", NULL };

	delete_ref(NULL, "REBASE_HEAD", NULL, REF_NO_DEREF);
	apply_autostash();
	close_all_packs(the_repository->objects);
	/*
	 * We ignore errors in 'gc --auto', since the
	 * user should see them.
	 */
	run_command_v_opt(argv_gc_auto, RUN_GIT_CMD);
	strbuf_addstr(&dir, opts->state_dir);
	remove_dir_recursively(&dir, 0);
	strbuf_release(&dir);

	return 0;
}

static struct commit *peel_committish(const char *name)
{
	struct object *obj;
	struct object_id oid;

	if (get_oid(name, &oid))
		return NULL;
	obj = parse_object(the_repository, &oid);
	return (struct commit *)peel_to_type(name, 0, obj, OBJ_COMMIT);
}

static void add_var(struct strbuf *buf, const char *name, const char *value)
{
	if (!value)
		strbuf_addf(buf, "unset %s; ", name);
	else {
		strbuf_addf(buf, "%s=", name);
		sq_quote_buf(buf, value);
		strbuf_addstr(buf, "; ");
	}
}

static int run_specific_rebase(struct rebase_options *opts)
{
	const char *argv[] = { NULL, NULL };
	struct strbuf script_snippet = STRBUF_INIT;
	int status;
	const char *backend, *backend_func;

	add_var(&script_snippet, "GIT_DIR", absolute_path(get_git_dir()));
	add_var(&script_snippet, "state_dir", opts->state_dir);

	add_var(&script_snippet, "upstream_name", opts->upstream_name);
	add_var(&script_snippet, "upstream",
				 oid_to_hex(&opts->upstream->object.oid));
	add_var(&script_snippet, "head_name", opts->head_name);
	add_var(&script_snippet, "orig_head", oid_to_hex(&opts->orig_head));
	add_var(&script_snippet, "onto", oid_to_hex(&opts->onto->object.oid));
	add_var(&script_snippet, "onto_name", opts->onto_name);
	add_var(&script_snippet, "revisions", opts->revisions);
	add_var(&script_snippet, "restrict_revision", opts->restrict_revision ?
		oid_to_hex(&opts->restrict_revision->object.oid) : NULL);
	add_var(&script_snippet, "GIT_QUIET",
		opts->flags & REBASE_NO_QUIET ? "" : "t");
	add_var(&script_snippet, "git_am_opt", opts->git_am_opt.buf);
	add_var(&script_snippet, "verbose",
		opts->flags & REBASE_VERBOSE ? "t" : "");
	add_var(&script_snippet, "diffstat",
		opts->flags & REBASE_DIFFSTAT ? "t" : "");

	switch (opts->type) {
	case REBASE_AM:
		backend = "git-rebase--am";
		backend_func = "git_rebase__am";
		break;
	case REBASE_INTERACTIVE:
		backend = "git-rebase--interactive";
		backend_func = "git_rebase__interactive";
		break;
	case REBASE_MERGE:
		backend = "git-rebase--merge";
		backend_func = "git_rebase__merge";
		break;
	case REBASE_PRESERVE_MERGES:
		backend = "git-rebase--preserve-merges";
		backend_func = "git_rebase__preserve_merges";
		break;
	default:
		BUG("Unhandled rebase type %d", opts->type);
		break;
	}

	strbuf_addf(&script_snippet,
		    ". git-sh-setup && . git-rebase--common &&"
		    " . %s && %s", backend, backend_func);
	argv[0] = script_snippet.buf;

	status = run_command_v_opt(argv, RUN_USING_SHELL);
	if (opts->dont_finish_rebase)
		; /* do nothing */
	else if (status == 0) {
		if (!file_exists(state_dir_path("stopped-sha", opts)))
			finish_rebase(opts);
	} else if (status == 2) {
		struct strbuf dir = STRBUF_INIT;

		apply_autostash();
		strbuf_addstr(&dir, opts->state_dir);
		remove_dir_recursively(&dir, 0);
		strbuf_release(&dir);
		die("Nothing to do");
	}

	strbuf_release(&script_snippet);

	return status ? -1 : 0;
}

#define GIT_REFLOG_ACTION_ENVIRONMENT "GIT_REFLOG_ACTION"

static int reset_head(struct object_id *oid, const char *action,
		      const char *switch_to_branch, int detach_head)
{
	struct object_id head_oid;
	struct tree_desc desc;
	struct lock_file lock = LOCK_INIT;
	struct unpack_trees_options unpack_tree_opts;
	struct tree *tree;
	const char *reflog_action;
	struct strbuf msg = STRBUF_INIT;
	size_t prefix_len;
	struct object_id *orig = NULL, oid_orig,
		*old_orig = NULL, oid_old_orig;
	int ret = 0;

	if (hold_locked_index(&lock, LOCK_REPORT_ON_ERROR) < 0)
		return -1;

	if (!oid) {
		if (get_oid("HEAD", &head_oid)) {
			rollback_lock_file(&lock);
			return error(_("could not determine HEAD revision"));
		}
		oid = &head_oid;
	}

	memset(&unpack_tree_opts, 0, sizeof(unpack_tree_opts));
	setup_unpack_trees_porcelain(&unpack_tree_opts, action);
	unpack_tree_opts.head_idx = 1;
	unpack_tree_opts.src_index = the_repository->index;
	unpack_tree_opts.dst_index = the_repository->index;
	unpack_tree_opts.fn = oneway_merge;
	unpack_tree_opts.update = 1;
	unpack_tree_opts.merge = 1;
	if (!detach_head)
		unpack_tree_opts.reset = 1;

	if (read_index_unmerged(the_repository->index) < 0) {
		rollback_lock_file(&lock);
		return error(_("could not read index"));
	}

	if (!fill_tree_descriptor(&desc, oid)) {
		error(_("failed to find tree of %s"), oid_to_hex(oid));
		rollback_lock_file(&lock);
		free((void *)desc.buffer);
		return -1;
	}

	if (unpack_trees(1, &desc, &unpack_tree_opts)) {
		rollback_lock_file(&lock);
		free((void *)desc.buffer);
		return -1;
	}

	tree = parse_tree_indirect(oid);
	prime_cache_tree(the_repository->index, tree);

	if (write_locked_index(the_repository->index, &lock, COMMIT_LOCK) < 0)
		ret = error(_("could not write index"));
	free((void *)desc.buffer);

	if (ret)
		return ret;

	reflog_action = getenv(GIT_REFLOG_ACTION_ENVIRONMENT);
	strbuf_addf(&msg, "%s: ", reflog_action ? reflog_action : "rebase");
	prefix_len = msg.len;

	if (!get_oid("ORIG_HEAD", &oid_old_orig))
		old_orig = &oid_old_orig;
	if (!get_oid("HEAD", &oid_orig)) {
		orig = &oid_orig;
		strbuf_addstr(&msg, "updating ORIG_HEAD");
		update_ref(msg.buf, "ORIG_HEAD", orig, old_orig, 0,
			   UPDATE_REFS_MSG_ON_ERR);
	} else if (old_orig)
		delete_ref(NULL, "ORIG_HEAD", old_orig, 0);
	strbuf_setlen(&msg, prefix_len);
	strbuf_addstr(&msg, "updating HEAD");
	if (!switch_to_branch)
		ret = update_ref(msg.buf, "HEAD", oid, orig, REF_NO_DEREF,
				 UPDATE_REFS_MSG_ON_ERR);
	else {
		ret = create_symref("HEAD", switch_to_branch, msg.buf);
		if (!ret)
			ret = update_ref(msg.buf, "HEAD", oid, NULL, 0,
					 UPDATE_REFS_MSG_ON_ERR);
	}

	strbuf_release(&msg);
	return ret;
}

static int rebase_config(const char *var, const char *value, void *data)
{
	struct rebase_options *opts = data;

	if (!strcmp(var, "rebase.stat")) {
		if (git_config_bool(var, value))
			opts->flags |= REBASE_DIFFSTAT;
		else
			opts->flags &= !REBASE_DIFFSTAT;
		return 0;
	}

	return git_default_config(var, value, data);
}

int cmd_rebase(int argc, const char **argv, const char *prefix)
{
	struct rebase_options options = {
		.type = REBASE_UNSPECIFIED,
		.flags = REBASE_NO_QUIET,
		.git_am_opt = STRBUF_INIT,
	};
	const char *branch_name;
	int ret, flags;
	int ok_to_skip_pre_rebase = 0;
	struct strbuf msg = STRBUF_INIT;
	struct strbuf revisions = STRBUF_INIT;
	struct object_id merge_base;
	struct option builtin_rebase_options[] = {
		OPT_STRING(0, "onto", &options.onto_name,
			   N_("revision"),
			   N_("rebase onto given branch instead of upstream")),
		OPT_BOOL(0, "no-verify", &ok_to_skip_pre_rebase,
			 N_("allow pre-rebase hook to run")),
		OPT_NEGBIT('q', "quiet", &options.flags,
			   N_("be quiet. implies --no-stat"),
			   REBASE_NO_QUIET| REBASE_VERBOSE | REBASE_DIFFSTAT),
		OPT_BIT('v', "verbose", &options.flags,
			N_("display a diffstat of what changed upstream"),
			REBASE_NO_QUIET | REBASE_VERBOSE | REBASE_DIFFSTAT),
		{OPTION_NEGBIT, 'n', "no-stat", &options.flags, NULL,
			N_("do not show diffstat of what changed upstream"),
			PARSE_OPT_NOARG, NULL, REBASE_DIFFSTAT },
		OPT_END(),
	};

	/*
	 * NEEDSWORK: Once the builtin rebase has been tested enough
	 * and git-legacy-rebase.sh is retired to contrib/, this preamble
	 * can be removed.
	 */

	if (!use_builtin_rebase()) {
		const char *path = mkpath("%s/git-legacy-rebase",
					  git_exec_path());

		if (sane_execvp(path, (char **)argv) < 0)
			die_errno(_("could not exec %s"), path);
		else
			BUG("sane_execvp() returned???");
	}

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_rebase_usage,
				   builtin_rebase_options);

	prefix = setup_git_directory();
	trace_repo_setup(prefix);
	setup_work_tree();

	git_config(rebase_config, &options);

	argc = parse_options(argc, argv, prefix,
			     builtin_rebase_options,
			     builtin_rebase_usage, 0);

	if (argc > 2)
		usage_with_options(builtin_rebase_usage,
				   builtin_rebase_options);

	if (!(options.flags & REBASE_NO_QUIET))
		strbuf_addstr(&options.git_am_opt, " -q");

	switch (options.type) {
	case REBASE_MERGE:
	case REBASE_INTERACTIVE:
	case REBASE_PRESERVE_MERGES:
		options.state_dir = merge_dir();
		break;
	case REBASE_AM:
		options.state_dir = apply_dir();
		break;
	default:
		/* the default rebase backend is `--am` */
		options.type = REBASE_AM;
		options.state_dir = apply_dir();
		break;
	}

	if (!options.root) {
		if (argc < 1)
			die("TODO: handle @{upstream}");
		else {
			options.upstream_name = argv[0];
			argc--;
			argv++;
			if (!strcmp(options.upstream_name, "-"))
				options.upstream_name = "@{-1}";
		}
		options.upstream = peel_committish(options.upstream_name);
		if (!options.upstream)
			die(_("invalid upstream '%s'"), options.upstream_name);
		options.upstream_arg = options.upstream_name;
	} else
		die("TODO: upstream for --root");

	/* Make sure the branch to rebase onto is valid. */
	if (!options.onto_name)
		options.onto_name = options.upstream_name;
	if (strstr(options.onto_name, "...")) {
		if (get_oid_mb(options.onto_name, &merge_base) < 0)
			die(_("'%s': need exactly one merge base"),
			    options.onto_name);
		options.onto = lookup_commit_or_die(&merge_base,
						    options.onto_name);
	} else {
		options.onto = peel_committish(options.onto_name);
		if (!options.onto)
			die(_("Does not point to a valid commit '%s'"),
				options.onto_name);
	}

	/*
	 * If the branch to rebase is given, that is the branch we will rebase
	 * branch_name -- branch/commit being rebased, or
	 * 		  HEAD (already detached)
	 * orig_head -- commit object name of tip of the branch before rebasing
	 * head_name -- refs/heads/<that-branch> or "detached HEAD"
	 */
	if (argc > 0)
		 die("TODO: handle switch_to");
	else {
		/* Do not need to switch branches, we are already on it. */
		options.head_name =
			xstrdup_or_null(resolve_ref_unsafe("HEAD", 0, NULL,
					 &flags));
		if (!options.head_name)
			die(_("No such ref: %s"), "HEAD");
		if (flags & REF_ISSYMREF) {
			if (!skip_prefix(options.head_name,
					 "refs/heads/", &branch_name))
				branch_name = options.head_name;

		} else {
			options.head_name = xstrdup("detached HEAD");
			branch_name = "HEAD";
		}
		if (get_oid("HEAD", &options.orig_head))
			die(_("Could not resolve HEAD to a revision"));
	}

	/* If a hook exists, give it a chance to interrupt*/
	if (!ok_to_skip_pre_rebase &&
	    run_hook_le(NULL, "pre-rebase", options.upstream_arg,
			argc ? argv[0] : NULL, NULL))
		die(_("The pre-rebase hook refused to rebase."));

	if (options.flags & REBASE_DIFFSTAT) {
		struct diff_options opts;

		if (options.flags & REBASE_VERBOSE)
			printf(_("Changes from %s to %s:\n"),
				oid_to_hex(&merge_base),
				oid_to_hex(&options.onto->object.oid));

		/* We want color (if set), but no pager */
		diff_setup(&opts);
		opts.stat_width = -1; /* use full terminal width */
		opts.stat_graph_width = -1; /* respect statGraphWidth config */
		opts.output_format |=
			DIFF_FORMAT_SUMMARY | DIFF_FORMAT_DIFFSTAT;
		opts.detect_rename = DIFF_DETECT_RENAME;
		diff_setup_done(&opts);
		diff_tree_oid(&merge_base, &options.onto->object.oid,
			      "", &opts);
		diffcore_std(&opts);
		diff_flush(&opts);
	}

	/* Detach HEAD and reset the tree */
	if (options.flags & REBASE_NO_QUIET)
		printf(_("First, rewinding head to replay your work on top of "
			 "it...\n"));

	strbuf_addf(&msg, "rebase: checkout %s", options.onto_name);
	if (reset_head(&options.onto->object.oid, "checkout", NULL, 1))
		die(_("Could not detach HEAD"));
	strbuf_release(&msg);

	strbuf_addf(&revisions, "%s..%s",
		    options.root ? oid_to_hex(&options.onto->object.oid) :
		    (options.restrict_revision ?
		     oid_to_hex(&options.restrict_revision->object.oid) :
		     oid_to_hex(&options.upstream->object.oid)),
		    oid_to_hex(&options.orig_head));

	options.revisions = revisions.buf;

	ret = !!run_specific_rebase(&options);

	strbuf_release(&revisions);
	free(options.head_name);
	return ret;
}
