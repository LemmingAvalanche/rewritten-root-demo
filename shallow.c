#include "cache.h"
#include "tempfile.h"
#include "lockfile.h"
#include "commit.h"
#include "tag.h"
#include "pkt-line.h"
#include "remote.h"
#include "refs.h"
#include "sha1-array.h"
#include "diff.h"
#include "revision.h"
#include "commit-slab.h"
#include "revision.h"
#include "list-objects.h"

static int is_shallow = -1;
static struct stat_validity shallow_stat;
static char *alternate_shallow_file;

void set_alternate_shallow_file(const char *path, int override)
{
	if (is_shallow != -1)
		die("BUG: is_repository_shallow must not be called before set_alternate_shallow_file");
	if (alternate_shallow_file && !override)
		return;
	free(alternate_shallow_file);
	alternate_shallow_file = xstrdup_or_null(path);
}

int register_shallow(const struct object_id *oid)
{
	struct commit_graft *graft =
		xmalloc(sizeof(struct commit_graft));
	struct commit *commit = lookup_commit(oid);

	oidcpy(&graft->oid, oid);
	graft->nr_parent = -1;
	if (commit && commit->object.parsed)
		commit->parents = NULL;
	return register_commit_graft(graft, 0);
}

int is_repository_shallow(void)
{
	FILE *fp;
	char buf[1024];
	const char *path = alternate_shallow_file;

	if (is_shallow >= 0)
		return is_shallow;

	if (!path)
		path = git_path_shallow();
	/*
	 * fetch-pack sets '--shallow-file ""' as an indicator that no
	 * shallow file should be used. We could just open it and it
	 * will likely fail. But let's do an explicit check instead.
	 */
	if (!*path || (fp = fopen(path, "r")) == NULL) {
		stat_validity_clear(&shallow_stat);
		is_shallow = 0;
		return is_shallow;
	}
	stat_validity_update(&shallow_stat, fileno(fp));
	is_shallow = 1;

	while (fgets(buf, sizeof(buf), fp)) {
		struct object_id oid;
		if (get_oid_hex(buf, &oid))
			die("bad shallow line: %s", buf);
		register_shallow(&oid);
	}
	fclose(fp);
	return is_shallow;
}

struct commit_list *get_shallow_commits(struct object_array *heads, int depth,
		int shallow_flag, int not_shallow_flag)
{
	int i = 0, cur_depth = 0;
	struct commit_list *result = NULL;
	struct object_array stack = OBJECT_ARRAY_INIT;
	struct commit *commit = NULL;
	struct commit_graft *graft;

	while (commit || i < heads->nr || stack.nr) {
		struct commit_list *p;
		if (!commit) {
			if (i < heads->nr) {
				commit = (struct commit *)
					deref_tag(heads->objects[i++].item, NULL, 0);
				if (!commit || commit->object.type != OBJ_COMMIT) {
					commit = NULL;
					continue;
				}
				if (!commit->util)
					commit->util = xmalloc(sizeof(int));
				*(int *)commit->util = 0;
				cur_depth = 0;
			} else {
				commit = (struct commit *)
					stack.objects[--stack.nr].item;
				cur_depth = *(int *)commit->util;
			}
		}
		parse_commit_or_die(commit);
		cur_depth++;
		if ((depth != INFINITE_DEPTH && cur_depth >= depth) ||
		    (is_repository_shallow() && !commit->parents &&
		     (graft = lookup_commit_graft(&commit->object.oid)) != NULL &&
		     graft->nr_parent < 0)) {
			commit_list_insert(commit, &result);
			commit->object.flags |= shallow_flag;
			commit = NULL;
			continue;
		}
		commit->object.flags |= not_shallow_flag;
		for (p = commit->parents, commit = NULL; p; p = p->next) {
			if (!p->item->util) {
				int *pointer = xmalloc(sizeof(int));
				p->item->util = pointer;
				*pointer =  cur_depth;
			} else {
				int *pointer = p->item->util;
				if (cur_depth >= *pointer)
					continue;
				*pointer = cur_depth;
			}
			if (p->next)
				add_object_array(&p->item->object,
						NULL, &stack);
			else {
				commit = p->item;
				cur_depth = *(int *)commit->util;
			}
		}
	}

	return result;
}

static void show_commit(struct commit *commit, void *data)
{
	commit_list_insert(commit, data);
}

/*
 * Given rev-list arguments, run rev-list. All reachable commits
 * except border ones are marked with not_shallow_flag. Border commits
 * are marked with shallow_flag. The list of border/shallow commits
 * are also returned.
 */
struct commit_list *get_shallow_commits_by_rev_list(int ac, const char **av,
						    int shallow_flag,
						    int not_shallow_flag)
{
	struct commit_list *result = NULL, *p;
	struct commit_list *not_shallow_list = NULL;
	struct rev_info revs;
	int both_flags = shallow_flag | not_shallow_flag;

	/*
	 * SHALLOW (excluded) and NOT_SHALLOW (included) should not be
	 * set at this point. But better be safe than sorry.
	 */
	clear_object_flags(both_flags);

	is_repository_shallow(); /* make sure shallows are read */

	init_revisions(&revs, NULL);
	save_commit_buffer = 0;
	setup_revisions(ac, av, &revs, NULL);

	if (prepare_revision_walk(&revs))
		die("revision walk setup failed");
	traverse_commit_list(&revs, show_commit, NULL, &not_shallow_list);

	/* Mark all reachable commits as NOT_SHALLOW */
	for (p = not_shallow_list; p; p = p->next)
		p->item->object.flags |= not_shallow_flag;

	/*
	 * mark border commits SHALLOW + NOT_SHALLOW.
	 * We cannot clear NOT_SHALLOW right now. Imagine border
	 * commit A is processed first, then commit B, whose parent is
	 * A, later. If NOT_SHALLOW on A is cleared at step 1, B
	 * itself is considered border at step 2, which is incorrect.
	 */
	for (p = not_shallow_list; p; p = p->next) {
		struct commit *c = p->item;
		struct commit_list *parent;

		if (parse_commit(c))
			die("unable to parse commit %s",
			    oid_to_hex(&c->object.oid));

		for (parent = c->parents; parent; parent = parent->next)
			if (!(parent->item->object.flags & not_shallow_flag)) {
				c->object.flags |= shallow_flag;
				commit_list_insert(c, &result);
				break;
			}
	}
	free_commit_list(not_shallow_list);

	/*
	 * Now we can clean up NOT_SHALLOW on border commits. Having
	 * both flags set can confuse the caller.
	 */
	for (p = result; p; p = p->next) {
		struct object *o = &p->item->object;
		if ((o->flags & both_flags) == both_flags)
			o->flags &= ~not_shallow_flag;
	}
	return result;
}

static void check_shallow_file_for_update(void)
{
	if (is_shallow == -1)
		die("BUG: shallow must be initialized by now");

	if (!stat_validity_check(&shallow_stat, git_path_shallow()))
		die("shallow file has changed since we read it");
}

#define SEEN_ONLY 1
#define VERBOSE   2

struct write_shallow_data {
	struct strbuf *out;
	int use_pack_protocol;
	int count;
	unsigned flags;
};

static int write_one_shallow(const struct commit_graft *graft, void *cb_data)
{
	struct write_shallow_data *data = cb_data;
	const char *hex = oid_to_hex(&graft->oid);
	if (graft->nr_parent != -1)
		return 0;
	if (data->flags & SEEN_ONLY) {
		struct commit *c = lookup_commit(&graft->oid);
		if (!c || !(c->object.flags & SEEN)) {
			if (data->flags & VERBOSE)
				printf("Removing %s from .git/shallow\n",
				       oid_to_hex(&c->object.oid));
			return 0;
		}
	}
	data->count++;
	if (data->use_pack_protocol)
		packet_buf_write(data->out, "shallow %s", hex);
	else {
		strbuf_addstr(data->out, hex);
		strbuf_addch(data->out, '\n');
	}
	return 0;
}

static int write_shallow_commits_1(struct strbuf *out, int use_pack_protocol,
				   const struct oid_array *extra,
				   unsigned flags)
{
	struct write_shallow_data data;
	int i;
	data.out = out;
	data.use_pack_protocol = use_pack_protocol;
	data.count = 0;
	data.flags = flags;
	for_each_commit_graft(write_one_shallow, &data);
	if (!extra)
		return data.count;
	for (i = 0; i < extra->nr; i++) {
		strbuf_addstr(out, oid_to_hex(extra->oid + i));
		strbuf_addch(out, '\n');
		data.count++;
	}
	return data.count;
}

int write_shallow_commits(struct strbuf *out, int use_pack_protocol,
			  const struct oid_array *extra)
{
	return write_shallow_commits_1(out, use_pack_protocol, extra, 0);
}

const char *setup_temporary_shallow(const struct oid_array *extra)
{
	static struct tempfile temp;
	struct strbuf sb = STRBUF_INIT;
	int fd;

	if (write_shallow_commits(&sb, 0, extra)) {
		fd = xmks_tempfile(&temp, git_path("shallow_XXXXXX"));

		if (write_in_full(fd, sb.buf, sb.len) != sb.len ||
		    close_tempfile(&temp) < 0)
			die_errno("failed to write to %s",
				  get_tempfile_path(&temp));
		strbuf_release(&sb);
		return get_tempfile_path(&temp);
	}
	/*
	 * is_repository_shallow() sees empty string as "no shallow
	 * file".
	 */
	return "";
}

void setup_alternate_shallow(struct lock_file *shallow_lock,
			     const char **alternate_shallow_file,
			     const struct oid_array *extra)
{
	struct strbuf sb = STRBUF_INIT;
	int fd;

	fd = hold_lock_file_for_update(shallow_lock, git_path_shallow(),
				       LOCK_DIE_ON_ERROR);
	check_shallow_file_for_update();
	if (write_shallow_commits(&sb, 0, extra)) {
		if (write_in_full(fd, sb.buf, sb.len) != sb.len)
			die_errno("failed to write to %s",
				  get_lock_file_path(shallow_lock));
		*alternate_shallow_file = get_lock_file_path(shallow_lock);
	} else
		/*
		 * is_repository_shallow() sees empty string as "no
		 * shallow file".
		 */
		*alternate_shallow_file = "";
	strbuf_release(&sb);
}

static int advertise_shallow_grafts_cb(const struct commit_graft *graft, void *cb)
{
	int fd = *(int *)cb;
	if (graft->nr_parent == -1)
		packet_write_fmt(fd, "shallow %s\n", oid_to_hex(&graft->oid));
	return 0;
}

void advertise_shallow_grafts(int fd)
{
	if (!is_repository_shallow())
		return;
	for_each_commit_graft(advertise_shallow_grafts_cb, &fd);
}

/*
 * mark_reachable_objects() should have been run prior to this and all
 * reachable commits marked as "SEEN".
 */
void prune_shallow(int show_only)
{
	static struct lock_file shallow_lock;
	struct strbuf sb = STRBUF_INIT;
	int fd;

	if (show_only) {
		write_shallow_commits_1(&sb, 0, NULL, SEEN_ONLY | VERBOSE);
		strbuf_release(&sb);
		return;
	}
	fd = hold_lock_file_for_update(&shallow_lock, git_path_shallow(),
				       LOCK_DIE_ON_ERROR);
	check_shallow_file_for_update();
	if (write_shallow_commits_1(&sb, 0, NULL, SEEN_ONLY)) {
		if (write_in_full(fd, sb.buf, sb.len) != sb.len)
			die_errno("failed to write to %s",
				  get_lock_file_path(&shallow_lock));
		commit_lock_file(&shallow_lock);
	} else {
		unlink(git_path_shallow());
		rollback_lock_file(&shallow_lock);
	}
	strbuf_release(&sb);
}

struct trace_key trace_shallow = TRACE_KEY_INIT(SHALLOW);

/*
 * Step 1, split sender shallow commits into "ours" and "theirs"
 * Step 2, clean "ours" based on .git/shallow
 */
void prepare_shallow_info(struct shallow_info *info, struct oid_array *sa)
{
	int i;
	trace_printf_key(&trace_shallow, "shallow: prepare_shallow_info\n");
	memset(info, 0, sizeof(*info));
	info->shallow = sa;
	if (!sa)
		return;
	ALLOC_ARRAY(info->ours, sa->nr);
	ALLOC_ARRAY(info->theirs, sa->nr);
	for (i = 0; i < sa->nr; i++) {
		if (has_object_file(sa->oid + i)) {
			struct commit_graft *graft;
			graft = lookup_commit_graft(&sa->oid[i]);
			if (graft && graft->nr_parent < 0)
				continue;
			info->ours[info->nr_ours++] = i;
		} else
			info->theirs[info->nr_theirs++] = i;
	}
}

void clear_shallow_info(struct shallow_info *info)
{
	free(info->ours);
	free(info->theirs);
}

/* Step 4, remove non-existent ones in "theirs" after getting the pack */

void remove_nonexistent_theirs_shallow(struct shallow_info *info)
{
	struct object_id *oid = info->shallow->oid;
	int i, dst;
	trace_printf_key(&trace_shallow, "shallow: remove_nonexistent_theirs_shallow\n");
	for (i = dst = 0; i < info->nr_theirs; i++) {
		if (i != dst)
			info->theirs[dst] = info->theirs[i];
		if (has_object_file(oid + info->theirs[i]))
			dst++;
	}
	info->nr_theirs = dst;
}

define_commit_slab(ref_bitmap, uint32_t *);

#define POOL_SIZE (512 * 1024)

struct paint_info {
	struct ref_bitmap ref_bitmap;
	unsigned nr_bits;
	char **pools;
	char *free, *end;
	unsigned pool_count;
};

static uint32_t *paint_alloc(struct paint_info *info)
{
	unsigned nr = DIV_ROUND_UP(info->nr_bits, 32);
	unsigned size = nr * sizeof(uint32_t);
	void *p;
	if (!info->pool_count || size > info->end - info->free) {
		if (size > POOL_SIZE)
			die("BUG: pool size too small for %d in paint_alloc()",
			    size);
		info->pool_count++;
		REALLOC_ARRAY(info->pools, info->pool_count);
		info->free = xmalloc(POOL_SIZE);
		info->pools[info->pool_count - 1] = info->free;
		info->end = info->free + POOL_SIZE;
	}
	p = info->free;
	info->free += size;
	return p;
}

/*
 * Given a commit SHA-1, walk down to parents until either SEEN,
 * UNINTERESTING or BOTTOM is hit. Set the id-th bit in ref_bitmap for
 * all walked commits.
 */
static void paint_down(struct paint_info *info, const struct object_id *oid,
		       unsigned int id)
{
	unsigned int i, nr;
	struct commit_list *head = NULL;
	int bitmap_nr = DIV_ROUND_UP(info->nr_bits, 32);
	size_t bitmap_size = st_mult(sizeof(uint32_t), bitmap_nr);
	struct commit *c = lookup_commit_reference_gently(oid, 1);
	uint32_t *tmp; /* to be freed before return */
	uint32_t *bitmap;

	if (!c)
		return;

	tmp = xmalloc(bitmap_size);
	bitmap = paint_alloc(info);
	memset(bitmap, 0, bitmap_size);
	bitmap[id / 32] |= (1U << (id % 32));
	commit_list_insert(c, &head);
	while (head) {
		struct commit_list *p;
		struct commit *c = pop_commit(&head);
		uint32_t **refs = ref_bitmap_at(&info->ref_bitmap, c);

		/* XXX check "UNINTERESTING" from pack bitmaps if available */
		if (c->object.flags & (SEEN | UNINTERESTING))
			continue;
		else
			c->object.flags |= SEEN;

		if (*refs == NULL)
			*refs = bitmap;
		else {
			memcpy(tmp, *refs, bitmap_size);
			for (i = 0; i < bitmap_nr; i++)
				tmp[i] |= bitmap[i];
			if (memcmp(tmp, *refs, bitmap_size)) {
				*refs = paint_alloc(info);
				memcpy(*refs, tmp, bitmap_size);
			}
		}

		if (c->object.flags & BOTTOM)
			continue;

		if (parse_commit(c))
			die("unable to parse commit %s",
			    oid_to_hex(&c->object.oid));

		for (p = c->parents; p; p = p->next) {
			if (p->item->object.flags & SEEN)
				continue;
			commit_list_insert(p->item, &head);
		}
	}

	nr = get_max_object_index();
	for (i = 0; i < nr; i++) {
		struct object *o = get_indexed_object(i);
		if (o && o->type == OBJ_COMMIT)
			o->flags &= ~SEEN;
	}

	free(tmp);
}

static int mark_uninteresting(const char *refname, const struct object_id *oid,
			      int flags, void *cb_data)
{
	struct commit *commit = lookup_commit_reference_gently(oid, 1);
	if (!commit)
		return 0;
	commit->object.flags |= UNINTERESTING;
	mark_parents_uninteresting(commit);
	return 0;
}

static void post_assign_shallow(struct shallow_info *info,
				struct ref_bitmap *ref_bitmap,
				int *ref_status);
/*
 * Step 6(+7), associate shallow commits with new refs
 *
 * info->ref must be initialized before calling this function.
 *
 * If used is not NULL, it's an array of info->shallow->nr
 * bitmaps. The n-th bit set in the m-th bitmap if ref[n] needs the
 * m-th shallow commit from info->shallow.
 *
 * If used is NULL, "ours" and "theirs" are updated. And if ref_status
 * is not NULL it's an array of ref->nr ints. ref_status[i] is true if
 * the ref needs some shallow commits from either info->ours or
 * info->theirs.
 */
void assign_shallow_commits_to_refs(struct shallow_info *info,
				    uint32_t **used, int *ref_status)
{
	struct object_id *oid = info->shallow->oid;
	struct oid_array *ref = info->ref;
	unsigned int i, nr;
	int *shallow, nr_shallow = 0;
	struct paint_info pi;

	trace_printf_key(&trace_shallow, "shallow: assign_shallow_commits_to_refs\n");
	ALLOC_ARRAY(shallow, info->nr_ours + info->nr_theirs);
	for (i = 0; i < info->nr_ours; i++)
		shallow[nr_shallow++] = info->ours[i];
	for (i = 0; i < info->nr_theirs; i++)
		shallow[nr_shallow++] = info->theirs[i];

	/*
	 * Prepare the commit graph to track what refs can reach what
	 * (new) shallow commits.
	 */
	nr = get_max_object_index();
	for (i = 0; i < nr; i++) {
		struct object *o = get_indexed_object(i);
		if (!o || o->type != OBJ_COMMIT)
			continue;

		o->flags &= ~(UNINTERESTING | BOTTOM | SEEN);
	}

	memset(&pi, 0, sizeof(pi));
	init_ref_bitmap(&pi.ref_bitmap);
	pi.nr_bits = ref->nr;

	/*
	 * "--not --all" to cut short the traversal if new refs
	 * connect to old refs. If not (e.g. force ref updates) it'll
	 * have to go down to the current shallow commits.
	 */
	head_ref(mark_uninteresting, NULL);
	for_each_ref(mark_uninteresting, NULL);

	/* Mark potential bottoms so we won't go out of bound */
	for (i = 0; i < nr_shallow; i++) {
		struct commit *c = lookup_commit(&oid[shallow[i]]);
		c->object.flags |= BOTTOM;
	}

	for (i = 0; i < ref->nr; i++)
		paint_down(&pi, ref->oid + i, i);

	if (used) {
		int bitmap_size = DIV_ROUND_UP(pi.nr_bits, 32) * sizeof(uint32_t);
		memset(used, 0, sizeof(*used) * info->shallow->nr);
		for (i = 0; i < nr_shallow; i++) {
			const struct commit *c = lookup_commit(&oid[shallow[i]]);
			uint32_t **map = ref_bitmap_at(&pi.ref_bitmap, c);
			if (*map)
				used[shallow[i]] = xmemdupz(*map, bitmap_size);
		}
		/*
		 * unreachable shallow commits are not removed from
		 * "ours" and "theirs". The user is supposed to run
		 * step 7 on every ref separately and not trust "ours"
		 * and "theirs" any more.
		 */
	} else
		post_assign_shallow(info, &pi.ref_bitmap, ref_status);

	clear_ref_bitmap(&pi.ref_bitmap);
	for (i = 0; i < pi.pool_count; i++)
		free(pi.pools[i]);
	free(pi.pools);
	free(shallow);
}

struct commit_array {
	struct commit **commits;
	int nr, alloc;
};

static int add_ref(const char *refname, const struct object_id *oid,
		   int flags, void *cb_data)
{
	struct commit_array *ca = cb_data;
	ALLOC_GROW(ca->commits, ca->nr + 1, ca->alloc);
	ca->commits[ca->nr] = lookup_commit_reference_gently(oid, 1);
	if (ca->commits[ca->nr])
		ca->nr++;
	return 0;
}

static void update_refstatus(int *ref_status, int nr, uint32_t *bitmap)
{
	unsigned int i;
	if (!ref_status)
		return;
	for (i = 0; i < nr; i++)
		if (bitmap[i / 32] & (1U << (i % 32)))
			ref_status[i]++;
}

/*
 * Step 7, reachability test on "ours" at commit level
 */
static void post_assign_shallow(struct shallow_info *info,
				struct ref_bitmap *ref_bitmap,
				int *ref_status)
{
	struct object_id *oid = info->shallow->oid;
	struct commit *c;
	uint32_t **bitmap;
	int dst, i, j;
	int bitmap_nr = DIV_ROUND_UP(info->ref->nr, 32);
	struct commit_array ca;

	trace_printf_key(&trace_shallow, "shallow: post_assign_shallow\n");
	if (ref_status)
		memset(ref_status, 0, sizeof(*ref_status) * info->ref->nr);

	/* Remove unreachable shallow commits from "theirs" */
	for (i = dst = 0; i < info->nr_theirs; i++) {
		if (i != dst)
			info->theirs[dst] = info->theirs[i];
		c = lookup_commit(&oid[info->theirs[i]]);
		bitmap = ref_bitmap_at(ref_bitmap, c);
		if (!*bitmap)
			continue;
		for (j = 0; j < bitmap_nr; j++)
			if (bitmap[0][j]) {
				update_refstatus(ref_status, info->ref->nr, *bitmap);
				dst++;
				break;
			}
	}
	info->nr_theirs = dst;

	memset(&ca, 0, sizeof(ca));
	head_ref(add_ref, &ca);
	for_each_ref(add_ref, &ca);

	/* Remove unreachable shallow commits from "ours" */
	for (i = dst = 0; i < info->nr_ours; i++) {
		if (i != dst)
			info->ours[dst] = info->ours[i];
		c = lookup_commit(&oid[info->ours[i]]);
		bitmap = ref_bitmap_at(ref_bitmap, c);
		if (!*bitmap)
			continue;
		for (j = 0; j < bitmap_nr; j++)
			if (bitmap[0][j] &&
			    /* Step 7, reachability test at commit level */
			    !in_merge_bases_many(c, ca.nr, ca.commits)) {
				update_refstatus(ref_status, info->ref->nr, *bitmap);
				dst++;
				break;
			}
	}
	info->nr_ours = dst;

	free(ca.commits);
}

/* (Delayed) step 7, reachability test at commit level */
int delayed_reachability_test(struct shallow_info *si, int c)
{
	if (si->need_reachability_test[c]) {
		struct commit *commit = lookup_commit(&si->shallow->oid[c]);

		if (!si->commits) {
			struct commit_array ca;

			memset(&ca, 0, sizeof(ca));
			head_ref(add_ref, &ca);
			for_each_ref(add_ref, &ca);
			si->commits = ca.commits;
			si->nr_commits = ca.nr;
		}

		si->reachable[c] = in_merge_bases_many(commit,
						       si->nr_commits,
						       si->commits);
		si->need_reachability_test[c] = 0;
	}
	return si->reachable[c];
}
