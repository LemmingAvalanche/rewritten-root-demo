#include "diff-merges.h"

#include "revision.h"

static void suppress(struct rev_info *revs)
{
	revs->ignore_merges = 1;
	revs->first_parent_merges = 0;
	revs->combine_merges = 0;
	revs->dense_combined_merges = 0;
}

static void set_dense_combined(struct rev_info *revs)
{
	revs->combine_merges = 1;
	revs->dense_combined_merges = 1;
}


/*
 * Public functions. They are in the order they are called.
 */

void diff_merges_init_revs(struct rev_info *revs)
{
	revs->ignore_merges = -1;
}

int diff_merges_parse_opts(struct rev_info *revs, const char **argv)
{
	int argcount = 1;
	const char *optarg;
	const char *arg = argv[0];

	if (!strcmp(arg, "-m")) {
		suppress(revs);
		/*
		 * To "diff-index", "-m" means "match missing", and to the "log"
		 * family of commands, it means "show full diff for merges". Set
		 * both fields appropriately.
		 */
		revs->ignore_merges = 0;
		revs->match_missing = 1;
	} else if (!strcmp(arg, "-c")) {
		revs->dense_combined_merges = 0;
		revs->combine_merges = 1;
	} else if (!strcmp(arg, "--cc")) {
		set_dense_combined(revs);
	} else if (!strcmp(arg, "--no-diff-merges")) {
		suppress(revs);
	} else if (!strcmp(arg, "--combined-all-paths")) {
		revs->combined_all_paths = 1;
	} else if ((argcount = parse_long_opt("diff-merges", argv, &optarg))) {
		if (!strcmp(optarg, "off")) {
			suppress(revs);
		} else {
			die(_("unknown value for --diff-merges: %s"), optarg);
		}
	} else
		argcount = 0;

	return argcount;
}

void diff_merges_suppress(struct rev_info *revs)
{
	suppress(revs);
}

void diff_merges_default_to_first_parent(struct rev_info *revs)
{
	if (revs->ignore_merges < 0)		/* No -m */
		revs->ignore_merges = 0;
	if (!revs->combine_merges)		/* No -c/--cc" */
		revs->first_parent_merges = 1;
}

void diff_merges_default_to_dense_combined(struct rev_info *revs)
{
	if (revs->ignore_merges < 0) {		/* No -m */
		revs->ignore_merges = 0;
		if (!revs->combine_merges) {	/* No -c/--cc" */
			revs->combine_merges = 1;
			revs->dense_combined_merges = 1;
		}
	}
}

void diff_merges_set_dense_combined_if_unset(struct rev_info *revs)
{
	if (!revs->combine_merges)
		set_dense_combined(revs);
}

void diff_merges_setup_revs(struct rev_info *revs)
{
	if (revs->combine_merges && revs->ignore_merges < 0)
		revs->ignore_merges = 0;
	if (revs->ignore_merges < 0)
		revs->ignore_merges = 1;
	if (revs->combined_all_paths && !revs->combine_merges)
		die("--combined-all-paths makes no sense without -c or --cc");
	if (revs->combine_merges) {
		revs->diff = 1;
		/* Turn --cc/-c into -p --cc/-c when -p was not given */
		if (!revs->diffopt.output_format)
			revs->diffopt.output_format = DIFF_FORMAT_PATCH;
	}
}
