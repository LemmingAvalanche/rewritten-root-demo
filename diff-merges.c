#include "diff-merges.h"

#include "revision.h"

void init_diff_merge_revs(struct rev_info *revs)
{
	revs->ignore_merges = -1;
}

int parse_diff_merge_opts(struct rev_info *revs, const char **argv)
{
	int argcount = 1;
	const char *optarg;
	const char *arg = argv[0];

	if (!strcmp(arg, "-m")) {
		/*
		 * To "diff-index", "-m" means "match missing", and to the "log"
		 * family of commands, it means "show full diff for merges". Set
		 * both fields appropriately.
		 */
		revs->ignore_merges = 0;
		revs->match_missing = 1;
	} else if (!strcmp(arg, "-c")) {
		revs->diff = 1;
		revs->dense_combined_merges = 0;
		revs->combine_merges = 1;
	} else if (!strcmp(arg, "--cc")) {
		revs->diff = 1;
		revs->dense_combined_merges = 1;
		revs->combine_merges = 1;
	} else if (!strcmp(arg, "--no-diff-merges")) {
		revs->ignore_merges = 1;
	} else if (!strcmp(arg, "--combined-all-paths")) {
		revs->diff = 1;
		revs->combined_all_paths = 1;
	} else if ((argcount = parse_long_opt("diff-merges", argv, &optarg))) {
		if (!strcmp(optarg, "off")) {
			revs->ignore_merges = 1;
		} else {
			die(_("unknown value for --diff-merges: %s"), optarg);
		}
	} else
		argcount = 0;

	return argcount;
}

void setup_diff_merges_revs(struct rev_info *revs)
{
	if (revs->combine_merges && revs->ignore_merges < 0)
		revs->ignore_merges = 0;
	if (revs->ignore_merges < 0)
		revs->ignore_merges = 1;
	if (revs->combined_all_paths && !revs->combine_merges)
		die("--combined-all-paths makes no sense without -c or --cc");
}

void rev_diff_merges_first_parent_defaults_to_enable(struct rev_info *revs)
{
	if (revs->first_parent_only && revs->ignore_merges < 0)
		revs->ignore_merges = 0;
}

void rev_diff_merges_default_to_dense_combined(struct rev_info *revs)
{
	if (revs->ignore_merges < 0) {
		/* There was no "-m" variant on the command line */
		revs->ignore_merges = 0;
		if (!revs->first_parent_only && !revs->combine_merges) {
			/* No "--first-parent", "-c", or "--cc" */
			revs->combine_merges = 1;
			revs->dense_combined_merges = 1;
		}
	}
}
