#include "cache.h"
#include "commit.h"
#include "diff.h"
#include "diffcore.h"
#include "quote.h"

static int uninteresting(struct diff_filepair *p)
{
	if (diff_unmodified_pair(p))
		return 1;
	if (!S_ISREG(p->one->mode) || !S_ISREG(p->two->mode))
		return 1;
	return 0;
}

static struct combine_diff_path *intersect_paths(struct combine_diff_path *curr, int n, int num_parent)
{
	struct diff_queue_struct *q = &diff_queued_diff;
	struct combine_diff_path *p;
	int i;

	if (!n) {
		struct combine_diff_path *list = NULL, **tail = &list;
		for (i = 0; i < q->nr; i++) {
			int len;
			const char *path;
			if (uninteresting(q->queue[i]))
				continue;
			path = q->queue[i]->two->path;
			len = strlen(path);

			p = xmalloc(sizeof(*p) + len + 1 + num_parent * 20);
			p->path = (char*) &(p->parent_sha1[num_parent][0]);
			memcpy(p->path, path, len);
			p->path[len] = 0;
			p->len = len;
			p->next = NULL;
			memcpy(p->sha1, q->queue[i]->two->sha1, 20);
			memcpy(p->parent_sha1[n], q->queue[i]->one->sha1, 20);
			*tail = p;
			tail = &p->next;
		}
		return list;
	}

	for (p = curr; p; p = p->next) {
		int found = 0;
		if (!p->len)
			continue;
		for (i = 0; i < q->nr; i++) {
			const char *path;
			int len;

			if (uninteresting(q->queue[i]))
				continue;
			path = q->queue[i]->two->path;
			len = strlen(path);
			if (len == p->len && !memcmp(path, p->path, len)) {
				found = 1;
				memcpy(p->parent_sha1[n],
				       q->queue[i]->one->sha1, 20);
				break;
			}
		}
		if (!found)
			p->len = 0;
	}
	return curr;
}

struct lline {
	struct lline *next;
	int len;
	unsigned long parent_map;
	char line[FLEX_ARRAY];
};

struct sline {
	struct lline *lost_head, **lost_tail;
	char *bol;
	int len;
	unsigned long flag;
};

static char *grab_blob(const unsigned char *sha1, unsigned long *size)
{
	char *blob;
	char type[20];
	if (!memcmp(sha1, null_sha1, 20)) {
		/* deleted blob */
		*size = 0;
		return xcalloc(1, 1);
	}
	blob = read_sha1_file(sha1, type, size);
	if (strcmp(type, "blob"))
		die("object '%s' is not a blob!", sha1_to_hex(sha1));
	return blob;
}

#define TMPPATHLEN 50
#define MAXLINELEN 10240

static void write_to_temp_file(char *tmpfile, void *blob, unsigned long size)
{
	int fd = git_mkstemp(tmpfile, TMPPATHLEN, ".diff_XXXXXX");
	if (fd < 0)
		die("unable to create temp-file");
	if (write(fd, blob, size) != size)
		die("unable to write temp-file");
	close(fd);
}

static void write_temp_blob(char *tmpfile, const unsigned char *sha1)
{
	unsigned long size;
	void *blob;
	blob = grab_blob(sha1, &size);
	write_to_temp_file(tmpfile, blob, size);
	free(blob);
}

static int parse_num(char **cp_p, unsigned int *num_p)
{
	char *cp = *cp_p;
	unsigned int num = 0;
	int read_some;

	while ('0' <= *cp && *cp <= '9')
		num = num * 10 + *cp++ - '0';
	if (!(read_some = cp - *cp_p))
		return -1;
	*cp_p = cp;
	*num_p = num;
	return 0;
}

static int parse_hunk_header(char *line, int len,
			     unsigned int *ob, unsigned int *on,
			     unsigned int *nb, unsigned int *nn)
{
	char *cp;
	cp = line + 4;
	if (parse_num(&cp, ob)) {
	bad_line:
		return error("malformed diff output: %s", line);
	}
	if (*cp == ',') {
		cp++;
		if (parse_num(&cp, on))
			goto bad_line;
	}
	else
		*on = 1;
	if (*cp++ != ' ' || *cp++ != '+')
		goto bad_line;
	if (parse_num(&cp, nb))
		goto bad_line;
	if (*cp == ',') {
		cp++;
		if (parse_num(&cp, nn))
			goto bad_line;
	}
	else
		*nn = 1;
	return -!!memcmp(cp, " @@", 3);
}

static void append_lost(struct sline *sline, int n, const char *line)
{
	struct lline *lline;
	int len = strlen(line);
	unsigned long this_mask = (1UL<<n);
	if (line[len-1] == '\n')
		len--;

	/* Check to see if we can squash things */
	if (sline->lost_head) {
		struct lline *last_one = NULL;
		/* We cannot squash it with earlier one */
		for (lline = sline->lost_head;
		     lline;
		     lline = lline->next)
			if (lline->parent_map & this_mask)
				last_one = lline;
		lline = last_one ? last_one->next : sline->lost_head;
		while (lline) {
			if (lline->len == len &&
			    !memcmp(lline->line, line, len)) {
				lline->parent_map |= this_mask;
				return;
			}
			lline = lline->next;
		}
	}

	lline = xmalloc(sizeof(*lline) + len + 1);
	lline->len = len;
	lline->next = NULL;
	lline->parent_map = this_mask;
	memcpy(lline->line, line, len);
	lline->line[len] = 0;
	*sline->lost_tail = lline;
	sline->lost_tail = &lline->next;
}

static void combine_diff(const unsigned char *parent, const char *ourtmp,
			 struct sline *sline, int cnt, int n)
{
	FILE *in;
	char parent_tmp[TMPPATHLEN];
	char cmd[TMPPATHLEN * 2 + 1024];
	char line[MAXLINELEN];
	unsigned int lno, ob, on, nb, nn;
	unsigned long pmask = ~(1UL << n);
	struct sline *lost_bucket = NULL;

	write_temp_blob(parent_tmp, parent);
	sprintf(cmd, "diff --unified=0 -La/x -Lb/x '%s' '%s'",
		parent_tmp, ourtmp);
	in = popen(cmd, "r");
	if (!in)
		return;

	lno = 1;
	while (fgets(line, sizeof(line), in) != NULL) {
		int len = strlen(line);
		if (5 < len && !memcmp("@@ -", line, 4)) {
			if (parse_hunk_header(line, len,
					      &ob, &on, &nb, &nn))
				break;
			lno = nb;
			if (!nb) {
				/* @@ -1,2 +0,0 @@ to remove the
				 * first two lines...
				 */
				nb = 1;
			}
			lost_bucket = &sline[nb-1]; /* sline is 0 based */
			continue;
		}
		if (!lost_bucket)
			continue;
		switch (line[0]) {
		case '-':
			append_lost(lost_bucket, n, line+1);
			break;
		case '+':
			sline[lno-1].flag &= pmask;
			lno++;
			break;
		}
	}
	fclose(in);
	unlink(parent_tmp);
}

static unsigned long context = 3;
static char combine_marker = '@';

static int interesting(struct sline *sline, unsigned long all_mask)
{
	return ((sline->flag & all_mask) != all_mask || sline->lost_head);
}

static unsigned long line_common_diff(struct sline *sline, unsigned long all_mask)
{
	/*
	 * Look at the line and see from which parents we have the
	 * same difference.
	 */

	/* Lower bits of sline->flag records if the parent had this
	 * line, so XOR with all_mask gives us on-bits for parents we
	 * have differences with.
	 */
	unsigned long common_adds = (sline->flag ^ all_mask) & all_mask;
	unsigned long common_removes = all_mask;

	/* If all the parents have this line, that also counts as
	 * having the same difference.
	 */
	if (!common_adds)
		common_adds = all_mask;

	if (sline->lost_head) {
		/* Lost head list records the lines removed from
		 * the parents, and parent_map records from which
		 * parent the line was removed.
		 */
		struct lline *ll;
		for (ll = sline->lost_head; ll; ll = ll->next) {
			common_removes &= ll->parent_map;
		}
	}
	return common_adds & common_removes;
}

static unsigned long line_all_diff(struct sline *sline, unsigned long all_mask)
{
	/*
	 * Look at the line and see from which parents we have some difference.
	 */
	unsigned long different = (sline->flag ^ all_mask) & all_mask;
	if (sline->lost_head) {
		/* Lost head list records the lines removed from
		 * the parents, and parent_map records from which
		 * parent the line was removed.
		 */
		struct lline *ll;
		for (ll = sline->lost_head; ll; ll = ll->next) {
			different |= ll->parent_map;
		}
	}
	return different;
}

static unsigned long adjust_hunk_tail(struct sline *sline,
				      unsigned long all_mask,
				      unsigned long hunk_begin,
				      unsigned long i)
{
	/* i points at the first uninteresting line.
	 * If the last line of the hunk was interesting
	 * only because it has some deletion, then
	 * it is not all that interesting for the
	 * purpose of giving trailing context lines.
	 */
	if ((hunk_begin + 1 <= i) &&
	    ((sline[i-1].flag & all_mask) == all_mask))
		i--;
	return i;
}

static unsigned long next_interesting(struct sline *sline,
				      unsigned long mark,
				      unsigned long i,
				      unsigned long cnt,
				      int uninteresting)
{
	while (i < cnt)
		if (uninteresting ?
		    !(sline[i].flag & mark) :
		    (sline[i].flag & mark))
			return i;
		else
			i++;
	return cnt;
}

static int give_context(struct sline *sline, unsigned long cnt, int num_parent)
{
	unsigned long all_mask = (1UL<<num_parent) - 1;
	unsigned long mark = (1UL<<num_parent);
	unsigned long i;

	i = next_interesting(sline, mark, 0, cnt, 0);
	if (cnt <= i)
		return 0;

	while (i < cnt) {
		unsigned long j = (context < i) ? (i - context) : 0;
		unsigned long k;
		while (j < i)
			sline[j++].flag |= mark;

	again:
		j = next_interesting(sline, mark, i, cnt, 1);
		if (cnt <= j)
			break; /* the rest are all interesting */

		/* lookahead context lines */
		k = next_interesting(sline, mark, j, cnt, 0);
		j = adjust_hunk_tail(sline, all_mask, i, j);

		if (k < j + context) {
			/* k is interesting and [j,k) are not, but
			 * paint them interesting because the gap is small.
			 */
			while (j < k)
				sline[j++].flag |= mark;
			i = k;
			goto again;
		}

		/* j is the first uninteresting line and there is
		 * no overlap beyond it within context lines.
		 */
		i = k;
		k = (j + context < cnt) ? j + context : cnt;
		while (j < k)
			sline[j++].flag |= mark;
	}
	return 1;
}

static int make_hunks(struct sline *sline, unsigned long cnt,
		       int num_parent, int dense)
{
	unsigned long all_mask = (1UL<<num_parent) - 1;
	unsigned long mark = (1UL<<num_parent);
	unsigned long i;
	int has_interesting = 0;

	for (i = 0; i < cnt; i++) {
		if (interesting(&sline[i], all_mask))
			sline[i].flag |= mark;
		else
			sline[i].flag &= ~mark;
	}
	if (!dense)
		return give_context(sline, cnt, num_parent);

	/* Look at each hunk, and if we have changes from only one
	 * parent, or the changes are the same from all but one
	 * parent, mark that uninteresting.
	 */
	i = 0;
	while (i < cnt) {
		unsigned long j, hunk_begin, hunk_end;
		int same, diff;
		unsigned long same_diff, all_diff;
		while (i < cnt && !(sline[i].flag & mark))
			i++;
		if (cnt <= i)
			break; /* No more interesting hunks */
		hunk_begin = i;
		for (j = i + 1; j < cnt; j++) {
			if (!(sline[j].flag & mark)) {
				/* Look beyond the end to see if there
				 * is an interesting line after this
				 * hunk within context span.
				 */
				unsigned long la; /* lookahead */
				int contin = 0;
				la = adjust_hunk_tail(sline, all_mask,
						     hunk_begin, j);
				la = (la + context < cnt) ?
					(la + context) : cnt;
				while (j <= --la) {
					if (sline[la].flag & mark) {
						contin = 1;
						break;
					}
				}
				if (!contin)
					break;
				j = la;
			}
		}
		hunk_end = j;

		/* [i..hunk_end) are interesting.  Now does it have
		 * the same change with all but one parent?
		 */
		same_diff = all_mask;
		all_diff = 0;
		for (j = i; j < hunk_end; j++) {
			same_diff &= line_common_diff(sline + j, all_mask);
			all_diff |= line_all_diff(sline + j, all_mask);
		}
		diff = same = 0;
		for (j = 0; j < num_parent; j++) {
			if (same_diff & (1UL<<j))
				same++;
			if (all_diff & (1UL<<j))
				diff++;
		}
		if ((num_parent - 1 <= same) || (diff == 1)) {
			/* This hunk is not that interesting after all */
			for (j = hunk_begin; j < hunk_end; j++)
				sline[j].flag &= ~mark;
		}
		i = hunk_end;
	}

	has_interesting = give_context(sline, cnt, num_parent);
	return has_interesting;
}

static void dump_sline(struct sline *sline, int cnt, int num_parent)
{
	unsigned long mark = (1UL<<num_parent);
	int i;
	int lno = 0;

	while (1) {
		struct sline *sl = &sline[lno];
		int hunk_end;
		while (lno < cnt && !(sline[lno].flag & mark))
			lno++;
		if (cnt <= lno)
			break;
		for (hunk_end = lno + 1; hunk_end < cnt; hunk_end++)
			if (!(sline[hunk_end].flag & mark))
				break;
		for (i = 0; i <= num_parent; i++) putchar(combine_marker);
		printf(" +%d,%d ", lno+1, hunk_end-lno);
		for (i = 0; i <= num_parent; i++) putchar(combine_marker);
		putchar('\n');
		while (lno < hunk_end) {
			struct lline *ll;
			int j;
			sl = &sline[lno++];
			ll = sl->lost_head;
			while (ll) {
				for (j = 0; j < num_parent; j++) {
					if (ll->parent_map & (1UL<<j))
						putchar('-');
					else
						putchar(' ');
				}
				puts(ll->line);
				ll = ll->next;
			}
			for (j = 0; j < num_parent; j++) {
				if ((1UL<<j) & sl->flag)
					putchar(' ');
				else
					putchar('+');
			}
			printf("%.*s\n", sl->len, sl->bol);
		}
	}
}

static void reuse_combine_diff(struct sline *sline, unsigned long cnt,
			       int i, int j)
{
	/* We have already examined parent j and we know parent i
	 * and parent j are the same, so reuse the combined result
	 * of parent j for parent i.
	 */
	unsigned long lno, imask, jmask;
	imask = (1UL<<i);
	jmask = (1UL<<j);

	for (lno = 0; lno < cnt; lno++) {
		struct lline *ll = sline->lost_head;
		while (ll) {
			if (ll->parent_map & jmask)
				ll->parent_map |= imask;
			ll = ll->next;
		}
		if (!(sline->flag & jmask))
			sline->flag &= ~imask;
		sline++;
	}
}

int show_combined_diff(struct combine_diff_path *elem, int num_parent,
		       int dense, const char *header, int show_empty)
{
	unsigned long size, cnt, lno;
	char *result, *cp, *ep;
	struct sline *sline; /* survived lines */
	int i, show_hunks, shown_header = 0;
	char ourtmp_buf[TMPPATHLEN];
	char *ourtmp = ourtmp_buf;

	/* Read the result of merge first */
	if (memcmp(elem->sha1, null_sha1, 20)) {
		result = grab_blob(elem->sha1, &size);
		write_to_temp_file(ourtmp, result, size);
	}
	else {
		struct stat st;
		int fd;
		ourtmp = elem->path;
		if (0 <= (fd = open(ourtmp, O_RDONLY)) &&
		    !fstat(fd, &st)) {
			int len = st.st_size;
			int cnt = 0;

			size = len;
			result = xmalloc(len + 1);
			while (cnt < len) {
				int done = xread(fd, result+cnt, len-cnt);
				if (done == 0)
					break;
				if (done < 0)
					die("read error '%s'", ourtmp);
				cnt += done;
			}
			result[len] = 0;
		}
		else {
			/* deleted file */
			size = 0;
			result = xmalloc(1);
			result[0] = 0;
			ourtmp = "/dev/null";
		}
		if (0 <= fd)
			close(fd);
	}

	for (cnt = 0, cp = result; cp - result < size; cp++) {
		if (*cp == '\n')
			cnt++;
	}
	if (result[size-1] != '\n')
		cnt++; /* incomplete line */

	sline = xcalloc(cnt, sizeof(*sline));
	ep = result;
	sline[0].bol = result;
	for (lno = 0, cp = result; cp - result < size; cp++) {
		if (*cp == '\n') {
			sline[lno].lost_tail = &sline[lno].lost_head;
			sline[lno].len = cp - sline[lno].bol;
			sline[lno].flag = (1UL<<num_parent) - 1;
			lno++;
			if (lno < cnt)
				sline[lno].bol = cp + 1;
		}
	}
	if (result[size-1] != '\n') {
		sline[cnt-1].lost_tail = &sline[cnt-1].lost_head;
		sline[cnt-1].len = size - (sline[cnt-1].bol - result);
		sline[cnt-1].flag = (1UL<<num_parent) - 1;
	}

	for (i = 0; i < num_parent; i++) {
		int j;
		for (j = 0; j < i; j++) {
			if (!memcmp(elem->parent_sha1[i],
				    elem->parent_sha1[j], 20)) {
				reuse_combine_diff(sline, cnt, i, j);
				break;
			}
		}
		if (i <= j)
			combine_diff(elem->parent_sha1[i], ourtmp, sline,
				     cnt, i);
	}

	show_hunks = make_hunks(sline, cnt, num_parent, dense);

	if (header && (show_hunks || show_empty)) {
		shown_header++;
		puts(header);
	}
	if (show_hunks) {
		printf("diff --%s ", dense ? "cc" : "combined");
		if (quote_c_style(elem->path, NULL, NULL, 0))
			quote_c_style(elem->path, NULL, stdout, 0);
		else
			printf("%s", elem->path);
		putchar('\n');
		dump_sline(sline, cnt, num_parent);
	}
	if (ourtmp == ourtmp_buf)
		unlink(ourtmp);
	free(result);

	for (i = 0; i < cnt; i++) {
		if (sline[i].lost_head) {
			struct lline *ll = sline[i].lost_head;
			while (ll) {
				struct lline *tmp = ll;
				ll = ll->next;
				free(tmp);
			}
		}
	}
	free(sline);
	return shown_header;
}

int diff_tree_combined_merge(const unsigned char *sha1,
			     const char *header,
			     int show_empty_merge, int dense)
{
	struct commit *commit = lookup_commit(sha1);
	struct diff_options diffopts;
	struct commit_list *parents;
	struct combine_diff_path *p, *paths = NULL;
	int num_parent, i, num_paths;

	diff_setup(&diffopts);
	diffopts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diffopts.recursive = 1;

	/* count parents */
	for (parents = commit->parents, num_parent = 0;
	     parents;
	     parents = parents->next, num_parent++)
		; /* nothing */

	/* find set of paths that everybody touches */
	for (parents = commit->parents, i = 0;
	     parents;
	     parents = parents->next, i++) {
		struct commit *parent = parents->item;
		diff_tree_sha1(parent->object.sha1, commit->object.sha1, "",
			       &diffopts);
		paths = intersect_paths(paths, i, num_parent);
		diff_flush(&diffopts);
	}

	/* find out surviving paths */
	for (num_paths = 0, p = paths; p; p = p->next) {
		if (p->len)
			num_paths++;
	}
	if (num_paths || show_empty_merge) {
		for (p = paths; p; p = p->next) {
			if (!p->len)
				continue;
			if (show_combined_diff(p, num_parent, dense, header,
					       show_empty_merge))
				header = NULL;
		}
	}

	/* Clean things up */
	while (paths) {
		struct combine_diff_path *tmp = paths;
		paths = paths->next;
		free(tmp);
	}
	return 0;
}
