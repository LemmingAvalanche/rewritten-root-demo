/*
 * git gc builtin command
 *
 * Cleanup unreachable files and optimize the repository.
 *
 * Copyright (c) 2007 James Bowes
 *
 * Based on git-gc.sh, which is
 *
 * Copyright (c) 2006 Shawn O. Pearce
 */

#include "builtin.h"
#include "cache.h"
#include "parse-options.h"
#include "run-command.h"

#define FAILED_RUN "failed to run %s"

static const char * const builtin_gc_usage[] = {
	"git-gc [options]",
	NULL
};

static int pack_refs = 1;
static int aggressive_window = -1;
static int gc_auto_threshold = 6700;
static int gc_auto_pack_limit = 20;
static char *prune_expire = "2.weeks.ago";

#define MAX_ADD 10
static const char *argv_pack_refs[] = {"pack-refs", "--all", "--prune", NULL};
static const char *argv_reflog[] = {"reflog", "expire", "--all", NULL};
static const char *argv_repack[MAX_ADD] = {"repack", "-d", "-l", NULL};
static const char *argv_prune[] = {"prune", "--expire", NULL, NULL};
static const char *argv_rerere[] = {"rerere", "gc", NULL};

static int gc_config(const char *var, const char *value)
{
	if (!strcmp(var, "gc.packrefs")) {
		if (value && !strcmp(value, "notbare"))
			pack_refs = -1;
		else
			pack_refs = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "gc.aggressivewindow")) {
		aggressive_window = git_config_int(var, value);
		return 0;
	}
	if (!strcmp(var, "gc.auto")) {
		gc_auto_threshold = git_config_int(var, value);
		return 0;
	}
	if (!strcmp(var, "gc.autopacklimit")) {
		gc_auto_pack_limit = git_config_int(var, value);
		return 0;
	}
	if (!strcmp(var, "gc.pruneexpire")) {
		if (!value)
			return config_error_nonbool(var);
		if (strcmp(value, "now")) {
			unsigned long now = approxidate("now");
			if (approxidate(value) >= now)
				return error("Invalid %s: '%s'", var, value);
		}
		prune_expire = xstrdup(value);
		return 0;
	}
	return git_default_config(var, value);
}

static void append_option(const char **cmd, const char *opt, int max_length)
{
	int i;

	for (i = 0; cmd[i]; i++)
		;

	if (i + 2 >= max_length)
		die("Too many options specified");
	cmd[i++] = opt;
	cmd[i] = NULL;
}

static int too_many_loose_objects(void)
{
	/*
	 * Quickly check if a "gc" is needed, by estimating how
	 * many loose objects there are.  Because SHA-1 is evenly
	 * distributed, we can check only one and get a reasonable
	 * estimate.
	 */
	char path[PATH_MAX];
	const char *objdir = get_object_directory();
	DIR *dir;
	struct dirent *ent;
	int auto_threshold;
	int num_loose = 0;
	int needed = 0;

	if (gc_auto_threshold <= 0)
		return 0;

	if (sizeof(path) <= snprintf(path, sizeof(path), "%s/17", objdir)) {
		warning("insanely long object directory %.*s", 50, objdir);
		return 0;
	}
	dir = opendir(path);
	if (!dir)
		return 0;

	auto_threshold = (gc_auto_threshold + 255) / 256;
	while ((ent = readdir(dir)) != NULL) {
		if (strspn(ent->d_name, "0123456789abcdef") != 38 ||
		    ent->d_name[38] != '\0')
			continue;
		if (++num_loose > auto_threshold) {
			needed = 1;
			break;
		}
	}
	closedir(dir);
	return needed;
}

static int too_many_packs(void)
{
	struct packed_git *p;
	int cnt;

	if (gc_auto_pack_limit <= 0)
		return 0;

	prepare_packed_git();
	for (cnt = 0, p = packed_git; p; p = p->next) {
		char path[PATH_MAX];
		size_t len;
		int keep;

		if (!p->pack_local)
			continue;
		len = strlen(p->pack_name);
		if (PATH_MAX <= len + 1)
			continue; /* oops, give up */
		memcpy(path, p->pack_name, len-5);
		memcpy(path + len - 5, ".keep", 6);
		keep = access(p->pack_name, F_OK) && (errno == ENOENT);
		if (keep)
			continue;
		/*
		 * Perhaps check the size of the pack and count only
		 * very small ones here?
		 */
		cnt++;
	}
	return gc_auto_pack_limit <= cnt;
}

static int need_to_gc(void)
{
	/*
	 * Setting gc.auto and gc.autopacklimit to 0 or negative can
	 * disable the automatic gc.
	 */
	if (gc_auto_threshold <= 0 && gc_auto_pack_limit <= 0)
		return 0;

	/*
	 * If there are too many loose objects, but not too many
	 * packs, we run "repack -d -l".  If there are too many packs,
	 * we run "repack -A -d -l".  Otherwise we tell the caller
	 * there is no need.
	 */
	if (too_many_packs())
		append_option(argv_repack, "-A", MAX_ADD);
	else if (!too_many_loose_objects())
		return 0;
	return 1;
}

int cmd_gc(int argc, const char **argv, const char *prefix)
{
	int prune = 0;
	int aggressive = 0;
	int auto_gc = 0;
	int quiet = 0;
	char buf[80];

	struct option builtin_gc_options[] = {
		OPT_BOOLEAN(0, "prune", &prune, "prune unreferenced objects"),
		OPT_BOOLEAN(0, "aggressive", &aggressive, "be more thorough (increased runtime)"),
		OPT_BOOLEAN(0, "auto", &auto_gc, "enable auto-gc mode"),
		OPT_BOOLEAN('q', "quiet", &quiet, "suppress progress reports"),
		OPT_END()
	};

	git_config(gc_config);

	if (pack_refs < 0)
		pack_refs = !is_bare_repository();

	argc = parse_options(argc, argv, builtin_gc_options, builtin_gc_usage, 0);
	if (argc > 0)
		usage_with_options(builtin_gc_usage, builtin_gc_options);

	if (aggressive) {
		append_option(argv_repack, "-f", MAX_ADD);
		if (aggressive_window > 0) {
			sprintf(buf, "--window=%d", aggressive_window);
			append_option(argv_repack, buf, MAX_ADD);
		}
	}
	if (quiet)
		append_option(argv_repack, "-q", MAX_ADD);

	if (auto_gc) {
		/*
		 * Auto-gc should be least intrusive as possible.
		 */
		prune = 0;
		if (!need_to_gc())
			return 0;
		fprintf(stderr, "Auto packing your repository for optimum "
			"performance. You may also\n"
			"run \"git gc\" manually. See "
			"\"git help gc\" for more information.\n");
	} else {
		/*
		 * Use safer (for shared repos) "-A" option to
		 * repack when not pruning. Auto-gc makes its
		 * own decision.
		 */
		if (prune)
			append_option(argv_repack, "-a", MAX_ADD);
		else
			append_option(argv_repack, "-A", MAX_ADD);
	}

	if (pack_refs && run_command_v_opt(argv_pack_refs, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_pack_refs[0]);

	if (run_command_v_opt(argv_reflog, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_reflog[0]);

	if (run_command_v_opt(argv_repack, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_repack[0]);

	argv_prune[2] = prune_expire;
	if (run_command_v_opt(argv_prune, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_prune[0]);

	if (run_command_v_opt(argv_rerere, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_rerere[0]);

	if (auto_gc && too_many_loose_objects())
		warning("There are too many unreachable loose objects; "
			"run 'git prune' to remove them.");

	return 0;
}
