#include "cache.h"
#include "diff.h"
#include "commit.h"

static int show_root_diff = 0;
static int verbose_header = 0;
static int ignore_merges = 1;
static int read_stdin = 0;

static const char *header = NULL;
static const char *header_prefix = "";
static enum cmit_fmt commit_format = CMIT_FMT_RAW;

static struct diff_options diff_options;

static void call_diff_setup_done(void)
{
	diff_setup_done(&diff_options);
}

static int call_diff_flush(void)
{
	diffcore_std(&diff_options);
	if (diff_queue_is_empty()) {
		int saved_fmt = diff_options.output_format;
		diff_options.output_format = DIFF_FORMAT_NO_OUTPUT;
		diff_flush(&diff_options);
		diff_options.output_format = saved_fmt;
		return 0;
	}
	if (header) {
		printf("%s%c", header, diff_options.line_termination);
		header = NULL;
	}
	diff_flush(&diff_options);
	return 1;
}

static int diff_tree_sha1_top(const unsigned char *old,
			      const unsigned char *new, const char *base)
{
	int ret;

	call_diff_setup_done();
	ret = diff_tree_sha1(old, new, base, &diff_options);
	call_diff_flush();
	return ret;
}

static int diff_root_tree(const unsigned char *new, const char *base)
{
	int retval;
	void *tree;
	struct tree_desc empty, real;

	call_diff_setup_done();
	tree = read_object_with_reference(new, "tree", &real.size, NULL);
	if (!tree)
		die("unable to read root tree (%s)", sha1_to_hex(new));
	real.buf = tree;

	empty.buf = "";
	empty.size = 0;
	retval = diff_tree(&empty, &real, base, &diff_options);
	free(tree);
	call_diff_flush();
	return retval;
}

static const char *generate_header(const char *commit, const char *parent, const char *msg, unsigned long len)
{
	static char this_header[16384];
	int offset;

	if (!verbose_header)
		return commit;

	offset = sprintf(this_header, "%s%s (from %s)\n", header_prefix, commit, parent);
	offset += pretty_print_commit(commit_format, msg, len, this_header + offset, sizeof(this_header) - offset);
	return this_header;
}

static int diff_tree_commit(const unsigned char *commit, const char *name)
{
	unsigned long size, offset;
	char *buf = read_object_with_reference(commit, "commit", &size, NULL);

	if (!buf)
		return -1;

	if (!name) {
		static char commit_name[60];
		strcpy(commit_name, sha1_to_hex(commit));
		name = commit_name;
	}

	/* Root commit? */
	if (show_root_diff && memcmp(buf + 46, "parent ", 7)) {
		header = generate_header(name, "root", buf, size);
		diff_root_tree(commit, "");
	}

	/* More than one parent? */
	if (ignore_merges) {
		if (!memcmp(buf + 46 + 48, "parent ", 7))
			return 0;
	}

	offset = 46;
	while (offset + 48 < size && !memcmp(buf + offset, "parent ", 7)) {
		unsigned char parent[20];
		if (get_sha1_hex(buf + offset + 7, parent))
			return -1;
		header = generate_header(name, sha1_to_hex(parent), buf, size);
		diff_tree_sha1_top(parent, commit, "");
		if (!header && verbose_header) {
			header_prefix = "\ndiff-tree ";
			/*
			 * Don't print multiple merge entries if we
			 * don't print the diffs.
			 */
		}
		offset += 48;
	}
	free(buf);
	return 0;
}

static int diff_tree_stdin(char *line)
{
	int len = strlen(line);
	unsigned char commit[20], parent[20];
	static char this_header[1000];

	if (!len || line[len-1] != '\n')
		return -1;
	line[len-1] = 0;
	if (get_sha1_hex(line, commit))
		return -1;
	if (isspace(line[40]) && !get_sha1_hex(line+41, parent)) {
		line[40] = 0;
		line[81] = 0;
		sprintf(this_header, "%s (from %s)\n", line, line+41);
		header = this_header;
		return diff_tree_sha1_top(parent, commit, "");
	}
	line[40] = 0;
	return diff_tree_commit(commit, line);
}

static const char diff_tree_usage[] =
"git-diff-tree [--stdin] [-m] [-s] [-v] [--pretty] [-t] "
"[<common diff options>] <tree-ish> <tree-ish>"
COMMON_DIFF_OPTIONS_HELP;

int main(int argc, const char **argv)
{
	int nr_sha1;
	char line[1000];
	unsigned char sha1[2][20];
	const char *prefix = setup_git_directory();

	git_config(git_default_config);
	nr_sha1 = 0;
	diff_setup(&diff_options);

	for (;;) {
		int diff_opt_cnt;
		const char *arg;

		argv++;
		argc--;
		arg = *argv;
		if (!arg)
			break;

		if (*arg != '-') {
			if (nr_sha1 < 2 && !get_sha1(arg, sha1[nr_sha1])) {
				nr_sha1++;
				continue;
			}
			break;
		}

		diff_opt_cnt = diff_opt_parse(&diff_options, argv, argc);
		if (diff_opt_cnt < 0)
			usage(diff_tree_usage);
		else if (diff_opt_cnt) {
			argv += diff_opt_cnt - 1;
			argc -= diff_opt_cnt - 1;
			continue;
		}


		if (!strcmp(arg, "--")) {
			argv++;
			argc--;
			break;
		}
		if (!strcmp(arg, "-r")) {
			diff_options.recursive = 1;
			continue;
		}
		if (!strcmp(arg, "-t")) {
			diff_options.recursive = 1;
			diff_options.tree_in_recursive = 1;
			continue;
		}
		if (!strcmp(arg, "-m")) {
			ignore_merges = 0;
			continue;
		}
		if (!strcmp(arg, "-v")) {
			verbose_header = 1;
			header_prefix = "diff-tree ";
			continue;
		}
		if (!strncmp(arg, "--pretty", 8)) {
			verbose_header = 1;
			header_prefix = "diff-tree ";
			commit_format = get_commit_format(arg+8);
			continue;
		}
		if (!strcmp(arg, "--stdin")) {
			read_stdin = 1;
			continue;
		}
		if (!strcmp(arg, "--root")) {
			show_root_diff = 1;
			continue;
		}
		usage(diff_tree_usage);
	}
	if (diff_options.output_format == DIFF_FORMAT_PATCH)
		diff_options.recursive = 1;

	diff_tree_setup_paths(get_pathspec(prefix, argv));

	switch (nr_sha1) {
	case 0:
		if (!read_stdin)
			usage(diff_tree_usage);
		break;
	case 1:
		diff_tree_commit(sha1[0], NULL);
		break;
	case 2:
		diff_tree_sha1_top(sha1[0], sha1[1], "");
		break;
	}

	if (!read_stdin)
		return 0;

	if (diff_options.detect_rename)
		diff_options.setup |= (DIFF_SETUP_USE_SIZE_CACHE |
				       DIFF_SETUP_USE_CACHE);
	while (fgets(line, sizeof(line), stdin))
		diff_tree_stdin(line);

	return 0;
}
