/*
 * Builtin "git grep"
 *
 * Copyright (c) 2006 Junio C Hamano
 */
#include "cache.h"
#include "blob.h"
#include "tree.h"
#include "commit.h"
#include "tag.h"
#include "diff.h"
#include "revision.h"
#include "builtin.h"
#include <regex.h>

static int pathspec_matches(struct diff_options *opt, const char *name)
{
	int i, j;
	int namelen;
	if (!opt->nr_paths)
		return 1;
	namelen = strlen(name);
	for (i = 0; i < opt->nr_paths; i++) {
		const char *match = opt->paths[i];
		int matchlen = opt->pathlens[i];
		if (matchlen <= namelen) {
			if (!strncmp(name, match, matchlen))
				return 1;
			continue;
		}
		/* If name is "Documentation" and pathspec is
		 * "Documentation/", they should match.  Maybe
		 * we would want to strip it in get_pathspec()???
		 */
		if (strncmp(name, match, namelen))
			continue;
		for (j = namelen; j < matchlen; j++)
			if (match[j] != '/')
				break;
		if (matchlen <= j)
			return 1;
	}
	return 0;
}

struct grep_opt {
	const char *pattern;
	regex_t regexp;
	unsigned linenum:1;
	unsigned invert:1;
	int regflags;
	unsigned pre_context;
	unsigned post_context;
};

static char *end_of_line(char *cp, unsigned long *left)
{
	unsigned long l = *left;
	while (l && *cp != '\n') {
		l--;
		cp++;
	}
	*left = l;
	return cp;
}

static void show_line(struct grep_opt *opt, const char *bol, const char *eol,
		      const char *name, unsigned lno, char sign)
{
	printf("%s%c", name, sign);
	if (opt->linenum)
		printf("%d%c", lno, sign);
	printf("%.*s\n", eol-bol, bol);
}

static int grep_buffer(struct grep_opt *opt, const char *name,
		       char *buf, unsigned long size)
{
	char *bol = buf;
	unsigned long left = size;
	unsigned lno = 1;
	struct pre_context_line {
		char *bol;
		char *eol;
	} *prev = NULL, *pcl;
	unsigned last_hit = 0;
	unsigned last_shown = 0;
	const char *hunk_mark = "";

	if (opt->pre_context)
		prev = xcalloc(opt->pre_context, sizeof(*prev));
	if (opt->pre_context || opt->post_context)
		hunk_mark = "--\n";

	while (left) {
		regmatch_t pmatch[10];
		char *eol, ch;
		int hit;

		eol = end_of_line(bol, &left);
		ch = *eol;
		*eol = 0;

		hit = !regexec(&opt->regexp, bol, ARRAY_SIZE(pmatch),
			       pmatch, 0);
		if (opt->invert)
			hit = !hit;
		if (hit) {
			/* Hit at this line.  If we haven't shown the
			 * pre-context lines, we would need to show them.
			 */
			if (opt->pre_context) {
				unsigned from;
				if (opt->pre_context < lno)
					from = lno - opt->pre_context;
				else
					from = 1;
				if (from <= last_shown)
					from = last_shown + 1;
				if (last_shown && from != last_shown + 1)
					printf(hunk_mark);
				while (from < lno) {
					pcl = &prev[lno-from-1];
					show_line(opt, pcl->bol, pcl->eol,
						  name, from, '-');
					from++;
				}
				last_shown = lno-1;
			}
			if (last_shown && lno != last_shown + 1)
				printf(hunk_mark);
			show_line(opt, bol, eol, name, lno, ':');
			last_shown = last_hit = lno;
		}
		else if (last_hit &&
			 lno <= last_hit + opt->post_context) {
			/* If the last hit is within the post context,
			 * we need to show this line.
			 */
			if (last_shown && lno != last_shown + 1)
				printf(hunk_mark);
			show_line(opt, bol, eol, name, lno, '-');
			last_shown = lno;
		}
		if (opt->pre_context) {
			memmove(prev+1, prev,
				(opt->pre_context-1) * sizeof(*prev));
			prev->bol = bol;
			prev->eol = eol;
		}
		*eol = ch;
		bol = eol + 1;
		left--;
		lno++;
	}
	return !!last_hit;
}

static int grep_sha1(struct grep_opt *opt, const unsigned char *sha1, const char *name)
{
	unsigned long size;
	char *data;
	char type[20];
	int hit;
	data = read_sha1_file(sha1, type, &size);
	if (!data) {
		error("'%s': unable to read %s", name, sha1_to_hex(sha1));
		return 0;
	}
	hit = grep_buffer(opt, name, data, size);
	free(data);
	return hit;
}

static int grep_file(struct grep_opt *opt, const char *filename)
{
	struct stat st;
	int i;
	char *data;
	if (lstat(filename, &st) < 0) {
	err_ret:
		if (errno != ENOENT)
			error("'%s': %s", filename, strerror(errno));
		return 0;
	}
	if (!st.st_size)
		return 0; /* empty file -- no grep hit */
	if (!S_ISREG(st.st_mode))
		return 0;
	i = open(filename, O_RDONLY);
	if (i < 0)
		goto err_ret;
	data = xmalloc(st.st_size + 1);
	if (st.st_size != xread(i, data, st.st_size)) {
		error("'%s': short read %s", filename, strerror(errno));
		close(i);
		free(data);
		return 0;
	}
	close(i);
	i = grep_buffer(opt, filename, data, st.st_size);
	free(data);
	return i;
}

static int grep_cache(struct grep_opt *opt, struct rev_info *revs, int cached)
{
	int hit = 0;
	int nr;
	read_cache();

	for (nr = 0; nr < active_nr; nr++) {
		struct cache_entry *ce = active_cache[nr];
		if (ce_stage(ce) || !S_ISREG(ntohl(ce->ce_mode)))
			continue;
		if (!pathspec_matches(&revs->diffopt, ce->name))
			continue;
		if (cached)
			hit |= grep_sha1(opt, ce->sha1, ce->name);
		else
			hit |= grep_file(opt, ce->name);
	}
	return hit;
}

static int grep_tree(struct grep_opt *opt, struct rev_info *revs,
		     struct tree_desc *tree,
		     const char *tree_name, const char *base)
{
	unsigned mode;
	int len;
	int hit = 0;
	const char *path;
	const unsigned char *sha1;
	char *down_base;
	char *path_buf = xmalloc(PATH_MAX + strlen(tree_name) + 100);

	if (tree_name[0]) {
		int offset = sprintf(path_buf, "%s:", tree_name);
		down_base = path_buf + offset;
		strcat(down_base, base);
	}
	else {
		down_base = path_buf;
		strcpy(down_base, base);
	}
	len = strlen(path_buf);

	while (tree->size) {
		int pathlen;
		sha1 = tree_entry_extract(tree, &path, &mode);
		pathlen = strlen(path);
		strcpy(path_buf + len, path);

		if (!pathspec_matches(&revs->diffopt, down_base))
			;
		else if (S_ISREG(mode))
			hit |= grep_sha1(opt, sha1, path_buf);
		else if (S_ISDIR(mode)) {
			char type[20];
			struct tree_desc sub;
			void *data;
			data = read_sha1_file(sha1, type, &sub.size);
			if (!data)
				die("unable to read tree (%s)",
				    sha1_to_hex(sha1));
			strcpy(path_buf + len + pathlen, "/");
			sub.buf = data;
			hit = grep_tree(opt, revs, &sub, tree_name, down_base);
			free(data);
		}
		update_tree_entry(tree);
	}
	return hit;
}

static int grep_object(struct grep_opt *opt, struct rev_info *revs,
		       struct object *obj, const char *name)
{
	if (!strcmp(obj->type, blob_type))
		return grep_sha1(opt, obj->sha1, name);
	if (!strcmp(obj->type, commit_type) ||
	    !strcmp(obj->type, tree_type)) {
		struct tree_desc tree;
		void *data;
		int hit;
		data = read_object_with_reference(obj->sha1, tree_type,
						  &tree.size, NULL);
		if (!data)
			die("unable to read tree (%s)", sha1_to_hex(obj->sha1));
		tree.buf = data;
		hit = grep_tree(opt, revs, &tree, name, "");
		free(data);
		return hit;
	}
	die("unable to grep from object of type %s", obj->type);
}

static const char builtin_grep_usage[] =
"git-grep <option>* <rev>* [-e] <pattern> [<path>...]";

int cmd_grep(int argc, const char **argv, char **envp)
{
	struct rev_info rev;
	const char **dst, **src;
	int err;
	int hit = 0;
	int no_more_arg = 0;
	int seen_range = 0;
	int seen_noncommit = 0;
	int cached = 0;
	struct grep_opt opt;
	struct object_list *list;

	memset(&opt, 0, sizeof(opt));
	opt.regflags = REG_NEWLINE;

	/*
	 * Interpret and remove the grep options upfront.  Sigh...
	 */
	for (dst = src = &argv[1]; src < argc + argv; ) {
		const char *arg = *src++;
		if (!no_more_arg) {
			if (!strcmp("--", arg)) {
				no_more_arg = 1;
				*dst++ = arg;
				continue;
			}
			if (!strcmp("--cached", arg)) {
				cached = 1;
				continue;
			}
			if (!strcmp("-i", arg) ||
			    !strcmp("--ignore-case", arg)) {
				opt.regflags |= REG_ICASE;
				continue;
			}
			if (!strcmp("-v", arg) ||
			    !strcmp("--invert-match", arg)) {
				opt.invert = 1;
				continue;
			}
			if (!strcmp("-E", arg) ||
			    !strcmp("--extended-regexp", arg)) {
				opt.regflags |= REG_EXTENDED;
				continue;
			}
			if (!strcmp("-G", arg) ||
			    !strcmp("--basic-regexp", arg)) {
				opt.regflags &= ~REG_EXTENDED;
				continue;
			}
			if (!strcmp("-e", arg)) {
				if (src < argc + argv) {
					opt.pattern = *src++;
					continue;
				}
				usage(builtin_grep_usage);
			}
			if (!strcmp("-n", arg)) {
				opt.linenum = 1;
				continue;
			}
			if (!strcmp("-H", arg)) {
				/* We always show the pathname, so this
				 * is a noop.
				 */
				continue;
			}
			if (!strcmp("-A", arg) ||
			    !strcmp("-B", arg) ||
			    !strcmp("-C", arg)) {
				unsigned num;
				if ((argc + argv <= src) ||
				    sscanf(*src++, "%u", &num) != 1)
					usage(builtin_grep_usage);
				switch (arg[1]) {
				case 'A':
					opt.post_context = num;
					break;
				case 'C':
					opt.post_context = num;
				case 'B':
					opt.pre_context = num;
					break;
				}
				continue;
			}
		}
		*dst++ = arg;
	}
	if (!opt.pattern)
		die("no pattern given.");

	err = regcomp(&opt.regexp, opt.pattern, opt.regflags);
	if (err) {
		char errbuf[1024];
		regerror(err, &opt.regexp, errbuf, 1024);
		regfree(&opt.regexp);
		die("'%s': %s", opt.pattern, errbuf);
	}

	init_revisions(&rev);
	*dst = NULL;
	argc = setup_revisions(dst - argv, argv, &rev, NULL);

	/*
	 * Do not walk "grep -e foo master next pu -- Documentation/"
	 * but do walk "grep -e foo master..next -- Documentation/".
	 * Ranged request mixed with a blob or tree object, like
	 * "grep -e foo v1.0.0:Documentation/ master..next"
	 * so detect that and complain.
	 */
	for (list = rev.pending_objects; list; list = list->next) {
		struct object *real_obj;
		if (list->item->flags & UNINTERESTING)
			seen_range = 1;
		real_obj = deref_tag(list->item, NULL, 0);
		if (strcmp(real_obj->type, commit_type))
			seen_noncommit = 1;
	}
	if (!rev.pending_objects)
		return !grep_cache(&opt, &rev, cached);
	if (cached)
		die("both --cached and revisions given.");

	if (seen_range && seen_noncommit)
		die("both A..B and non commit are given.");
	if (seen_range) {
		struct commit *commit;
		prepare_revision_walk(&rev);
		while ((commit = get_revision(&rev)) != NULL) {
			unsigned char *sha1 = commit->object.sha1;
			const char *n = find_unique_abbrev(sha1, rev.abbrev);
			char rev_name[41];
			strcpy(rev_name, n);
			if (grep_object(&opt, &rev, &commit->object, rev_name))
				hit = 1;
			commit->buffer = NULL;
		}
		return !hit;
	}

	/* all of them are non-commit; do not walk, and
	 * do not lose their names.
	 */
	for (list = rev.pending_objects; list; list = list->next) {
		struct object *real_obj;
		real_obj = deref_tag(list->item, NULL, 0);
		if (grep_object(&opt, &rev, real_obj, list->name))
			hit = 1;
	}
	return !hit;
}
