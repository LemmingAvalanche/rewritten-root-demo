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
#include "run-command.h"

#define FAILED_RUN "failed to run %s"

static const char builtin_gc_usage[] = "git-gc [--prune] [--aggressive]";

static int pack_refs = 1;
static int aggressive_window = -1;
static int gc_auto_threshold = 6700;

#define MAX_ADD 10
static const char *argv_pack_refs[] = {"pack-refs", "--all", "--prune", NULL};
static const char *argv_reflog[] = {"reflog", "expire", "--all", NULL};
static const char *argv_repack[MAX_ADD] = {"repack", "-a", "-d", "-l", NULL};
static const char *argv_prune[] = {"prune", NULL};
static const char *argv_rerere[] = {"rerere", "gc", NULL};

static const char *argv_repack_auto[] = {"repack", "-d", "-l", NULL};

static int gc_config(const char *var, const char *value)
{
	if (!strcmp(var, "gc.packrefs")) {
		if (!strcmp(value, "notbare"))
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

static int need_to_gc(void)
{
	/*
	 * Setting gc.auto to 0 or negative can disable the
	 * automatic gc
	 */
	if (gc_auto_threshold <= 0)
		return 0;

	return too_many_loose_objects();
}

int cmd_gc(int argc, const char **argv, const char *prefix)
{
	int i;
	int prune = 0;
	int auto_gc = 0;
	char buf[80];

	git_config(gc_config);

	if (pack_refs < 0)
		pack_refs = !is_bare_repository();

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (!strcmp(arg, "--prune")) {
			prune = 1;
			continue;
		}
		if (!strcmp(arg, "--aggressive")) {
			append_option(argv_repack, "-f", MAX_ADD);
			if (aggressive_window > 0) {
				sprintf(buf, "--window=%d", aggressive_window);
				append_option(argv_repack, buf, MAX_ADD);
			}
			continue;
		}
		if (!strcmp(arg, "--auto")) {
			auto_gc = 1;
			continue;
		}
		break;
	}
	if (i != argc)
		usage(builtin_gc_usage);

	if (auto_gc) {
		/*
		 * Auto-gc should be least intrusive as possible.
		 */
		prune = 0;
		for (i = 0; i < ARRAY_SIZE(argv_repack_auto); i++)
			argv_repack[i] = argv_repack_auto[i];
		if (!need_to_gc())
			return 0;
	}

	if (pack_refs && run_command_v_opt(argv_pack_refs, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_pack_refs[0]);

	if (run_command_v_opt(argv_reflog, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_reflog[0]);

	if (run_command_v_opt(argv_repack, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_repack[0]);

	if (prune && run_command_v_opt(argv_prune, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_prune[0]);

	if (run_command_v_opt(argv_rerere, RUN_GIT_CMD))
		return error(FAILED_RUN, argv_rerere[0]);

	if (auto_gc && too_many_loose_objects())
		warning("There are too many unreachable loose objects; "
			"run 'git prune' to remove them.");

	return 0;
}
