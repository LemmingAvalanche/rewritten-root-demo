
#include "cache.h"
#include "diff.h"
#include "commit.h"
#include "log-tree.h"

void show_log(struct rev_info *opt, struct log_info *log, const char *sep)
{
	static char this_header[16384];
	struct commit *commit = log->commit, *parent = log->parent;
	int abbrev = opt->diffopt.abbrev;
	int abbrev_commit = opt->abbrev_commit ? opt->abbrev : 40;
	int len;

	opt->loginfo = NULL;
	if (!opt->verbose_header) {
		puts(sha1_to_hex(commit->object.sha1));
		return;
	}

	/*
	 * Whitespace between commit messages, unless we are oneline
	 */
	if (opt->shown_one && opt->commit_format != CMIT_FMT_ONELINE)
		putchar('\n');
	opt->shown_one = 1;

	/*
	 * Print header line of header..
	 */
	printf("%s%s",
		opt->commit_format == CMIT_FMT_ONELINE ? "" : "commit ",
		diff_unique_abbrev(commit->object.sha1, abbrev_commit));
	if (parent) 
		printf(" (from %s)", diff_unique_abbrev(parent->object.sha1, abbrev_commit));
	putchar(opt->commit_format == CMIT_FMT_ONELINE ? ' ' : '\n');

	/*
	 * And then the pretty-printed message itself
	 */
	len = pretty_print_commit(opt->commit_format, commit, ~0u, this_header, sizeof(this_header), abbrev);
	printf("%s%s", this_header, sep);
}

int log_tree_diff_flush(struct rev_info *opt)
{
	diffcore_std(&opt->diffopt);

	if (diff_queue_is_empty()) {
		int saved_fmt = opt->diffopt.output_format;
		opt->diffopt.output_format = DIFF_FORMAT_NO_OUTPUT;
		diff_flush(&opt->diffopt);
		opt->diffopt.output_format = saved_fmt;
		return 0;
	}

	if (opt->loginfo && !opt->no_commit_id)
		show_log(opt, opt->loginfo, "\n");
	diff_flush(&opt->diffopt);
	return 1;
}

static int diff_root_tree(struct rev_info *opt,
			  const unsigned char *new, const char *base)
{
	int retval;
	void *tree;
	struct tree_desc empty, real;

	tree = read_object_with_reference(new, tree_type, &real.size, NULL);
	if (!tree)
		die("unable to read root tree (%s)", sha1_to_hex(new));
	real.buf = tree;

	empty.buf = "";
	empty.size = 0;
	retval = diff_tree(&empty, &real, base, &opt->diffopt);
	free(tree);
	log_tree_diff_flush(opt);
	return retval;
}

static int do_diff_combined(struct rev_info *opt, struct commit *commit)
{
	unsigned const char *sha1 = commit->object.sha1;

	diff_tree_combined_merge(sha1, opt->dense_combined_merges, opt);
	return !opt->loginfo;
}

/*
 * Show the diff of a commit.
 *
 * Return true if we printed any log info messages
 */
static int log_tree_diff(struct rev_info *opt, struct commit *commit, struct log_info *log)
{
	int showed_log;
	struct commit_list *parents;
	unsigned const char *sha1 = commit->object.sha1;

	if (!opt->diff)
		return 0;

	/* Root commit? */
	parents = commit->parents;
	if (!parents) {
		if (opt->show_root_diff)
			diff_root_tree(opt, sha1, "");
		return !opt->loginfo;
	}

	/* More than one parent? */
	if (parents && parents->next) {
		if (opt->ignore_merges)
			return 0;
		else if (opt->combine_merges)
			return do_diff_combined(opt, commit);

		/* If we show individual diffs, show the parent info */
		log->parent = parents->item;
	}

	showed_log = 0;
	for (;;) {
		struct commit *parent = parents->item;

		diff_tree_sha1(parent->object.sha1, sha1, "", &opt->diffopt);
		log_tree_diff_flush(opt);

		showed_log |= !opt->loginfo;

		/* Set up the log info for the next parent, if any.. */
		parents = parents->next;
		if (!parents)
			break;
		log->parent = parents->item;
		opt->loginfo = log;
	}
	return showed_log;
}

int log_tree_commit(struct rev_info *opt, struct commit *commit)
{
	struct log_info log;

	log.commit = commit;
	log.parent = NULL;
	opt->loginfo = &log;

	if (!log_tree_diff(opt, commit, &log) && opt->loginfo && opt->always_show_header) {
		log.parent = NULL;
		show_log(opt, opt->loginfo, "");
	}
	opt->loginfo = NULL;
	return 0;
}
