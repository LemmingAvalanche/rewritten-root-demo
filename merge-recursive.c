/*
 * Recursive Merge algorithm stolen from git-merge-recursive.py by
 * Fredrik Kuivinen.
 * The thieves were Alex Riesen and Johannes Schindelin, in June/July 2006
 */
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include "cache.h"
#include "cache-tree.h"
#include "commit.h"
#include "blob.h"
#include "tree-walk.h"
#include "diff.h"
#include "diffcore.h"
#include "run-command.h"
#include "tag.h"

#include "path-list.h"

/*#define DEBUG*/

#ifdef DEBUG
#define debug(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug(...) do { ; /* nothing */ } while (0)
#endif

#ifdef DEBUG
#include "quote.h"
static void show_ce_entry(const char *tag, struct cache_entry *ce)
{
	if (tag && *tag &&
	    (ce->ce_flags & htons(CE_VALID))) {
		static char alttag[4];
		memcpy(alttag, tag, 3);
		if (isalpha(tag[0]))
			alttag[0] = tolower(tag[0]);
		else if (tag[0] == '?')
			alttag[0] = '!';
		else {
			alttag[0] = 'v';
			alttag[1] = tag[0];
			alttag[2] = ' ';
			alttag[3] = 0;
		}
		tag = alttag;
	}

	fprintf(stderr,"%s%06o %s %d\t",
			tag,
			ntohl(ce->ce_mode),
			sha1_to_hex(ce->sha1),
			ce_stage(ce));
	write_name_quoted("", 0, ce->name,
			'\n', stderr);
	fputc('\n', stderr);
}

static void ls_files(void) {
	int i;
	for (i = 0; i < active_nr; i++) {
		struct cache_entry *ce = active_cache[i];
		show_ce_entry("", ce);
	}
	fprintf(stderr, "---\n");
	if (0) ls_files(); /* avoid "unused" warning */
}
#endif

/*
 * A virtual commit has
 * - (const char *)commit->util set to the name, and
 * - *(int *)commit->object.sha1 set to the virtual id.
 */

static unsigned commit_list_count(const struct commit_list *l)
{
	unsigned c = 0;
	for (; l; l = l->next )
		c++;
	return c;
}

static struct commit *make_virtual_commit(struct tree *tree, const char *comment)
{
	struct commit *commit = xcalloc(1, sizeof(struct commit));
	static unsigned virtual_id = 1;
	commit->tree = tree;
	commit->util = (void*)comment;
	*(int*)commit->object.sha1 = virtual_id++;
	return commit;
}

/*
 * TODO: we should not have to copy the SHA1s around, but rather reference
 * them. That way, sha_eq() is just sha1 == sha2.
 */
static int sha_eq(const unsigned char *a, const unsigned char *b)
{
	if (!a && !b)
		return 2;
	return a && b && memcmp(a, b, 20) == 0;
}

/*
 * TODO: check if we can just reuse the active_cache structure: it is already
 * sorted (by name, stage).
 * Only problem: do not write it when flushing the cache.
 */
struct stage_data
{
	struct
	{
		unsigned mode;
		unsigned char sha[20];
	} stages[4];
	unsigned processed:1;
};

static struct path_list currentFileSet = {NULL, 0, 0, 1};
static struct path_list currentDirectorySet = {NULL, 0, 0, 1};

static int output_indent = 0;

static void output(const char *fmt, ...)
{
	va_list args;
	int i;
	for (i = output_indent; i--;)
		fputs("  ", stdout);
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
	fputc('\n', stdout);
}

static void output_commit_title(struct commit *commit)
{
	int i;
	for (i = output_indent; i--;)
		fputs("  ", stdout);
	if (commit->util)
		printf("virtual %s\n", (char *)commit->util);
	else {
		printf("%s ", sha1_to_hex(commit->object.sha1));
		if (parse_commit(commit) != 0)
			printf("(bad commit)\n");
		else {
			const char *s;
			int len;
			for (s = commit->buffer; *s; s++)
				if (*s == '\n' && s[1] == '\n') {
					s += 2;
					break;
				}
			for (len = 0; s[len] && '\n' != s[len]; len++)
				; /* do nothing */
			printf("%.*s\n", len, s);
		}
	}
}

static const char *original_index_file;
static const char *temporary_index_file;
static int cache_dirty = 0;

static int flush_cache(void)
{
	/* flush temporary index */
	struct lock_file *lock = xcalloc(1, sizeof(struct lock_file));
	int fd = hold_lock_file_for_update(lock, getenv("GIT_INDEX_FILE"));
	if (fd < 0)
		die("could not lock %s", temporary_index_file);
	if (write_cache(fd, active_cache, active_nr) ||
			close(fd) || commit_lock_file(lock))
		die ("unable to write %s", getenv("GIT_INDEX_FILE"));
	discard_cache();
	cache_dirty = 0;
	return 0;
}

static void setup_index(int temp)
{
	const char *idx = temp ? temporary_index_file: original_index_file;
	if (cache_dirty)
		die("fatal: cache changed flush_cache();");
	unlink(temporary_index_file);
	setenv("GIT_INDEX_FILE", idx, 1);
	discard_cache();
}

static struct cache_entry *make_cache_entry(unsigned int mode,
		const unsigned char *sha1, const char *path, int stage, int refresh)
{
	int size, len;
	struct cache_entry *ce;

	if (!verify_path(path))
		return NULL;

	len = strlen(path);
	size = cache_entry_size(len);
	ce = xcalloc(1, size);

	memcpy(ce->sha1, sha1, 20);
	memcpy(ce->name, path, len);
	ce->ce_flags = create_ce_flags(len, stage);
	ce->ce_mode = create_ce_mode(mode);

	if (refresh)
		return refresh_cache_entry(ce, 0);

	return ce;
}

static int add_cacheinfo(unsigned int mode, const unsigned char *sha1,
		const char *path, int stage, int refresh, int options)
{
	struct cache_entry *ce;
	if (!cache_dirty)
		read_cache_from(getenv("GIT_INDEX_FILE"));
	cache_dirty++;
	ce = make_cache_entry(mode, sha1 ? sha1 : null_sha1, path, stage, refresh);
	if (!ce)
		return error("cache_addinfo failed: %s", strerror(cache_errno));
	return add_cache_entry(ce, options);
}

/*
 * This is a global variable which is used in a number of places but
 * only written to in the 'merge' function.
 *
 * index_only == 1    => Don't leave any non-stage 0 entries in the cache and
 *                       don't update the working directory.
 *               0    => Leave unmerged entries in the cache and update
 *                       the working directory.
 */
static int index_only = 0;

/*
 * TODO: this can be streamlined by refactoring builtin-read-tree.c
 */
static int git_read_tree(const struct tree *tree)
{
#if 0
	fprintf(stderr, "GIT_INDEX_FILE='%s' git-read-tree %s\n",
		getenv("GIT_INDEX_FILE"),
		sha1_to_hex(tree->object.sha1));
#endif
	int rc;
	const char *argv[] = { "git-read-tree", NULL, NULL, };
	if (cache_dirty)
		die("read-tree with dirty cache");
	argv[1] = sha1_to_hex(tree->object.sha1);
	rc = run_command_v(2, argv);
	return rc < 0 ? -1: rc;
}

/*
 * TODO: this can be streamlined by refactoring builtin-read-tree.c
 */
static int git_merge_trees(const char *update_arg,
			   struct tree *common,
			   struct tree *head,
			   struct tree *merge)
{
#if 0
	fprintf(stderr, "GIT_INDEX_FILE='%s' git-read-tree %s -m %s %s %s\n",
		getenv("GIT_INDEX_FILE"),
		update_arg,
		sha1_to_hex(common->object.sha1),
		sha1_to_hex(head->object.sha1),
		sha1_to_hex(merge->object.sha1));
#endif
	int rc;
	const char *argv[] = {
		"git-read-tree", NULL, "-m", NULL, NULL, NULL,
		NULL,
	};
	if (cache_dirty)
		flush_cache();
	argv[1] = update_arg;
	argv[3] = sha1_to_hex(common->object.sha1);
	argv[4] = sha1_to_hex(head->object.sha1);
	argv[5] = sha1_to_hex(merge->object.sha1);
	rc = run_command_v(6, argv);
	return rc < 0 ? -1: rc;
}

/*
 * TODO: this can be streamlined by refactoring builtin-write-tree.c
 */
static struct tree *git_write_tree(void)
{
#if 0
	fprintf(stderr, "GIT_INDEX_FILE='%s' git-write-tree\n",
		getenv("GIT_INDEX_FILE"));
#endif
	FILE *fp;
	int rc;
	char buf[41];
	unsigned char sha1[20];
	int ch;
	unsigned i = 0;
	if (cache_dirty)
		flush_cache();
	fp = popen("git-write-tree 2>/dev/null", "r");
	while ((ch = fgetc(fp)) != EOF)
		if (i < sizeof(buf)-1 && ch >= '0' && ch <= 'f')
			buf[i++] = ch;
		else
			break;
	rc = pclose(fp);
	if (rc == -1 || WEXITSTATUS(rc))
		return NULL;
	buf[i] = '\0';
	if (get_sha1(buf, sha1) != 0)
		return NULL;
	return lookup_tree(sha1);
}

static int save_files_dirs(const unsigned char *sha1,
		const char *base, int baselen, const char *path,
		unsigned int mode, int stage)
{
	int len = strlen(path);
	char *newpath = malloc(baselen + len + 1);
	memcpy(newpath, base, baselen);
	memcpy(newpath + baselen, path, len);
	newpath[baselen + len] = '\0';

	if (S_ISDIR(mode))
		path_list_insert(newpath, &currentDirectorySet);
	else
		path_list_insert(newpath, &currentFileSet);
	free(newpath);

	return READ_TREE_RECURSIVE;
}

static int get_files_dirs(struct tree *tree)
{
	int n;
	debug("get_files_dirs ...\n");
	if (read_tree_recursive(tree, "", 0, 0, NULL, save_files_dirs) != 0) {
		debug("  get_files_dirs done (0)\n");
		return 0;
	}
	n = currentFileSet.nr + currentDirectorySet.nr;
	debug("  get_files_dirs done (%d)\n", n);
	return n;
}

/*
 * Returns a index_entry instance which doesn't have to correspond to
 * a real cache entry in Git's index.
 */
static struct stage_data *insert_stage_data(const char *path,
		struct tree *o, struct tree *a, struct tree *b,
		struct path_list *entries)
{
	struct path_list_item *item;
	struct stage_data *e = xcalloc(1, sizeof(struct stage_data));
	get_tree_entry(o->object.sha1, path,
			e->stages[1].sha, &e->stages[1].mode);
	get_tree_entry(a->object.sha1, path,
			e->stages[2].sha, &e->stages[2].mode);
	get_tree_entry(b->object.sha1, path,
			e->stages[3].sha, &e->stages[3].mode);
	item = path_list_insert(path, entries);
	item->util = e;
	return e;
}

/*
 * Create a dictionary mapping file names to CacheEntry objects. The
 * dictionary contains one entry for every path with a non-zero stage entry.
 */
static struct path_list *get_unmerged(void)
{
	struct path_list *unmerged = xcalloc(1, sizeof(struct path_list));
	int i;

	unmerged->strdup_paths = 1;
	if (!cache_dirty) {
		read_cache_from(getenv("GIT_INDEX_FILE"));
		cache_dirty++;
	}
	for (i = 0; i < active_nr; i++) {
		struct path_list_item *item;
		struct stage_data *e;
		struct cache_entry *ce = active_cache[i];
		if (!ce_stage(ce))
			continue;

		item = path_list_lookup(ce->name, unmerged);
		if (!item) {
			item = path_list_insert(ce->name, unmerged);
			item->util = xcalloc(1, sizeof(struct stage_data));
		}
		e = item->util;
		e->stages[ce_stage(ce)].mode = ntohl(ce->ce_mode);
		memcpy(e->stages[ce_stage(ce)].sha, ce->sha1, 20);
	}

	return unmerged;
}

struct rename
{
	struct diff_filepair *pair;
	struct stage_data *src_entry;
	struct stage_data *dst_entry;
	unsigned processed:1;
};

/*
 * Get information of all renames which occured between 'oTree' and
 * 'tree'. We need the three trees in the merge ('oTree', 'aTree' and
 * 'bTree') to be able to associate the correct cache entries with
 * the rename information. 'tree' is always equal to either aTree or bTree.
 */
static struct path_list *get_renames(struct tree *tree,
					struct tree *oTree,
					struct tree *aTree,
					struct tree *bTree,
					struct path_list *entries)
{
	int i;
	struct path_list *renames;
	struct diff_options opts;
#ifdef DEBUG
	time_t t = time(0);

	debug("getRenames ...\n");
#endif

	renames = xcalloc(1, sizeof(struct path_list));
	diff_setup(&opts);
	opts.recursive = 1;
	opts.detect_rename = DIFF_DETECT_RENAME;
	opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	if (diff_setup_done(&opts) < 0)
		die("diff setup failed");
	diff_tree_sha1(oTree->object.sha1, tree->object.sha1, "", &opts);
	diffcore_std(&opts);
	for (i = 0; i < diff_queued_diff.nr; ++i) {
		struct path_list_item *item;
		struct rename *re;
		struct diff_filepair *pair = diff_queued_diff.queue[i];
		if (pair->status != 'R') {
			diff_free_filepair(pair);
			continue;
		}
		re = xmalloc(sizeof(*re));
		re->processed = 0;
		re->pair = pair;
		item = path_list_lookup(re->pair->one->path, entries);
		if (!item)
			re->src_entry = insert_stage_data(re->pair->one->path,
					oTree, aTree, bTree, entries);
		else
			re->src_entry = item->util;

		item = path_list_lookup(re->pair->two->path, entries);
		if (!item)
			re->dst_entry = insert_stage_data(re->pair->two->path,
					oTree, aTree, bTree, entries);
		else
			re->dst_entry = item->util;
		item = path_list_insert(pair->one->path, renames);
		item->util = re;
	}
	opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_queued_diff.nr = 0;
	diff_flush(&opts);
#ifdef DEBUG
	debug("  getRenames done in %ld\n", time(0)-t);
#endif
	return renames;
}

int update_stages(const char *path, struct diff_filespec *o,
		struct diff_filespec *a, struct diff_filespec *b, int clear)
{
	int options = ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE;
	if (clear)
		if (remove_file_from_cache(path))
			return -1;
	if (o)
		if (add_cacheinfo(o->mode, o->sha1, path, 1, 0, options))
			return -1;
	if (a)
		if (add_cacheinfo(a->mode, a->sha1, path, 2, 0, options))
			return -1;
	if (b)
		if (add_cacheinfo(b->mode, b->sha1, path, 3, 0, options))
			return -1;
	return 0;
}

static int remove_path(const char *name)
{
	int ret, len;
	char *slash, *dirs;

	ret = unlink(name);
	if (ret)
		return ret;
	len = strlen(name);
	dirs = malloc(len+1);
	memcpy(dirs, name, len);
	dirs[len] = '\0';
	while ((slash = strrchr(name, '/'))) {
		*slash = '\0';
		len = slash - name;
		if (rmdir(name) != 0)
			break;
	}
	free(dirs);
	return ret;
}

/* General TODO: unC99ify the code: no declaration after code */
/* General TODO: no javaIfiCation: rename updateCache to update_cache */
/*
 * TODO: once we no longer call external programs, we'd probably be better off
 * not setting / getting the environment variable GIT_INDEX_FILE all the time.
 */
int remove_file(int clean, const char *path)
{
	int updateCache = index_only || clean;
	int updateWd = !index_only;

	if (updateCache) {
		if (!cache_dirty)
			read_cache_from(getenv("GIT_INDEX_FILE"));
		cache_dirty++;
		if (remove_file_from_cache(path))
			return -1;
	}
	if (updateWd)
	{
		unlink(path);
		if (errno != ENOENT || errno != EISDIR)
			return -1;
		remove_path(path);
	}
	return 0;
}

static char *unique_path(const char *path, const char *branch)
{
	char *newpath = xmalloc(strlen(path) + 1 + strlen(branch) + 8 + 1);
	int suffix = 0;
	struct stat st;
	char *p = newpath + strlen(newpath);
	strcpy(newpath, path);
	strcat(newpath, "~");
	strcpy(p, branch);
	for (; *p; ++p)
		if ('/' == *p)
			*p = '_';
	while (path_list_has_path(&currentFileSet, newpath) ||
	       path_list_has_path(&currentDirectorySet, newpath) ||
	       lstat(newpath, &st) == 0)
		sprintf(p, "_%d", suffix++);

	path_list_insert(newpath, &currentFileSet);
	return newpath;
}

static int mkdir_p(const char *path, unsigned long mode)
{
	/* path points to cache entries, so strdup before messing with it */
	char *buf = strdup(path);
	int result = safe_create_leading_directories(buf);
	free(buf);
	return result;
}

static void flush_buffer(int fd, const char *buf, unsigned long size)
{
	while (size > 0) {
		long ret = xwrite(fd, buf, size);
		if (ret < 0) {
			/* Ignore epipe */
			if (errno == EPIPE)
				break;
			die("merge-recursive: %s", strerror(errno));
		} else if (!ret) {
			die("merge-recursive: disk full?");
		}
		size -= ret;
		buf += ret;
	}
}

void update_file_flags(const unsigned char *sha,
		       unsigned mode,
		       const char *path,
		       int update_cache,
		       int update_wd)
{
	if (index_only)
		update_wd = 0;

	if (update_wd) {
		char type[20];
		void *buf;
		unsigned long size;

		buf = read_sha1_file(sha, type, &size);
		if (!buf)
			die("cannot read object %s '%s'", sha1_to_hex(sha), path);
		if (strcmp(type, blob_type) != 0)
			die("blob expected for %s '%s'", sha1_to_hex(sha), path);

		if (S_ISREG(mode)) {
			int fd;
			if (mkdir_p(path, 0777))
				die("failed to create path %s: %s", path, strerror(errno));
			unlink(path);
			if (mode & 0100)
				mode = 0777;
			else
				mode = 0666;
			fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, mode);
			if (fd < 0)
				die("failed to open %s: %s", path, strerror(errno));
			flush_buffer(fd, buf, size);
			close(fd);
		} else if (S_ISLNK(mode)) {
			char *lnk = malloc(size + 1);
			memcpy(lnk, buf, size);
			lnk[size] = '\0';
			mkdir_p(path, 0777);
			unlink(lnk);
			symlink(lnk, path);
		} else
			die("do not know what to do with %06o %s '%s'",
			    mode, sha1_to_hex(sha), path);
	}
	if (update_cache)
		add_cacheinfo(mode, sha, path, 0, update_wd, ADD_CACHE_OK_TO_ADD);
}

void update_file(int clean,
		const unsigned char *sha,
		unsigned mode,
		const char *path)
{
	update_file_flags(sha, mode, path, index_only || clean, !index_only);
}

/* Low level file merging, update and removal */

struct merge_file_info
{
	unsigned char sha[20];
	unsigned mode;
	unsigned clean:1,
		 merge:1;
};

static char *git_unpack_file(const unsigned char *sha1, char *path)
{
	void *buf;
	char type[20];
	unsigned long size;
	int fd;

	buf = read_sha1_file(sha1, type, &size);
	if (!buf || strcmp(type, blob_type))
		die("unable to read blob object %s", sha1_to_hex(sha1));

	strcpy(path, ".merge_file_XXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		die("unable to create temp-file");
	flush_buffer(fd, buf, size);
	close(fd);
	return path;
}

static struct merge_file_info merge_file(struct diff_filespec *o,
		struct diff_filespec *a, struct diff_filespec *b,
		const char *branch1Name, const char *branch2Name)
{
	struct merge_file_info result;
	result.merge = 0;
	result.clean = 1;

	if ((S_IFMT & a->mode) != (S_IFMT & b->mode)) {
		result.clean = 0;
		if (S_ISREG(a->mode)) {
			result.mode = a->mode;
			memcpy(result.sha, a->sha1, 20);
		} else {
			result.mode = b->mode;
			memcpy(result.sha, b->sha1, 20);
		}
	} else {
		if (!sha_eq(a->sha1, o->sha1) && !sha_eq(b->sha1, o->sha1))
			result.merge = 1;

		result.mode = a->mode == o->mode ? b->mode: a->mode;

		if (sha_eq(a->sha1, o->sha1))
			memcpy(result.sha, b->sha1, 20);
		else if (sha_eq(b->sha1, o->sha1))
			memcpy(result.sha, a->sha1, 20);
		else if (S_ISREG(a->mode)) {
			int code = 1, fd;
			struct stat st;
			char orig[PATH_MAX];
			char src1[PATH_MAX];
			char src2[PATH_MAX];
			const char *argv[] = {
				"merge", "-L", NULL, "-L", NULL, "-L", NULL,
				src1, orig, src2,
				NULL
			};
			char *la, *lb, *lo;

			git_unpack_file(o->sha1, orig);
			git_unpack_file(a->sha1, src1);
			git_unpack_file(b->sha1, src2);

			argv[2] = la = strdup(mkpath("%s/%s", branch1Name, a->path));
			argv[6] = lb = strdup(mkpath("%s/%s", branch2Name, b->path));
			argv[4] = lo = strdup(mkpath("orig/%s", o->path));

#if 0
			printf("%s %s %s %s %s %s %s %s %s %s\n",
			       argv[0], argv[1], argv[2], argv[3], argv[4],
			       argv[5], argv[6], argv[7], argv[8], argv[9]);
#endif
			code = run_command_v(10, argv);

			free(la);
			free(lb);
			free(lo);
			if (code && code < -256) {
				die("Failed to execute 'merge'. merge(1) is used as the "
				    "file-level merge tool. Is 'merge' in your path?");
			}
			fd = open(src1, O_RDONLY);
			if (fd < 0 || fstat(fd, &st) < 0 ||
					index_fd(result.sha, fd, &st, 1,
						"blob"))
				die("Unable to add %s to database", src1);

			unlink(orig);
			unlink(src1);
			unlink(src2);

			result.clean = WEXITSTATUS(code) == 0;
		} else {
			if (!(S_ISLNK(a->mode) || S_ISLNK(b->mode)))
				die("cannot merge modes?");

			memcpy(result.sha, a->sha1, 20);

			if (!sha_eq(a->sha1, b->sha1))
				result.clean = 0;
		}
	}

	return result;
}

static void conflict_rename_rename(struct rename *ren1,
				   const char *branch1,
				   struct rename *ren2,
				   const char *branch2)
{
	char *del[2];
	int delp = 0;
	const char *ren1_dst = ren1->pair->two->path;
	const char *ren2_dst = ren2->pair->two->path;
	const char *dstName1 = ren1_dst;
	const char *dstName2 = ren2_dst;
	if (path_list_has_path(&currentDirectorySet, ren1_dst)) {
		dstName1 = del[delp++] = unique_path(ren1_dst, branch1);
		output("%s is a directory in %s adding as %s instead",
		       ren1_dst, branch2, dstName1);
		remove_file(0, ren1_dst);
	}
	if (path_list_has_path(&currentDirectorySet, ren2_dst)) {
		dstName2 = del[delp++] = unique_path(ren2_dst, branch2);
		output("%s is a directory in %s adding as %s instead",
		       ren2_dst, branch1, dstName2);
		remove_file(0, ren2_dst);
	}
	update_stages(dstName1, NULL, ren1->pair->two, NULL, 1);
	update_stages(dstName2, NULL, NULL, ren2->pair->two, 1);
	while (delp--)
		free(del[delp]);
}

static void conflict_rename_dir(struct rename *ren1,
				const char *branch1)
{
	char *newPath = unique_path(ren1->pair->two->path, branch1);
	output("Renaming %s to %s instead", ren1->pair->one->path, newPath);
	remove_file(0, ren1->pair->two->path);
	update_file(0, ren1->pair->two->sha1, ren1->pair->two->mode, newPath);
	free(newPath);
}

static void conflict_rename_rename_2(struct rename *ren1,
				     const char *branch1,
				     struct rename *ren2,
				     const char *branch2)
{
	char *newPath1 = unique_path(ren1->pair->two->path, branch1);
	char *newPath2 = unique_path(ren2->pair->two->path, branch2);
	output("Renaming %s to %s and %s to %s instead",
	       ren1->pair->one->path, newPath1,
	       ren2->pair->one->path, newPath2);
	remove_file(0, ren1->pair->two->path);
	update_file(0, ren1->pair->two->sha1, ren1->pair->two->mode, newPath1);
	update_file(0, ren2->pair->two->sha1, ren2->pair->two->mode, newPath2);
	free(newPath2);
	free(newPath1);
}

/* General TODO: get rid of all the debug messages */
static int process_renames(struct path_list *renamesA,
			   struct path_list *renamesB,
			   const char *branchNameA,
			   const char *branchNameB)
{
	int cleanMerge = 1, i, j;
	struct path_list byDstA = {NULL, 0, 0, 0}, byDstB = {NULL, 0, 0, 0};
	const struct rename *sre;

	for (i = 0; i < renamesA->nr; i++) {
		sre = renamesA->items[i].util;
		path_list_insert(sre->pair->two->path, &byDstA)->util
			= sre->dst_entry;
	}
	for (i = 0; i < renamesB->nr; i++) {
		sre = renamesB->items[i].util;
		path_list_insert(sre->pair->two->path, &byDstB)->util
			= sre->dst_entry;
	}

	for (i = 0, j = 0; i < renamesA->nr || j < renamesB->nr;) {
		int compare;
		char *src;
		struct path_list *renames1, *renames2, *renames2Dst;
		struct rename *ren1 = NULL, *ren2 = NULL;
		const char *branchName1, *branchName2;
		const char *ren1_src, *ren1_dst;

		if (i >= renamesA->nr) {
			compare = 1;
			ren2 = renamesB->items[j++].util;
		} else if (j >= renamesB->nr) {
			compare = -1;
			ren1 = renamesA->items[i++].util;
		} else {
			compare = strcmp(renamesA->items[i].path,
					renamesB->items[j].path);
			ren1 = renamesA->items[i++].util;
			ren2 = renamesB->items[j++].util;
		}

		/* TODO: refactor, so that 1/2 are not needed */
		if (ren1) {
			renames1 = renamesA;
			renames2 = renamesB;
			renames2Dst = &byDstB;
			branchName1 = branchNameA;
			branchName2 = branchNameB;
		} else {
			struct rename *tmp;
			renames1 = renamesB;
			renames2 = renamesA;
			renames2Dst = &byDstA;
			branchName1 = branchNameB;
			branchName2 = branchNameA;
			tmp = ren2;
			ren2 = ren1;
			ren1 = tmp;
		}
		src = ren1->pair->one->path;

		ren1->dst_entry->processed = 1;
		ren1->src_entry->processed = 1;

		if (ren1->processed)
			continue;
		ren1->processed = 1;

		ren1_src = ren1->pair->one->path;
		ren1_dst = ren1->pair->two->path;

		if (ren2) {
			const char *ren2_src = ren2->pair->one->path;
			const char *ren2_dst = ren2->pair->two->path;
			/* Renamed in 1 and renamed in 2 */
			if (strcmp(ren1_src, ren2_src) != 0)
				die("ren1.src != ren2.src");
			ren2->dst_entry->processed = 1;
			ren2->processed = 1;
			if (strcmp(ren1_dst, ren2_dst) != 0) {
				cleanMerge = 0;
				output("CONFLICT (rename/rename): "
				       "Rename %s->%s in branch %s "
				       "rename %s->%s in %s",
				       src, ren1_dst, branchName1,
				       src, ren2_dst, branchName2);
				conflict_rename_rename(ren1, branchName1, ren2, branchName2);
			} else {
				remove_file(1, ren1_src);
				struct merge_file_info mfi;
				mfi = merge_file(ren1->pair->one,
						 ren1->pair->two,
						 ren2->pair->two,
						 branchName1,
						 branchName2);
				if (mfi.merge || !mfi.clean)
					output("Renaming %s->%s", src, ren1_dst);

				if (mfi.merge)
					output("Auto-merging %s", ren1_dst);

				if (!mfi.clean) {
					output("CONFLICT (content): merge conflict in %s",
					       ren1_dst);
					cleanMerge = 0;

					if (!index_only)
						update_stages(ren1_dst,
							      ren1->pair->one,
							      ren1->pair->two,
							      ren2->pair->two,
							      1 /* clear */);
				}
				update_file(mfi.clean, mfi.sha, mfi.mode, ren1_dst);
			}
		} else {
			/* Renamed in 1, maybe changed in 2 */
			struct path_list_item *item;
			/* we only use sha1 and mode of these */
			struct diff_filespec src_other, dst_other;
			int tryMerge, stage = renamesA == renames1 ? 3: 2;

			remove_file(1, ren1_src);

			memcpy(src_other.sha1,
					ren1->src_entry->stages[stage].sha, 20);
			src_other.mode = ren1->src_entry->stages[stage].mode;
			memcpy(dst_other.sha1,
					ren1->dst_entry->stages[stage].sha, 20);
			dst_other.mode = ren1->dst_entry->stages[stage].mode;

			tryMerge = 0;

			if (path_list_has_path(&currentDirectorySet, ren1_dst)) {
				cleanMerge = 0;
				output("CONFLICT (rename/directory): Rename %s->%s in %s "
				       " directory %s added in %s",
				       ren1_src, ren1_dst, branchName1,
				       ren1_dst, branchName2);
				conflict_rename_dir(ren1, branchName1);
			} else if (sha_eq(src_other.sha1, null_sha1)) {
				cleanMerge = 0;
				output("CONFLICT (rename/delete): Rename %s->%s in %s "
				       "and deleted in %s",
				       ren1_src, ren1_dst, branchName1,
				       branchName2);
				update_file(0, ren1->pair->two->sha1, ren1->pair->two->mode, ren1_dst);
			} else if (!sha_eq(dst_other.sha1, null_sha1)) {
				const char *newPath;
				cleanMerge = 0;
				tryMerge = 1;
				output("CONFLICT (rename/add): Rename %s->%s in %s. "
				       "%s added in %s",
				       ren1_src, ren1_dst, branchName1,
				       ren1_dst, branchName2);
				newPath = unique_path(ren1_dst, branchName2);
				output("Adding as %s instead", newPath);
				update_file(0, dst_other.sha1, dst_other.mode, newPath);
			} else if ((item = path_list_lookup(ren1_dst, renames2Dst))) {
				ren2 = item->util;
				cleanMerge = 0;
				ren2->processed = 1;
				output("CONFLICT (rename/rename): Rename %s->%s in %s. "
				       "Rename %s->%s in %s",
				       ren1_src, ren1_dst, branchName1,
				       ren2->pair->one->path, ren2->pair->two->path, branchName2);
				conflict_rename_rename_2(ren1, branchName1, ren2, branchName2);
			} else
				tryMerge = 1;

			if (tryMerge) {
				struct diff_filespec *o, *a, *b;
				struct merge_file_info mfi;
				src_other.path = (char *)ren1_src;

				o = ren1->pair->one;
				if (renamesA == renames1) {
					a = ren1->pair->two;
					b = &src_other;
				} else {
					b = ren1->pair->two;
					a = &src_other;
				}
				mfi = merge_file(o, a, b,
						branchNameA, branchNameB);

				if (mfi.merge || !mfi.clean)
					output("Renaming %s => %s", ren1_src, ren1_dst);
				if (mfi.merge)
					output("Auto-merging %s", ren1_dst);
				if (!mfi.clean) {
					output("CONFLICT (rename/modify): Merge conflict in %s",
					       ren1_dst);
					cleanMerge = 0;

					if (!index_only)
						update_stages(ren1_dst,
								o, a, b, 1);
				}
				update_file(mfi.clean, mfi.sha, mfi.mode, ren1_dst);
			}
		}
	}
	path_list_clear(&byDstA, 0);
	path_list_clear(&byDstB, 0);

	if (cache_dirty)
		flush_cache();
	return cleanMerge;
}

static unsigned char *has_sha(const unsigned char *sha)
{
	return memcmp(sha, null_sha1, 20) == 0 ? NULL: (unsigned char *)sha;
}

/* Per entry merge function */
static int process_entry(const char *path, struct stage_data *entry,
			 const char *branch1Name,
			 const char *branch2Name)
{
	/*
	printf("processing entry, clean cache: %s\n", index_only ? "yes": "no");
	print_index_entry("\tpath: ", entry);
	*/
	int cleanMerge = 1;
	unsigned char *oSha = has_sha(entry->stages[1].sha);
	unsigned char *aSha = has_sha(entry->stages[2].sha);
	unsigned char *bSha = has_sha(entry->stages[3].sha);
	unsigned oMode = entry->stages[1].mode;
	unsigned aMode = entry->stages[2].mode;
	unsigned bMode = entry->stages[3].mode;

	if (oSha && (!aSha || !bSha)) {
		/* Case A: Deleted in one */
		if ((!aSha && !bSha) ||
		    (sha_eq(aSha, oSha) && !bSha) ||
		    (!aSha && sha_eq(bSha, oSha))) {
			/* Deleted in both or deleted in one and
			 * unchanged in the other */
			if (aSha)
				output("Removing %s", path);
			remove_file(1, path);
		} else {
			/* Deleted in one and changed in the other */
			cleanMerge = 0;
			if (!aSha) {
				output("CONFLICT (delete/modify): %s deleted in %s "
				       "and modified in %s. Version %s of %s left in tree.",
				       path, branch1Name,
				       branch2Name, branch2Name, path);
				update_file(0, bSha, bMode, path);
			} else {
				output("CONFLICT (delete/modify): %s deleted in %s "
				       "and modified in %s. Version %s of %s left in tree.",
				       path, branch2Name,
				       branch1Name, branch1Name, path);
				update_file(0, aSha, aMode, path);
			}
		}

	} else if ((!oSha && aSha && !bSha) ||
		   (!oSha && !aSha && bSha)) {
		/* Case B: Added in one. */
		const char *addBranch;
		const char *otherBranch;
		unsigned mode;
		const unsigned char *sha;
		const char *conf;

		if (aSha) {
			addBranch = branch1Name;
			otherBranch = branch2Name;
			mode = aMode;
			sha = aSha;
			conf = "file/directory";
		} else {
			addBranch = branch2Name;
			otherBranch = branch1Name;
			mode = bMode;
			sha = bSha;
			conf = "directory/file";
		}
		if (path_list_has_path(&currentDirectorySet, path)) {
			const char *newPath = unique_path(path, addBranch);
			cleanMerge = 0;
			output("CONFLICT (%s): There is a directory with name %s in %s. "
			       "Adding %s as %s",
			       conf, path, otherBranch, path, newPath);
			remove_file(0, path);
			update_file(0, sha, mode, newPath);
		} else {
			output("Adding %s", path);
			update_file(1, sha, mode, path);
		}
	} else if (!oSha && aSha && bSha) {
		/* Case C: Added in both (check for same permissions). */
		if (sha_eq(aSha, bSha)) {
			if (aMode != bMode) {
				cleanMerge = 0;
				output("CONFLICT: File %s added identically in both branches, "
				       "but permissions conflict %06o->%06o",
				       path, aMode, bMode);
				output("CONFLICT: adding with permission: %06o", aMode);
				update_file(0, aSha, aMode, path);
			} else {
				/* This case is handled by git-read-tree */
				assert(0 && "This case must be handled by git-read-tree");
			}
		} else {
			const char *newPath1, *newPath2;
			cleanMerge = 0;
			newPath1 = unique_path(path, branch1Name);
			newPath2 = unique_path(path, branch2Name);
			output("CONFLICT (add/add): File %s added non-identically "
			       "in both branches. Adding as %s and %s instead.",
			       path, newPath1, newPath2);
			remove_file(0, path);
			update_file(0, aSha, aMode, newPath1);
			update_file(0, bSha, bMode, newPath2);
		}

	} else if (oSha && aSha && bSha) {
		/* case D: Modified in both, but differently. */
		struct merge_file_info mfi;
		struct diff_filespec o, a, b;

		output("Auto-merging %s", path);
		o.path = a.path = b.path = (char *)path;
		memcpy(o.sha1, oSha, 20);
		o.mode = oMode;
		memcpy(a.sha1, aSha, 20);
		a.mode = aMode;
		memcpy(b.sha1, bSha, 20);
		b.mode = bMode;

		mfi = merge_file(&o, &a, &b,
				 branch1Name, branch2Name);

		if (mfi.clean)
			update_file(1, mfi.sha, mfi.mode, path);
		else {
			cleanMerge = 0;
			output("CONFLICT (content): Merge conflict in %s", path);

			if (index_only)
				update_file(0, mfi.sha, mfi.mode, path);
			else
				update_file_flags(mfi.sha, mfi.mode, path,
					      0 /* updateCache */, 1 /* updateWd */);
		}
	} else
		die("Fatal merge failure, shouldn't happen.");

	if (cache_dirty)
		flush_cache();

	return cleanMerge;
}

static int merge_trees(struct tree *head,
		       struct tree *merge,
		       struct tree *common,
		       const char *branch1Name,
		       const char *branch2Name,
		       struct tree **result)
{
	int code, clean;
	if (sha_eq(common->object.sha1, merge->object.sha1)) {
		output("Already uptodate!");
		*result = head;
		return 1;
	}

	code = git_merge_trees(index_only ? "-i": "-u", common, head, merge);

	if (code != 0)
		die("merging of trees %s and %s failed",
		    sha1_to_hex(head->object.sha1),
		    sha1_to_hex(merge->object.sha1));

	*result = git_write_tree();

	if (!*result) {
		struct path_list *entries, *re_head, *re_merge;
		int i;
		path_list_clear(&currentFileSet, 1);
		path_list_clear(&currentDirectorySet, 1);
		get_files_dirs(head);
		get_files_dirs(merge);

		entries = get_unmerged();
		re_head  = get_renames(head, common, head, merge, entries);
		re_merge = get_renames(merge, common, head, merge, entries);
		clean = process_renames(re_head, re_merge,
				branch1Name, branch2Name);
		for (i = 0; i < entries->nr; i++) {
			const char *path = entries->items[i].path;
			struct stage_data *e = entries->items[i].util;
			if (e->processed)
				continue;
			if (!process_entry(path, e, branch1Name, branch2Name))
				clean = 0;
		}

		path_list_clear(re_merge, 0);
		path_list_clear(re_head, 0);
		path_list_clear(entries, 1);

		if (clean || index_only)
			*result = git_write_tree();
		else
			*result = NULL;
	} else {
		clean = 1;
		printf("merging of trees %s and %s resulted in %s\n",
		       sha1_to_hex(head->object.sha1),
		       sha1_to_hex(merge->object.sha1),
		       sha1_to_hex((*result)->object.sha1));
	}

	return clean;
}

/*
 * Merge the commits h1 and h2, return the resulting virtual
 * commit object and a flag indicating the cleaness of the merge.
 */
static
int merge(struct commit *h1,
			  struct commit *h2,
			  const char *branch1Name,
			  const char *branch2Name,
			  int callDepth /* =0 */,
			  struct commit *ancestor /* =None */,
			  struct commit **result)
{
	struct commit_list *ca = NULL, *iter;
	struct commit *mergedCA;
	struct tree *mrtree;
	int clean;

	output("Merging:");
	output_commit_title(h1);
	output_commit_title(h2);

	if (ancestor)
		commit_list_insert(ancestor, &ca);
	else
		ca = get_merge_bases(h1, h2, 1);

	output("found %u common ancestor(s):", commit_list_count(ca));
	for (iter = ca; iter; iter = iter->next)
		output_commit_title(iter->item);

	mergedCA = pop_commit(&ca);

	for (iter = ca; iter; iter = iter->next) {
		output_indent = callDepth + 1;
		/*
		 * When the merge fails, the result contains files
		 * with conflict markers. The cleanness flag is
		 * ignored, it was never acutally used, as result of
		 * merge_trees has always overwritten it: the commited
		 * "conflicts" were already resolved.
		 */
		merge(mergedCA, iter->item,
		      "Temporary merge branch 1",
		      "Temporary merge branch 2",
		      callDepth + 1,
		      NULL,
		      &mergedCA);
		output_indent = callDepth;

		if (!mergedCA)
			die("merge returned no commit");
	}

	if (callDepth == 0) {
		setup_index(0 /* $GIT_DIR/index */);
		index_only = 0;
	} else {
		setup_index(1 /* temporary index */);
		git_read_tree(h1->tree);
		index_only = 1;
	}

	clean = merge_trees(h1->tree, h2->tree, mergedCA->tree,
			    branch1Name, branch2Name, &mrtree);

	if (!ancestor && (clean || index_only)) {
		*result = make_virtual_commit(mrtree, "merged tree");
		commit_list_insert(h1, &(*result)->parents);
		commit_list_insert(h2, &(*result)->parents->next);
	} else
		*result = NULL;

	return clean;
}

static struct commit *get_ref(const char *ref)
{
	unsigned char sha1[20];
	struct object *object;

	if (get_sha1(ref, sha1))
		die("Could not resolve ref '%s'", ref);
	object = deref_tag(parse_object(sha1), ref, strlen(ref));
	if (object->type != OBJ_COMMIT)
		return NULL;
	if (parse_commit((struct commit *)object))
		die("Could not parse commit '%s'", sha1_to_hex(object->sha1));
	return (struct commit *)object;
}

int main(int argc, char *argv[])
{
	static const char *bases[2];
	static unsigned bases_count = 0;
	int i, clean;
	const char *branch1, *branch2;
	struct commit *result, *h1, *h2;

	original_index_file = getenv("GIT_INDEX_FILE");

	if (!original_index_file)
		original_index_file = strdup(git_path("index"));

	temporary_index_file = strdup(git_path("mrg-rcrsv-tmp-idx"));

	if (argc < 4)
		die("Usage: %s <base>... -- <head> <remote> ...\n", argv[0]);

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--"))
			break;
		if (bases_count < sizeof(bases)/sizeof(*bases))
			bases[bases_count++] = argv[i];
	}
	if (argc - i != 3) /* "--" "<head>" "<remote>" */
		die("Not handling anything other than two heads merge.");

	branch1 = argv[++i];
	branch2 = argv[++i];
	printf("Merging %s with %s\n", branch1, branch2);

	h1 = get_ref(branch1);
	h2 = get_ref(branch2);

	if (bases_count == 1) {
		struct commit *ancestor = get_ref(bases[0]);
		clean = merge(h1, h2, branch1, branch2, 0, ancestor, &result);
	} else
		clean = merge(h1, h2, branch1, branch2, 0, NULL, &result);

	if (cache_dirty)
		flush_cache();

	return clean ? 0: 1;
}

/*
vim: sw=8 noet
*/
