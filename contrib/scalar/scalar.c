/*
 * The Scalar command-line interface.
 */

#include "cache.h"
#include "gettext.h"
#include "parse-options.h"
#include "config.h"
#include "run-command.h"
#include "refs.h"

/*
 * Remove the deepest subdirectory in the provided path string. Path must not
 * include a trailing path separator. Returns 1 if parent directory found,
 * otherwise 0.
 */
static int strbuf_parent_directory(struct strbuf *buf)
{
	size_t len = buf->len;
	size_t offset = offset_1st_component(buf->buf);
	char *path_sep = find_last_dir_sep(buf->buf + offset);
	strbuf_setlen(buf, path_sep ? path_sep - buf->buf : offset);

	return buf->len < len;
}

static void setup_enlistment_directory(int argc, const char **argv,
				       const char * const *usagestr,
				       const struct option *options,
				       struct strbuf *enlistment_root)
{
	struct strbuf path = STRBUF_INIT;
	char *root;
	int enlistment_found = 0;

	if (startup_info->have_repository)
		BUG("gitdir already set up?!?");

	if (argc > 1)
		usage_with_options(usagestr, options);

	/* find the worktree, determine its corresponding root */
	if (argc == 1)
		strbuf_add_absolute_path(&path, argv[0]);
	else if (strbuf_getcwd(&path) < 0)
		die(_("need a working directory"));

	strbuf_trim_trailing_dir_sep(&path);
	do {
		const size_t len = path.len;

		/* check if currently in enlistment root with src/ workdir */
		strbuf_addstr(&path, "/src");
		if (is_nonbare_repository_dir(&path)) {
			if (enlistment_root)
				strbuf_add(enlistment_root, path.buf, len);

			enlistment_found = 1;
			break;
		}

		/* reset to original path */
		strbuf_setlen(&path, len);

		/* check if currently in workdir */
		if (is_nonbare_repository_dir(&path)) {
			if (enlistment_root) {
				/*
				 * If the worktree's directory's name is `src`, the enlistment is the
				 * parent directory, otherwise it is identical to the worktree.
				 */
				root = strip_path_suffix(path.buf, "src");
				strbuf_addstr(enlistment_root, root ? root : path.buf);
				free(root);
			}

			enlistment_found = 1;
			break;
		}
	} while (strbuf_parent_directory(&path));

	if (!enlistment_found)
		die(_("could not find enlistment root"));

	if (chdir(path.buf) < 0)
		die_errno(_("could not switch to '%s'"), path.buf);

	strbuf_release(&path);
	setup_git_directory();
}

static int run_git(const char *arg, ...)
{
	struct strvec argv = STRVEC_INIT;
	va_list args;
	const char *p;
	int res;

	va_start(args, arg);
	strvec_push(&argv, arg);
	while ((p = va_arg(args, const char *)))
		strvec_push(&argv, p);
	va_end(args);

	res = run_command_v_opt(argv.v, RUN_GIT_CMD);

	strvec_clear(&argv);
	return res;
}

static int set_recommended_config(void)
{
	struct {
		const char *key;
		const char *value;
	} config[] = {
		{ "am.keepCR", "true" },
		{ "core.FSCache", "true" },
		{ "core.multiPackIndex", "true" },
		{ "core.preloadIndex", "true" },
#ifndef WIN32
		{ "core.untrackedCache", "true" },
#else
		/*
		 * Unfortunately, Scalar's Functional Tests demonstrated
		 * that the untracked cache feature is unreliable on Windows
		 * (which is a bummer because that platform would benefit the
		 * most from it). For some reason, freshly created files seem
		 * not to update the directory's `lastModified` time
		 * immediately, but the untracked cache would need to rely on
		 * that.
		 *
		 * Therefore, with a sad heart, we disable this very useful
		 * feature on Windows.
		 */
		{ "core.untrackedCache", "false" },
#endif
		{ "core.logAllRefUpdates", "true" },
		{ "credential.https://dev.azure.com.useHttpPath", "true" },
		{ "credential.validate", "false" }, /* GCM4W-only */
		{ "gc.auto", "0" },
		{ "gui.GCWarning", "false" },
		{ "index.threads", "true" },
		{ "index.version", "4" },
		{ "merge.stat", "false" },
		{ "merge.renames", "true" },
		{ "pack.useBitmaps", "false" },
		{ "pack.useSparse", "true" },
		{ "receive.autoGC", "false" },
		{ "reset.quiet", "true" },
		{ "feature.manyFiles", "false" },
		{ "feature.experimental", "false" },
		{ "fetch.unpackLimit", "1" },
		{ "fetch.writeCommitGraph", "false" },
#ifdef WIN32
		{ "http.sslBackend", "schannel" },
#endif
		{ "status.aheadBehind", "false" },
		{ "commitGraph.generationVersion", "1" },
		{ "core.autoCRLF", "false" },
		{ "core.safeCRLF", "false" },
		{ "fetch.showForcedUpdates", "false" },
		{ NULL, NULL },
	};
	int i;
	char *value;

	for (i = 0; config[i].key; i++) {
		if (git_config_get_string(config[i].key, &value)) {
			trace2_data_string("scalar", the_repository, config[i].key, "created");
			if (git_config_set_gently(config[i].key,
						  config[i].value) < 0)
				return error(_("could not configure %s=%s"),
					     config[i].key, config[i].value);
		} else {
			trace2_data_string("scalar", the_repository, config[i].key, "exists");
			free(value);
		}
	}

	/*
	 * The `log.excludeDecoration` setting is special because it allows
	 * for multiple values.
	 */
	if (git_config_get_string("log.excludeDecoration", &value)) {
		trace2_data_string("scalar", the_repository,
				   "log.excludeDecoration", "created");
		if (git_config_set_multivar_gently("log.excludeDecoration",
						   "refs/prefetch/*",
						   CONFIG_REGEX_NONE, 0))
			return error(_("could not configure "
				       "log.excludeDecoration"));
	} else {
		trace2_data_string("scalar", the_repository,
				   "log.excludeDecoration", "exists");
		free(value);
	}

	return 0;
}

static int toggle_maintenance(int enable)
{
	return run_git("maintenance", enable ? "start" : "unregister", NULL);
}

static int add_or_remove_enlistment(int add)
{
	int res;

	if (!the_repository->worktree)
		die(_("Scalar enlistments require a worktree"));

	res = run_git("config", "--global", "--get", "--fixed-value",
		      "scalar.repo", the_repository->worktree, NULL);

	/*
	 * If we want to add and the setting is already there, then do nothing.
	 * If we want to remove and the setting is not there, then do nothing.
	 */
	if ((add && !res) || (!add && res))
		return 0;

	return run_git("config", "--global", add ? "--add" : "--unset",
		       add ? "--no-fixed-value" : "--fixed-value",
		       "scalar.repo", the_repository->worktree, NULL);
}

static int register_dir(void)
{
	int res = add_or_remove_enlistment(1);

	if (!res)
		res = set_recommended_config();

	if (!res)
		res = toggle_maintenance(1);

	return res;
}

static int unregister_dir(void)
{
	int res = 0;

	if (toggle_maintenance(0) < 0)
		res = -1;

	if (add_or_remove_enlistment(0) < 0)
		res = -1;

	return res;
}

/* printf-style interface, expects `<key>=<value>` argument */
static int set_config(const char *fmt, ...)
{
	struct strbuf buf = STRBUF_INIT;
	char *value;
	int res;
	va_list args;

	va_start(args, fmt);
	strbuf_vaddf(&buf, fmt, args);
	va_end(args);

	value = strchr(buf.buf, '=');
	if (value)
		*(value++) = '\0';
	res = git_config_set_gently(buf.buf, value);
	strbuf_release(&buf);

	return res;
}

static char *remote_default_branch(const char *url)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT;

	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "ls-remote", "--symref", url, "HEAD", NULL);
	if (!pipe_command(&cp, NULL, 0, &out, 0, NULL, 0)) {
		const char *line = out.buf;

		while (*line) {
			const char *eol = strchrnul(line, '\n'), *p;
			size_t len = eol - line;
			char *branch;

			if (!skip_prefix(line, "ref: ", &p) ||
			    !strip_suffix_mem(line, &len, "\tHEAD")) {
				line = eol + (*eol == '\n');
				continue;
			}

			eol = line + len;
			if (skip_prefix(p, "refs/heads/", &p)) {
				branch = xstrndup(p, eol - p);
				strbuf_release(&out);
				return branch;
			}

			error(_("remote HEAD is not a branch: '%.*s'"),
			      (int)(eol - p), p);
			strbuf_release(&out);
			return NULL;
		}
	}
	warning(_("failed to get default branch name from remote; "
		  "using local default"));
	strbuf_reset(&out);

	child_process_init(&cp);
	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "symbolic-ref", "--short", "HEAD", NULL);
	if (!pipe_command(&cp, NULL, 0, &out, 0, NULL, 0)) {
		strbuf_trim(&out);
		return strbuf_detach(&out, NULL);
	}

	strbuf_release(&out);
	error(_("failed to get default branch name"));
	return NULL;
}

static int cmd_clone(int argc, const char **argv)
{
	const char *branch = NULL;
	int full_clone = 0;
	struct option clone_options[] = {
		OPT_STRING('b', "branch", &branch, N_("<branch>"),
			   N_("branch to checkout after clone")),
		OPT_BOOL(0, "full-clone", &full_clone,
			 N_("when cloning, create full working directory")),
		OPT_END(),
	};
	const char * const clone_usage[] = {
		N_("scalar clone [<options>] [--] <repo> [<dir>]"),
		NULL
	};
	const char *url;
	char *enlistment = NULL, *dir = NULL;
	struct strbuf buf = STRBUF_INIT;
	int res;

	argc = parse_options(argc, argv, NULL, clone_options, clone_usage, 0);

	if (argc == 2) {
		url = argv[0];
		enlistment = xstrdup(argv[1]);
	} else if (argc == 1) {
		url = argv[0];

		strbuf_addstr(&buf, url);
		/* Strip trailing slashes, if any */
		while (buf.len > 0 && is_dir_sep(buf.buf[buf.len - 1]))
			strbuf_setlen(&buf, buf.len - 1);
		/* Strip suffix `.git`, if any */
		strbuf_strip_suffix(&buf, ".git");

		enlistment = find_last_dir_sep(buf.buf);
		if (!enlistment) {
			die(_("cannot deduce worktree name from '%s'"), url);
		}
		enlistment = xstrdup(enlistment + 1);
	} else {
		usage_msg_opt(_("You must specify a repository to clone."),
			      clone_usage, clone_options);
	}

	if (is_directory(enlistment))
		die(_("directory '%s' exists already"), enlistment);

	dir = xstrfmt("%s/src", enlistment);

	strbuf_reset(&buf);
	if (branch)
		strbuf_addf(&buf, "init.defaultBranch=%s", branch);
	else {
		char *b = repo_default_branch_name(the_repository, 1);
		strbuf_addf(&buf, "init.defaultBranch=%s", b);
		free(b);
	}

	if ((res = run_git("-c", buf.buf, "init", "--", dir, NULL)))
		goto cleanup;

	if (chdir(dir) < 0) {
		res = error_errno(_("could not switch to '%s'"), dir);
		goto cleanup;
	}

	setup_git_directory();

	/* common-main already logs `argv` */
	trace2_def_repo(the_repository);

	if (!branch && !(branch = remote_default_branch(url))) {
		res = error(_("failed to get default branch for '%s'"), url);
		goto cleanup;
	}

	if (set_config("remote.origin.url=%s", url) ||
	    set_config("remote.origin.fetch="
		       "+refs/heads/*:refs/remotes/origin/*") ||
	    set_config("remote.origin.promisor=true") ||
	    set_config("remote.origin.partialCloneFilter=blob:none")) {
		res = error(_("could not configure remote in '%s'"), dir);
		goto cleanup;
	}

	if (!full_clone &&
	    (res = run_git("sparse-checkout", "init", "--cone", NULL)))
		goto cleanup;

	if (set_recommended_config())
		return error(_("could not configure '%s'"), dir);

	if ((res = run_git("fetch", "--quiet", "origin", NULL))) {
		warning(_("partial clone failed; attempting full clone"));

		if (set_config("remote.origin.promisor") ||
		    set_config("remote.origin.partialCloneFilter")) {
			res = error(_("could not configure for full clone"));
			goto cleanup;
		}

		if ((res = run_git("fetch", "--quiet", "origin", NULL)))
			goto cleanup;
	}

	if ((res = set_config("branch.%s.remote=origin", branch)))
		goto cleanup;
	if ((res = set_config("branch.%s.merge=refs/heads/%s",
			      branch, branch)))
		goto cleanup;

	strbuf_reset(&buf);
	strbuf_addf(&buf, "origin/%s", branch);
	res = run_git("checkout", "-f", "-t", buf.buf, NULL);
	if (res)
		goto cleanup;

	res = register_dir();

cleanup:
	free(enlistment);
	free(dir);
	strbuf_release(&buf);
	return res;
}

static int cmd_list(int argc, const char **argv)
{
	if (argc != 1)
		die(_("`scalar list` does not take arguments"));

	if (run_git("config", "--global", "--get-all", "scalar.repo", NULL) < 0)
		return -1;
	return 0;
}

static int cmd_register(int argc, const char **argv)
{
	struct option options[] = {
		OPT_END(),
	};
	const char * const usage[] = {
		N_("scalar register [<enlistment>]"),
		NULL
	};

	argc = parse_options(argc, argv, NULL, options,
			     usage, 0);

	setup_enlistment_directory(argc, argv, usage, options, NULL);

	return register_dir();
}

static int remove_deleted_enlistment(struct strbuf *path)
{
	int res = 0;
	strbuf_realpath_forgiving(path, path->buf, 1);

	if (run_git("config", "--global",
		    "--unset", "--fixed-value",
		    "scalar.repo", path->buf, NULL) < 0)
		res = -1;

	if (run_git("config", "--global",
		    "--unset", "--fixed-value",
		    "maintenance.repo", path->buf, NULL) < 0)
		res = -1;

	return res;
}

static int cmd_unregister(int argc, const char **argv)
{
	struct option options[] = {
		OPT_END(),
	};
	const char * const usage[] = {
		N_("scalar unregister [<enlistment>]"),
		NULL
	};

	argc = parse_options(argc, argv, NULL, options,
			     usage, 0);

	/*
	 * Be forgiving when the enlistment or worktree does not even exist any
	 * longer; This can be the case if a user deleted the worktree by
	 * mistake and _still_ wants to unregister the thing.
	 */
	if (argc == 1) {
		struct strbuf src_path = STRBUF_INIT, workdir_path = STRBUF_INIT;

		strbuf_addf(&src_path, "%s/src/.git", argv[0]);
		strbuf_addf(&workdir_path, "%s/.git", argv[0]);
		if (!is_directory(src_path.buf) && !is_directory(workdir_path.buf)) {
			/* remove possible matching registrations */
			int res = -1;

			strbuf_strip_suffix(&src_path, "/.git");
			res = remove_deleted_enlistment(&src_path) && res;

			strbuf_strip_suffix(&workdir_path, "/.git");
			res = remove_deleted_enlistment(&workdir_path) && res;

			strbuf_release(&src_path);
			strbuf_release(&workdir_path);
			return res;
		}
		strbuf_release(&src_path);
		strbuf_release(&workdir_path);
	}

	setup_enlistment_directory(argc, argv, usage, options, NULL);

	return unregister_dir();
}

static struct {
	const char *name;
	int (*fn)(int, const char **);
} builtins[] = {
	{ "clone", cmd_clone },
	{ "list", cmd_list },
	{ "register", cmd_register },
	{ "unregister", cmd_unregister },
	{ NULL, NULL},
};

int cmd_main(int argc, const char **argv)
{
	struct strbuf scalar_usage = STRBUF_INIT;
	int i;

	if (argc > 1) {
		argv++;
		argc--;

		for (i = 0; builtins[i].name; i++)
			if (!strcmp(builtins[i].name, argv[0]))
				return !!builtins[i].fn(argc, argv);
	}

	strbuf_addstr(&scalar_usage,
		      N_("scalar <command> [<options>]\n\nCommands:\n"));
	for (i = 0; builtins[i].name; i++)
		strbuf_addf(&scalar_usage, "\t%s\n", builtins[i].name);

	usage(scalar_usage.buf);
}
