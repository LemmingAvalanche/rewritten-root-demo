#include "builtin.h"
#include "cache.h"
#include "attr.h"
#include "object.h"
#include "blob.h"
#include "commit.h"
#include "tag.h"
#include "tree.h"
#include "delta.h"
#include "pack.h"
#include "csum-file.h"
#include "tree-walk.h"
#include "diff.h"
#include "revision.h"
#include "list-objects.h"
#include "progress.h"

#ifdef THREADED_DELTA_SEARCH
#include <pthread.h>
#endif

static const char pack_usage[] = "\
git-pack-objects [{ -q | --progress | --all-progress }] \n\
	[--max-pack-size=N] [--local] [--incremental] \n\
	[--window=N] [--window-memory=N] [--depth=N] \n\
	[--no-reuse-delta] [--no-reuse-object] [--delta-base-offset] \n\
	[--threads=N] [--non-empty] [--revs [--unpacked | --all]*] [--reflog] \n\
	[--stdout | base-name] [--keep-unreachable] [<ref-list | <object-list]";

struct object_entry {
	struct pack_idx_entry idx;
	unsigned long size;	/* uncompressed size */
	struct packed_git *in_pack; 	/* already in pack */
	off_t in_pack_offset;
	struct object_entry *delta;	/* delta base object */
	struct object_entry *delta_child; /* deltified objects who bases me */
	struct object_entry *delta_sibling; /* other deltified objects who
					     * uses the same base as me
					     */
	void *delta_data;	/* cached delta (uncompressed) */
	unsigned long delta_size;	/* delta data size (uncompressed) */
	unsigned int hash;	/* name hint hash */
	enum object_type type;
	enum object_type in_pack_type;	/* could be delta */
	unsigned char in_pack_header_size;
	unsigned char preferred_base; /* we do not pack this, but is available
				       * to be used as the base object to delta
				       * objects against.
				       */
	unsigned char no_try_delta;
};

/*
 * Objects we are going to pack are collected in objects array (dynamically
 * expanded).  nr_objects & nr_alloc controls this array.  They are stored
 * in the order we see -- typically rev-list --objects order that gives us
 * nice "minimum seek" order.
 */
static struct object_entry *objects;
static struct object_entry **written_list;
static uint32_t nr_objects, nr_alloc, nr_result, nr_written;

static int non_empty;
static int no_reuse_delta, no_reuse_object, keep_unreachable;
static int local;
static int incremental;
static int allow_ofs_delta;
static const char *base_name;
static int progress = 1;
static int window = 10;
static uint32_t pack_size_limit;
static int depth = 50;
static int delta_search_threads = 1;
static int pack_to_stdout;
static int num_preferred_base;
static struct progress progress_state;
static int pack_compression_level = Z_DEFAULT_COMPRESSION;
static int pack_compression_seen;

static unsigned long delta_cache_size = 0;
static unsigned long max_delta_cache_size = 0;
static unsigned long cache_max_small_delta_size = 1000;

static unsigned long window_memory_limit = 0;

/*
 * The object names in objects array are hashed with this hashtable,
 * to help looking up the entry by object name.
 * This hashtable is built after all the objects are seen.
 */
static int *object_ix;
static int object_ix_hashsz;

/*
 * Pack index for existing packs give us easy access to the offsets into
 * corresponding pack file where each object's data starts, but the entries
 * do not store the size of the compressed representation (uncompressed
 * size is easily available by examining the pack entry header).  It is
 * also rather expensive to find the sha1 for an object given its offset.
 *
 * We build a hashtable of existing packs (pack_revindex), and keep reverse
 * index here -- pack index file is sorted by object name mapping to offset;
 * this pack_revindex[].revindex array is a list of offset/index_nr pairs
 * ordered by offset, so if you know the offset of an object, next offset
 * is where its packed representation ends and the index_nr can be used to
 * get the object sha1 from the main index.
 */
struct revindex_entry {
	off_t offset;
	unsigned int nr;
};
struct pack_revindex {
	struct packed_git *p;
	struct revindex_entry *revindex;
};
static struct  pack_revindex *pack_revindex;
static int pack_revindex_hashsz;

/*
 * stats
 */
static uint32_t written, written_delta;
static uint32_t reused, reused_delta;

static int pack_revindex_ix(struct packed_git *p)
{
	unsigned long ui = (unsigned long)p;
	int i;

	ui = ui ^ (ui >> 16); /* defeat structure alignment */
	i = (int)(ui % pack_revindex_hashsz);
	while (pack_revindex[i].p) {
		if (pack_revindex[i].p == p)
			return i;
		if (++i == pack_revindex_hashsz)
			i = 0;
	}
	return -1 - i;
}

static void prepare_pack_ix(void)
{
	int num;
	struct packed_git *p;
	for (num = 0, p = packed_git; p; p = p->next)
		num++;
	if (!num)
		return;
	pack_revindex_hashsz = num * 11;
	pack_revindex = xcalloc(sizeof(*pack_revindex), pack_revindex_hashsz);
	for (p = packed_git; p; p = p->next) {
		num = pack_revindex_ix(p);
		num = - 1 - num;
		pack_revindex[num].p = p;
	}
	/* revindex elements are lazily initialized */
}

static int cmp_offset(const void *a_, const void *b_)
{
	const struct revindex_entry *a = a_;
	const struct revindex_entry *b = b_;
	return (a->offset < b->offset) ? -1 : (a->offset > b->offset) ? 1 : 0;
}

/*
 * Ordered list of offsets of objects in the pack.
 */
static void prepare_pack_revindex(struct pack_revindex *rix)
{
	struct packed_git *p = rix->p;
	int num_ent = p->num_objects;
	int i;
	const char *index = p->index_data;

	rix->revindex = xmalloc(sizeof(*rix->revindex) * (num_ent + 1));
	index += 4 * 256;

	if (p->index_version > 1) {
		const uint32_t *off_32 =
			(uint32_t *)(index + 8 + p->num_objects * (20 + 4));
		const uint32_t *off_64 = off_32 + p->num_objects;
		for (i = 0; i < num_ent; i++) {
			uint32_t off = ntohl(*off_32++);
			if (!(off & 0x80000000)) {
				rix->revindex[i].offset = off;
			} else {
				rix->revindex[i].offset =
					((uint64_t)ntohl(*off_64++)) << 32;
				rix->revindex[i].offset |=
					ntohl(*off_64++);
			}
			rix->revindex[i].nr = i;
		}
	} else {
		for (i = 0; i < num_ent; i++) {
			uint32_t hl = *((uint32_t *)(index + 24 * i));
			rix->revindex[i].offset = ntohl(hl);
			rix->revindex[i].nr = i;
		}
	}

	/* This knows the pack format -- the 20-byte trailer
	 * follows immediately after the last object data.
	 */
	rix->revindex[num_ent].offset = p->pack_size - 20;
	rix->revindex[num_ent].nr = -1;
	qsort(rix->revindex, num_ent, sizeof(*rix->revindex), cmp_offset);
}

static struct revindex_entry * find_packed_object(struct packed_git *p,
						  off_t ofs)
{
	int num;
	int lo, hi;
	struct pack_revindex *rix;
	struct revindex_entry *revindex;
	num = pack_revindex_ix(p);
	if (num < 0)
		die("internal error: pack revindex uninitialized");
	rix = &pack_revindex[num];
	if (!rix->revindex)
		prepare_pack_revindex(rix);
	revindex = rix->revindex;
	lo = 0;
	hi = p->num_objects + 1;
	do {
		int mi = (lo + hi) / 2;
		if (revindex[mi].offset == ofs) {
			return revindex + mi;
		}
		else if (ofs < revindex[mi].offset)
			hi = mi;
		else
			lo = mi + 1;
	} while (lo < hi);
	die("internal error: pack revindex corrupt");
}

static const unsigned char *find_packed_object_name(struct packed_git *p,
						    off_t ofs)
{
	struct revindex_entry *entry = find_packed_object(p, ofs);
	return nth_packed_object_sha1(p, entry->nr);
}

static void *delta_against(void *buf, unsigned long size, struct object_entry *entry)
{
	unsigned long othersize, delta_size;
	enum object_type type;
	void *otherbuf = read_sha1_file(entry->delta->idx.sha1, &type, &othersize);
	void *delta_buf;

	if (!otherbuf)
		die("unable to read %s", sha1_to_hex(entry->delta->idx.sha1));
        delta_buf = diff_delta(otherbuf, othersize,
			       buf, size, &delta_size, 0);
        if (!delta_buf || delta_size != entry->delta_size)
		die("delta size changed");
        free(buf);
        free(otherbuf);
	return delta_buf;
}

/*
 * The per-object header is a pretty dense thing, which is
 *  - first byte: low four bits are "size", then three bits of "type",
 *    and the high bit is "size continues".
 *  - each byte afterwards: low seven bits are size continuation,
 *    with the high bit being "size continues"
 */
static int encode_header(enum object_type type, unsigned long size, unsigned char *hdr)
{
	int n = 1;
	unsigned char c;

	if (type < OBJ_COMMIT || type > OBJ_REF_DELTA)
		die("bad type %d", type);

	c = (type << 4) | (size & 15);
	size >>= 4;
	while (size) {
		*hdr++ = c | 0x80;
		c = size & 0x7f;
		size >>= 7;
		n++;
	}
	*hdr = c;
	return n;
}

/*
 * we are going to reuse the existing object data as is.  make
 * sure it is not corrupt.
 */
static int check_pack_inflate(struct packed_git *p,
		struct pack_window **w_curs,
		off_t offset,
		off_t len,
		unsigned long expect)
{
	z_stream stream;
	unsigned char fakebuf[4096], *in;
	int st;

	memset(&stream, 0, sizeof(stream));
	inflateInit(&stream);
	do {
		in = use_pack(p, w_curs, offset, &stream.avail_in);
		stream.next_in = in;
		stream.next_out = fakebuf;
		stream.avail_out = sizeof(fakebuf);
		st = inflate(&stream, Z_FINISH);
		offset += stream.next_in - in;
	} while (st == Z_OK || st == Z_BUF_ERROR);
	inflateEnd(&stream);
	return (st == Z_STREAM_END &&
		stream.total_out == expect &&
		stream.total_in == len) ? 0 : -1;
}

static int check_pack_crc(struct packed_git *p, struct pack_window **w_curs,
			  off_t offset, off_t len, unsigned int nr)
{
	const uint32_t *index_crc;
	uint32_t data_crc = crc32(0, Z_NULL, 0);

	do {
		unsigned int avail;
		void *data = use_pack(p, w_curs, offset, &avail);
		if (avail > len)
			avail = len;
		data_crc = crc32(data_crc, data, avail);
		offset += avail;
		len -= avail;
	} while (len);

	index_crc = p->index_data;
	index_crc += 2 + 256 + p->num_objects * (20/4) + nr;

	return data_crc != ntohl(*index_crc);
}

static void copy_pack_data(struct sha1file *f,
		struct packed_git *p,
		struct pack_window **w_curs,
		off_t offset,
		off_t len)
{
	unsigned char *in;
	unsigned int avail;

	while (len) {
		in = use_pack(p, w_curs, offset, &avail);
		if (avail > len)
			avail = (unsigned int)len;
		sha1write(f, in, avail);
		offset += avail;
		len -= avail;
	}
}

static unsigned long write_object(struct sha1file *f,
				  struct object_entry *entry,
				  off_t write_offset)
{
	unsigned long size;
	enum object_type type;
	void *buf;
	unsigned char header[10];
	unsigned char dheader[10];
	unsigned hdrlen;
	off_t datalen;
	enum object_type obj_type;
	int to_reuse = 0;
	/* write limit if limited packsize and not first object */
	unsigned long limit = pack_size_limit && nr_written ?
				pack_size_limit - write_offset : 0;
				/* no if no delta */
	int usable_delta =	!entry->delta ? 0 :
				/* yes if unlimited packfile */
				!pack_size_limit ? 1 :
				/* no if base written to previous pack */
				entry->delta->idx.offset == (off_t)-1 ? 0 :
				/* otherwise double-check written to this
				 * pack,  like we do below
				 */
				entry->delta->idx.offset ? 1 : 0;

	if (!pack_to_stdout)
		crc32_begin(f);

	obj_type = entry->type;
	if (no_reuse_object)
		to_reuse = 0;	/* explicit */
	else if (!entry->in_pack)
		to_reuse = 0;	/* can't reuse what we don't have */
	else if (obj_type == OBJ_REF_DELTA || obj_type == OBJ_OFS_DELTA)
				/* check_object() decided it for us ... */
		to_reuse = usable_delta;
				/* ... but pack split may override that */
	else if (obj_type != entry->in_pack_type)
		to_reuse = 0;	/* pack has delta which is unusable */
	else if (entry->delta)
		to_reuse = 0;	/* we want to pack afresh */
	else
		to_reuse = 1;	/* we have it in-pack undeltified,
				 * and we do not need to deltify it.
				 */

	if (!to_reuse) {
		z_stream stream;
		unsigned long maxsize;
		void *out;
		if (!usable_delta) {
			buf = read_sha1_file(entry->idx.sha1, &obj_type, &size);
			if (!buf)
				die("unable to read %s", sha1_to_hex(entry->idx.sha1));
		} else if (entry->delta_data) {
			size = entry->delta_size;
			buf = entry->delta_data;
			entry->delta_data = NULL;
			obj_type = (allow_ofs_delta && entry->delta->idx.offset) ?
				OBJ_OFS_DELTA : OBJ_REF_DELTA;
		} else {
			buf = read_sha1_file(entry->idx.sha1, &type, &size);
			if (!buf)
				die("unable to read %s", sha1_to_hex(entry->idx.sha1));
			buf = delta_against(buf, size, entry);
			size = entry->delta_size;
			obj_type = (allow_ofs_delta && entry->delta->idx.offset) ?
				OBJ_OFS_DELTA : OBJ_REF_DELTA;
		}
		/* compress the data to store and put compressed length in datalen */
		memset(&stream, 0, sizeof(stream));
		deflateInit(&stream, pack_compression_level);
		maxsize = deflateBound(&stream, size);
		out = xmalloc(maxsize);
		/* Compress it */
		stream.next_in = buf;
		stream.avail_in = size;
		stream.next_out = out;
		stream.avail_out = maxsize;
		while (deflate(&stream, Z_FINISH) == Z_OK)
			/* nothing */;
		deflateEnd(&stream);
		datalen = stream.total_out;
		deflateEnd(&stream);
		/*
		 * The object header is a byte of 'type' followed by zero or
		 * more bytes of length.
		 */
		hdrlen = encode_header(obj_type, size, header);

		if (obj_type == OBJ_OFS_DELTA) {
			/*
			 * Deltas with relative base contain an additional
			 * encoding of the relative offset for the delta
			 * base from this object's position in the pack.
			 */
			off_t ofs = entry->idx.offset - entry->delta->idx.offset;
			unsigned pos = sizeof(dheader) - 1;
			dheader[pos] = ofs & 127;
			while (ofs >>= 7)
				dheader[--pos] = 128 | (--ofs & 127);
			if (limit && hdrlen + sizeof(dheader) - pos + datalen + 20 >= limit) {
				free(out);
				free(buf);
				return 0;
			}
			sha1write(f, header, hdrlen);
			sha1write(f, dheader + pos, sizeof(dheader) - pos);
			hdrlen += sizeof(dheader) - pos;
		} else if (obj_type == OBJ_REF_DELTA) {
			/*
			 * Deltas with a base reference contain
			 * an additional 20 bytes for the base sha1.
			 */
			if (limit && hdrlen + 20 + datalen + 20 >= limit) {
				free(out);
				free(buf);
				return 0;
			}
			sha1write(f, header, hdrlen);
			sha1write(f, entry->delta->idx.sha1, 20);
			hdrlen += 20;
		} else {
			if (limit && hdrlen + datalen + 20 >= limit) {
				free(out);
				free(buf);
				return 0;
			}
			sha1write(f, header, hdrlen);
		}
		sha1write(f, out, datalen);
		free(out);
		free(buf);
	}
	else {
		struct packed_git *p = entry->in_pack;
		struct pack_window *w_curs = NULL;
		struct revindex_entry *revidx;
		off_t offset;

		if (entry->delta) {
			obj_type = (allow_ofs_delta && entry->delta->idx.offset) ?
				OBJ_OFS_DELTA : OBJ_REF_DELTA;
			reused_delta++;
		}
		hdrlen = encode_header(obj_type, entry->size, header);
		offset = entry->in_pack_offset;
		revidx = find_packed_object(p, offset);
		datalen = revidx[1].offset - offset;
		if (!pack_to_stdout && p->index_version > 1 &&
		    check_pack_crc(p, &w_curs, offset, datalen, revidx->nr))
			die("bad packed object CRC for %s", sha1_to_hex(entry->idx.sha1));
		offset += entry->in_pack_header_size;
		datalen -= entry->in_pack_header_size;
		if (obj_type == OBJ_OFS_DELTA) {
			off_t ofs = entry->idx.offset - entry->delta->idx.offset;
			unsigned pos = sizeof(dheader) - 1;
			dheader[pos] = ofs & 127;
			while (ofs >>= 7)
				dheader[--pos] = 128 | (--ofs & 127);
			if (limit && hdrlen + sizeof(dheader) - pos + datalen + 20 >= limit)
				return 0;
			sha1write(f, header, hdrlen);
			sha1write(f, dheader + pos, sizeof(dheader) - pos);
			hdrlen += sizeof(dheader) - pos;
		} else if (obj_type == OBJ_REF_DELTA) {
			if (limit && hdrlen + 20 + datalen + 20 >= limit)
				return 0;
			sha1write(f, header, hdrlen);
			sha1write(f, entry->delta->idx.sha1, 20);
			hdrlen += 20;
		} else {
			if (limit && hdrlen + datalen + 20 >= limit)
				return 0;
			sha1write(f, header, hdrlen);
		}

		if (!pack_to_stdout && p->index_version == 1 &&
		    check_pack_inflate(p, &w_curs, offset, datalen, entry->size))
			die("corrupt packed object for %s", sha1_to_hex(entry->idx.sha1));
		copy_pack_data(f, p, &w_curs, offset, datalen);
		unuse_pack(&w_curs);
		reused++;
	}
	if (usable_delta)
		written_delta++;
	written++;
	if (!pack_to_stdout)
		entry->idx.crc32 = crc32_end(f);
	return hdrlen + datalen;
}

static off_t write_one(struct sha1file *f,
			       struct object_entry *e,
			       off_t offset)
{
	unsigned long size;

	/* offset is non zero if object is written already. */
	if (e->idx.offset || e->preferred_base)
		return offset;

	/* if we are deltified, write out base object first. */
	if (e->delta) {
		offset = write_one(f, e->delta, offset);
		if (!offset)
			return 0;
	}

	e->idx.offset = offset;
	size = write_object(f, e, offset);
	if (!size) {
		e->idx.offset = 0;
		return 0;
	}
	written_list[nr_written++] = e;

	/* make sure off_t is sufficiently large not to wrap */
	if (offset > offset + size)
		die("pack too large for current definition of off_t");
	return offset + size;
}

/* forward declaration for write_pack_file */
static int adjust_perm(const char *path, mode_t mode);

static void write_pack_file(void)
{
	uint32_t i = 0, j;
	struct sha1file *f;
	off_t offset, offset_one, last_obj_offset = 0;
	struct pack_header hdr;
	int do_progress = progress >> pack_to_stdout;
	uint32_t nr_remaining = nr_result;

	if (do_progress)
		start_progress(&progress_state, "Writing objects", nr_result);
	written_list = xmalloc(nr_objects * sizeof(struct object_entry *));

	do {
		unsigned char sha1[20];
		char *pack_tmp_name = NULL;

		if (pack_to_stdout) {
			f = sha1fd(1, "<stdout>");
		} else {
			char tmpname[PATH_MAX];
			int fd;
			snprintf(tmpname, sizeof(tmpname),
				 "%s/tmp_pack_XXXXXX", get_object_directory());
			fd = xmkstemp(tmpname);
			pack_tmp_name = xstrdup(tmpname);
			f = sha1fd(fd, pack_tmp_name);
		}

		hdr.hdr_signature = htonl(PACK_SIGNATURE);
		hdr.hdr_version = htonl(PACK_VERSION);
		hdr.hdr_entries = htonl(nr_remaining);
		sha1write(f, &hdr, sizeof(hdr));
		offset = sizeof(hdr);
		nr_written = 0;
		for (; i < nr_objects; i++) {
			last_obj_offset = offset;
			offset_one = write_one(f, objects + i, offset);
			if (!offset_one)
				break;
			offset = offset_one;
			if (do_progress)
				display_progress(&progress_state, written);
		}

		/*
		 * Did we write the wrong # entries in the header?
		 * If so, rewrite it like in fast-import
		 */
		if (pack_to_stdout || nr_written == nr_remaining) {
			sha1close(f, sha1, 1);
		} else {
			int fd = sha1close(f, NULL, 0);
			fixup_pack_header_footer(fd, sha1, pack_tmp_name, nr_written);
			close(fd);
		}

		if (!pack_to_stdout) {
			mode_t mode = umask(0);
			char *idx_tmp_name, tmpname[PATH_MAX];

			umask(mode);
			mode = 0444 & ~mode;

			idx_tmp_name = write_idx_file(NULL,
					(struct pack_idx_entry **) written_list,
					nr_written, sha1);
			snprintf(tmpname, sizeof(tmpname), "%s-%s.pack",
				 base_name, sha1_to_hex(sha1));
			if (adjust_perm(pack_tmp_name, mode))
				die("unable to make temporary pack file readable: %s",
				    strerror(errno));
			if (rename(pack_tmp_name, tmpname))
				die("unable to rename temporary pack file: %s",
				    strerror(errno));
			snprintf(tmpname, sizeof(tmpname), "%s-%s.idx",
				 base_name, sha1_to_hex(sha1));
			if (adjust_perm(idx_tmp_name, mode))
				die("unable to make temporary index file readable: %s",
				    strerror(errno));
			if (rename(idx_tmp_name, tmpname))
				die("unable to rename temporary index file: %s",
				    strerror(errno));
			free(idx_tmp_name);
			free(pack_tmp_name);
			puts(sha1_to_hex(sha1));
		}

		/* mark written objects as written to previous pack */
		for (j = 0; j < nr_written; j++) {
			written_list[j]->idx.offset = (off_t)-1;
		}
		nr_remaining -= nr_written;
	} while (nr_remaining && i < nr_objects);

	free(written_list);
	if (do_progress)
		stop_progress(&progress_state);
	if (written != nr_result)
		die("wrote %u objects while expecting %u", written, nr_result);
	/*
	 * We have scanned through [0 ... i).  Since we have written
	 * the correct number of objects,  the remaining [i ... nr_objects)
	 * items must be either already written (due to out-of-order delta base)
	 * or a preferred base.  Count those which are neither and complain if any.
	 */
	for (j = 0; i < nr_objects; i++) {
		struct object_entry *e = objects + i;
		j += !e->idx.offset && !e->preferred_base;
	}
	if (j)
		die("wrote %u objects as expected but %u unwritten", written, j);
}

static int locate_object_entry_hash(const unsigned char *sha1)
{
	int i;
	unsigned int ui;
	memcpy(&ui, sha1, sizeof(unsigned int));
	i = ui % object_ix_hashsz;
	while (0 < object_ix[i]) {
		if (!hashcmp(sha1, objects[object_ix[i] - 1].idx.sha1))
			return i;
		if (++i == object_ix_hashsz)
			i = 0;
	}
	return -1 - i;
}

static struct object_entry *locate_object_entry(const unsigned char *sha1)
{
	int i;

	if (!object_ix_hashsz)
		return NULL;

	i = locate_object_entry_hash(sha1);
	if (0 <= i)
		return &objects[object_ix[i]-1];
	return NULL;
}

static void rehash_objects(void)
{
	uint32_t i;
	struct object_entry *oe;

	object_ix_hashsz = nr_objects * 3;
	if (object_ix_hashsz < 1024)
		object_ix_hashsz = 1024;
	object_ix = xrealloc(object_ix, sizeof(int) * object_ix_hashsz);
	memset(object_ix, 0, sizeof(int) * object_ix_hashsz);
	for (i = 0, oe = objects; i < nr_objects; i++, oe++) {
		int ix = locate_object_entry_hash(oe->idx.sha1);
		if (0 <= ix)
			continue;
		ix = -1 - ix;
		object_ix[ix] = i + 1;
	}
}

static unsigned name_hash(const char *name)
{
	unsigned char c;
	unsigned hash = 0;

	if (!name)
		return 0;

	/*
	 * This effectively just creates a sortable number from the
	 * last sixteen non-whitespace characters. Last characters
	 * count "most", so things that end in ".c" sort together.
	 */
	while ((c = *name++) != 0) {
		if (isspace(c))
			continue;
		hash = (hash >> 2) + (c << 24);
	}
	return hash;
}

static void setup_delta_attr_check(struct git_attr_check *check)
{
	static struct git_attr *attr_delta;

	if (!attr_delta)
		attr_delta = git_attr("delta", 5);

	check[0].attr = attr_delta;
}

static int no_try_delta(const char *path)
{
	struct git_attr_check check[1];

	setup_delta_attr_check(check);
	if (git_checkattr(path, ARRAY_SIZE(check), check))
		return 0;
	if (ATTR_FALSE(check->value))
		return 1;
	return 0;
}

static int add_object_entry(const unsigned char *sha1, enum object_type type,
			    const char *name, int exclude)
{
	struct object_entry *entry;
	struct packed_git *p, *found_pack = NULL;
	off_t found_offset = 0;
	int ix;
	unsigned hash = name_hash(name);

	ix = nr_objects ? locate_object_entry_hash(sha1) : -1;
	if (ix >= 0) {
		if (exclude) {
			entry = objects + object_ix[ix] - 1;
			if (!entry->preferred_base)
				nr_result--;
			entry->preferred_base = 1;
		}
		return 0;
	}

	for (p = packed_git; p; p = p->next) {
		off_t offset = find_pack_entry_one(sha1, p);
		if (offset) {
			if (!found_pack) {
				found_offset = offset;
				found_pack = p;
			}
			if (exclude)
				break;
			if (incremental)
				return 0;
			if (local && !p->pack_local)
				return 0;
		}
	}

	if (nr_objects >= nr_alloc) {
		nr_alloc = (nr_alloc  + 1024) * 3 / 2;
		objects = xrealloc(objects, nr_alloc * sizeof(*entry));
	}

	entry = objects + nr_objects++;
	memset(entry, 0, sizeof(*entry));
	hashcpy(entry->idx.sha1, sha1);
	entry->hash = hash;
	if (type)
		entry->type = type;
	if (exclude)
		entry->preferred_base = 1;
	else
		nr_result++;
	if (found_pack) {
		entry->in_pack = found_pack;
		entry->in_pack_offset = found_offset;
	}

	if (object_ix_hashsz * 3 <= nr_objects * 4)
		rehash_objects();
	else
		object_ix[-1 - ix] = nr_objects;

	if (progress)
		display_progress(&progress_state, nr_objects);

	if (name && no_try_delta(name))
		entry->no_try_delta = 1;

	return 1;
}

struct pbase_tree_cache {
	unsigned char sha1[20];
	int ref;
	int temporary;
	void *tree_data;
	unsigned long tree_size;
};

static struct pbase_tree_cache *(pbase_tree_cache[256]);
static int pbase_tree_cache_ix(const unsigned char *sha1)
{
	return sha1[0] % ARRAY_SIZE(pbase_tree_cache);
}
static int pbase_tree_cache_ix_incr(int ix)
{
	return (ix+1) % ARRAY_SIZE(pbase_tree_cache);
}

static struct pbase_tree {
	struct pbase_tree *next;
	/* This is a phony "cache" entry; we are not
	 * going to evict it nor find it through _get()
	 * mechanism -- this is for the toplevel node that
	 * would almost always change with any commit.
	 */
	struct pbase_tree_cache pcache;
} *pbase_tree;

static struct pbase_tree_cache *pbase_tree_get(const unsigned char *sha1)
{
	struct pbase_tree_cache *ent, *nent;
	void *data;
	unsigned long size;
	enum object_type type;
	int neigh;
	int my_ix = pbase_tree_cache_ix(sha1);
	int available_ix = -1;

	/* pbase-tree-cache acts as a limited hashtable.
	 * your object will be found at your index or within a few
	 * slots after that slot if it is cached.
	 */
	for (neigh = 0; neigh < 8; neigh++) {
		ent = pbase_tree_cache[my_ix];
		if (ent && !hashcmp(ent->sha1, sha1)) {
			ent->ref++;
			return ent;
		}
		else if (((available_ix < 0) && (!ent || !ent->ref)) ||
			 ((0 <= available_ix) &&
			  (!ent && pbase_tree_cache[available_ix])))
			available_ix = my_ix;
		if (!ent)
			break;
		my_ix = pbase_tree_cache_ix_incr(my_ix);
	}

	/* Did not find one.  Either we got a bogus request or
	 * we need to read and perhaps cache.
	 */
	data = read_sha1_file(sha1, &type, &size);
	if (!data)
		return NULL;
	if (type != OBJ_TREE) {
		free(data);
		return NULL;
	}

	/* We need to either cache or return a throwaway copy */

	if (available_ix < 0)
		ent = NULL;
	else {
		ent = pbase_tree_cache[available_ix];
		my_ix = available_ix;
	}

	if (!ent) {
		nent = xmalloc(sizeof(*nent));
		nent->temporary = (available_ix < 0);
	}
	else {
		/* evict and reuse */
		free(ent->tree_data);
		nent = ent;
	}
	hashcpy(nent->sha1, sha1);
	nent->tree_data = data;
	nent->tree_size = size;
	nent->ref = 1;
	if (!nent->temporary)
		pbase_tree_cache[my_ix] = nent;
	return nent;
}

static void pbase_tree_put(struct pbase_tree_cache *cache)
{
	if (!cache->temporary) {
		cache->ref--;
		return;
	}
	free(cache->tree_data);
	free(cache);
}

static int name_cmp_len(const char *name)
{
	int i;
	for (i = 0; name[i] && name[i] != '\n' && name[i] != '/'; i++)
		;
	return i;
}

static void add_pbase_object(struct tree_desc *tree,
			     const char *name,
			     int cmplen,
			     const char *fullname)
{
	struct name_entry entry;
	int cmp;

	while (tree_entry(tree,&entry)) {
		if (S_ISGITLINK(entry.mode))
			continue;
		cmp = tree_entry_len(entry.path, entry.sha1) != cmplen ? 1 :
		      memcmp(name, entry.path, cmplen);
		if (cmp > 0)
			continue;
		if (cmp < 0)
			return;
		if (name[cmplen] != '/') {
			add_object_entry(entry.sha1,
					 S_ISDIR(entry.mode) ? OBJ_TREE : OBJ_BLOB,
					 fullname, 1);
			return;
		}
		if (S_ISDIR(entry.mode)) {
			struct tree_desc sub;
			struct pbase_tree_cache *tree;
			const char *down = name+cmplen+1;
			int downlen = name_cmp_len(down);

			tree = pbase_tree_get(entry.sha1);
			if (!tree)
				return;
			init_tree_desc(&sub, tree->tree_data, tree->tree_size);

			add_pbase_object(&sub, down, downlen, fullname);
			pbase_tree_put(tree);
		}
	}
}

static unsigned *done_pbase_paths;
static int done_pbase_paths_num;
static int done_pbase_paths_alloc;
static int done_pbase_path_pos(unsigned hash)
{
	int lo = 0;
	int hi = done_pbase_paths_num;
	while (lo < hi) {
		int mi = (hi + lo) / 2;
		if (done_pbase_paths[mi] == hash)
			return mi;
		if (done_pbase_paths[mi] < hash)
			hi = mi;
		else
			lo = mi + 1;
	}
	return -lo-1;
}

static int check_pbase_path(unsigned hash)
{
	int pos = (!done_pbase_paths) ? -1 : done_pbase_path_pos(hash);
	if (0 <= pos)
		return 1;
	pos = -pos - 1;
	if (done_pbase_paths_alloc <= done_pbase_paths_num) {
		done_pbase_paths_alloc = alloc_nr(done_pbase_paths_alloc);
		done_pbase_paths = xrealloc(done_pbase_paths,
					    done_pbase_paths_alloc *
					    sizeof(unsigned));
	}
	done_pbase_paths_num++;
	if (pos < done_pbase_paths_num)
		memmove(done_pbase_paths + pos + 1,
			done_pbase_paths + pos,
			(done_pbase_paths_num - pos - 1) * sizeof(unsigned));
	done_pbase_paths[pos] = hash;
	return 0;
}

static void add_preferred_base_object(const char *name)
{
	struct pbase_tree *it;
	int cmplen;
	unsigned hash = name_hash(name);

	if (!num_preferred_base || check_pbase_path(hash))
		return;

	cmplen = name_cmp_len(name);
	for (it = pbase_tree; it; it = it->next) {
		if (cmplen == 0) {
			add_object_entry(it->pcache.sha1, OBJ_TREE, NULL, 1);
		}
		else {
			struct tree_desc tree;
			init_tree_desc(&tree, it->pcache.tree_data, it->pcache.tree_size);
			add_pbase_object(&tree, name, cmplen, name);
		}
	}
}

static void add_preferred_base(unsigned char *sha1)
{
	struct pbase_tree *it;
	void *data;
	unsigned long size;
	unsigned char tree_sha1[20];

	if (window <= num_preferred_base++)
		return;

	data = read_object_with_reference(sha1, tree_type, &size, tree_sha1);
	if (!data)
		return;

	for (it = pbase_tree; it; it = it->next) {
		if (!hashcmp(it->pcache.sha1, tree_sha1)) {
			free(data);
			return;
		}
	}

	it = xcalloc(1, sizeof(*it));
	it->next = pbase_tree;
	pbase_tree = it;

	hashcpy(it->pcache.sha1, tree_sha1);
	it->pcache.tree_data = data;
	it->pcache.tree_size = size;
}

static void check_object(struct object_entry *entry)
{
	if (entry->in_pack) {
		struct packed_git *p = entry->in_pack;
		struct pack_window *w_curs = NULL;
		const unsigned char *base_ref = NULL;
		struct object_entry *base_entry;
		unsigned long used, used_0;
		unsigned int avail;
		off_t ofs;
		unsigned char *buf, c;

		buf = use_pack(p, &w_curs, entry->in_pack_offset, &avail);

		/*
		 * We want in_pack_type even if we do not reuse delta
		 * since non-delta representations could still be reused.
		 */
		used = unpack_object_header_gently(buf, avail,
						   &entry->in_pack_type,
						   &entry->size);

		/*
		 * Determine if this is a delta and if so whether we can
		 * reuse it or not.  Otherwise let's find out as cheaply as
		 * possible what the actual type and size for this object is.
		 */
		switch (entry->in_pack_type) {
		default:
			/* Not a delta hence we've already got all we need. */
			entry->type = entry->in_pack_type;
			entry->in_pack_header_size = used;
			unuse_pack(&w_curs);
			return;
		case OBJ_REF_DELTA:
			if (!no_reuse_delta && !entry->preferred_base)
				base_ref = use_pack(p, &w_curs,
						entry->in_pack_offset + used, NULL);
			entry->in_pack_header_size = used + 20;
			break;
		case OBJ_OFS_DELTA:
			buf = use_pack(p, &w_curs,
				       entry->in_pack_offset + used, NULL);
			used_0 = 0;
			c = buf[used_0++];
			ofs = c & 127;
			while (c & 128) {
				ofs += 1;
				if (!ofs || MSB(ofs, 7))
					die("delta base offset overflow in pack for %s",
					    sha1_to_hex(entry->idx.sha1));
				c = buf[used_0++];
				ofs = (ofs << 7) + (c & 127);
			}
			if (ofs >= entry->in_pack_offset)
				die("delta base offset out of bound for %s",
				    sha1_to_hex(entry->idx.sha1));
			ofs = entry->in_pack_offset - ofs;
			if (!no_reuse_delta && !entry->preferred_base)
				base_ref = find_packed_object_name(p, ofs);
			entry->in_pack_header_size = used + used_0;
			break;
		}

		if (base_ref && (base_entry = locate_object_entry(base_ref))) {
			/*
			 * If base_ref was set above that means we wish to
			 * reuse delta data, and we even found that base
			 * in the list of objects we want to pack. Goodie!
			 *
			 * Depth value does not matter - find_deltas() will
			 * never consider reused delta as the base object to
			 * deltify other objects against, in order to avoid
			 * circular deltas.
			 */
			entry->type = entry->in_pack_type;
			entry->delta = base_entry;
			entry->delta_sibling = base_entry->delta_child;
			base_entry->delta_child = entry;
			unuse_pack(&w_curs);
			return;
		}

		if (entry->type) {
			/*
			 * This must be a delta and we already know what the
			 * final object type is.  Let's extract the actual
			 * object size from the delta header.
			 */
			entry->size = get_size_from_delta(p, &w_curs,
					entry->in_pack_offset + entry->in_pack_header_size);
			unuse_pack(&w_curs);
			return;
		}

		/*
		 * No choice but to fall back to the recursive delta walk
		 * with sha1_object_info() to find about the object type
		 * at this point...
		 */
		unuse_pack(&w_curs);
	}

	entry->type = sha1_object_info(entry->idx.sha1, &entry->size);
	if (entry->type < 0)
		die("unable to get type of object %s",
		    sha1_to_hex(entry->idx.sha1));
}

static int pack_offset_sort(const void *_a, const void *_b)
{
	const struct object_entry *a = *(struct object_entry **)_a;
	const struct object_entry *b = *(struct object_entry **)_b;

	/* avoid filesystem trashing with loose objects */
	if (!a->in_pack && !b->in_pack)
		return hashcmp(a->idx.sha1, b->idx.sha1);

	if (a->in_pack < b->in_pack)
		return -1;
	if (a->in_pack > b->in_pack)
		return 1;
	return a->in_pack_offset < b->in_pack_offset ? -1 :
			(a->in_pack_offset > b->in_pack_offset);
}

static void get_object_details(void)
{
	uint32_t i;
	struct object_entry **sorted_by_offset;

	sorted_by_offset = xcalloc(nr_objects, sizeof(struct object_entry *));
	for (i = 0; i < nr_objects; i++)
		sorted_by_offset[i] = objects + i;
	qsort(sorted_by_offset, nr_objects, sizeof(*sorted_by_offset), pack_offset_sort);

	prepare_pack_ix();
	for (i = 0; i < nr_objects; i++)
		check_object(sorted_by_offset[i]);
	free(sorted_by_offset);
}

static int type_size_sort(const void *_a, const void *_b)
{
	const struct object_entry *a = *(struct object_entry **)_a;
	const struct object_entry *b = *(struct object_entry **)_b;

	if (a->type < b->type)
		return -1;
	if (a->type > b->type)
		return 1;
	if (a->hash < b->hash)
		return -1;
	if (a->hash > b->hash)
		return 1;
	if (a->preferred_base < b->preferred_base)
		return -1;
	if (a->preferred_base > b->preferred_base)
		return 1;
	if (a->size < b->size)
		return -1;
	if (a->size > b->size)
		return 1;
	return a > b ? -1 : (a < b);  /* newest last */
}

struct unpacked {
	struct object_entry *entry;
	void *data;
	struct delta_index *index;
	unsigned depth;
};

static int delta_cacheable(unsigned long src_size, unsigned long trg_size,
			   unsigned long delta_size)
{
	if (max_delta_cache_size && delta_cache_size + delta_size > max_delta_cache_size)
		return 0;

	if (delta_size < cache_max_small_delta_size)
		return 1;

	/* cache delta, if objects are large enough compared to delta size */
	if ((src_size >> 20) + (trg_size >> 21) > (delta_size >> 10))
		return 1;

	return 0;
}

#ifdef THREADED_DELTA_SEARCH

static pthread_mutex_t read_mutex = PTHREAD_MUTEX_INITIALIZER;
#define read_lock()		pthread_mutex_lock(&read_mutex)
#define read_unlock()		pthread_mutex_unlock(&read_mutex)

static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
#define cache_lock()		pthread_mutex_lock(&cache_mutex)
#define cache_unlock()		pthread_mutex_unlock(&cache_mutex)

static pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
#define progress_lock()		pthread_mutex_lock(&progress_mutex)
#define progress_unlock()	pthread_mutex_unlock(&progress_mutex)

#else

#define read_lock()		(void)0
#define read_unlock()		(void)0
#define cache_lock()		(void)0
#define cache_unlock()		(void)0
#define progress_lock()		(void)0
#define progress_unlock()	(void)0

#endif

/*
 * We search for deltas _backwards_ in a list sorted by type and
 * by size, so that we see progressively smaller and smaller files.
 * That's because we prefer deltas to be from the bigger file
 * to the smaller - deletes are potentially cheaper, but perhaps
 * more importantly, the bigger file is likely the more recent
 * one.
 */
static int try_delta(struct unpacked *trg, struct unpacked *src,
		     unsigned max_depth, unsigned long *mem_usage)
{
	struct object_entry *trg_entry = trg->entry;
	struct object_entry *src_entry = src->entry;
	unsigned long trg_size, src_size, delta_size, sizediff, max_size, sz;
	unsigned ref_depth;
	enum object_type type;
	void *delta_buf;

	/* Don't bother doing diffs between different types */
	if (trg_entry->type != src_entry->type)
		return -1;

	/*
	 * We do not bother to try a delta that we discarded
	 * on an earlier try, but only when reusing delta data.
	 */
	if (!no_reuse_delta && trg_entry->in_pack &&
	    trg_entry->in_pack == src_entry->in_pack &&
	    trg_entry->in_pack_type != OBJ_REF_DELTA &&
	    trg_entry->in_pack_type != OBJ_OFS_DELTA)
		return 0;

	/* Let's not bust the allowed depth. */
	if (src->depth >= max_depth)
		return 0;

	/* Now some size filtering heuristics. */
	trg_size = trg_entry->size;
	if (!trg_entry->delta) {
		max_size = trg_size/2 - 20;
		ref_depth = 1;
	} else {
		max_size = trg_entry->delta_size;
		ref_depth = trg->depth;
	}
	max_size = max_size * (max_depth - src->depth) /
						(max_depth - ref_depth + 1);
	if (max_size == 0)
		return 0;
	src_size = src_entry->size;
	sizediff = src_size < trg_size ? trg_size - src_size : 0;
	if (sizediff >= max_size)
		return 0;
	if (trg_size < src_size / 32)
		return 0;

	/* Load data if not already done */
	if (!trg->data) {
		read_lock();
		trg->data = read_sha1_file(trg_entry->idx.sha1, &type, &sz);
		read_unlock();
		if (!trg->data)
			die("object %s cannot be read",
			    sha1_to_hex(trg_entry->idx.sha1));
		if (sz != trg_size)
			die("object %s inconsistent object length (%lu vs %lu)",
			    sha1_to_hex(trg_entry->idx.sha1), sz, trg_size);
		*mem_usage += sz;
	}
	if (!src->data) {
		read_lock();
		src->data = read_sha1_file(src_entry->idx.sha1, &type, &sz);
		read_unlock();
		if (!src->data)
			die("object %s cannot be read",
			    sha1_to_hex(src_entry->idx.sha1));
		if (sz != src_size)
			die("object %s inconsistent object length (%lu vs %lu)",
			    sha1_to_hex(src_entry->idx.sha1), sz, src_size);
		*mem_usage += sz;
	}
	if (!src->index) {
		src->index = create_delta_index(src->data, src_size);
		if (!src->index) {
			static int warned = 0;
			if (!warned++)
				warning("suboptimal pack - out of memory");
			return 0;
		}
		*mem_usage += sizeof_delta_index(src->index);
	}

	delta_buf = create_delta(src->index, trg->data, trg_size, &delta_size, max_size);
	if (!delta_buf)
		return 0;

	if (trg_entry->delta) {
		/* Prefer only shallower same-sized deltas. */
		if (delta_size == trg_entry->delta_size &&
		    src->depth + 1 >= trg->depth) {
			free(delta_buf);
			return 0;
		}
	}

	trg_entry->delta = src_entry;
	trg_entry->delta_size = delta_size;
	trg->depth = src->depth + 1;

	/*
	 * Handle memory allocation outside of the cache
	 * accounting lock.  Compiler will optimize the strangeness
	 * away when THREADED_DELTA_SEARCH is not defined.
	 */
	if (trg_entry->delta_data)
		free(trg_entry->delta_data);
	cache_lock();
	if (trg_entry->delta_data) {
		delta_cache_size -= trg_entry->delta_size;
		trg_entry->delta_data = NULL;
	}
	if (delta_cacheable(src_size, trg_size, delta_size)) {
		delta_cache_size += trg_entry->delta_size;
		cache_unlock();
		trg_entry->delta_data = xrealloc(delta_buf, delta_size);
	} else {
		cache_unlock();
		free(delta_buf);
	}

	return 1;
}

static unsigned int check_delta_limit(struct object_entry *me, unsigned int n)
{
	struct object_entry *child = me->delta_child;
	unsigned int m = n;
	while (child) {
		unsigned int c = check_delta_limit(child, n + 1);
		if (m < c)
			m = c;
		child = child->delta_sibling;
	}
	return m;
}

static unsigned long free_unpacked(struct unpacked *n)
{
	unsigned long freed_mem = sizeof_delta_index(n->index);
	free_delta_index(n->index);
	n->index = NULL;
	if (n->data) {
		freed_mem += n->entry->size;
		free(n->data);
		n->data = NULL;
	}
	n->entry = NULL;
	n->depth = 0;
	return freed_mem;
}

static void find_deltas(struct object_entry **list, unsigned list_size,
			int window, int depth, unsigned *processed)
{
	uint32_t i = list_size, idx = 0, count = 0;
	unsigned int array_size = window * sizeof(struct unpacked);
	struct unpacked *array;
	unsigned long mem_usage = 0;

	array = xmalloc(array_size);
	memset(array, 0, array_size);

	do {
		struct object_entry *entry = list[--i];
		struct unpacked *n = array + idx;
		int j, max_depth, best_base = -1;

		mem_usage -= free_unpacked(n);
		n->entry = entry;

		while (window_memory_limit &&
		       mem_usage > window_memory_limit &&
		       count > 1) {
			uint32_t tail = (idx + window - count) % window;
			mem_usage -= free_unpacked(array + tail);
			count--;
		}

		/* We do not compute delta to *create* objects we are not
		 * going to pack.
		 */
		if (entry->preferred_base)
			goto next;

		progress_lock();
		(*processed)++;
		if (progress)
			display_progress(&progress_state, *processed);
		progress_unlock();

		/*
		 * If the current object is at pack edge, take the depth the
		 * objects that depend on the current object into account
		 * otherwise they would become too deep.
		 */
		max_depth = depth;
		if (entry->delta_child) {
			max_depth -= check_delta_limit(entry, 0);
			if (max_depth <= 0)
				goto next;
		}

		j = window;
		while (--j > 0) {
			int ret;
			uint32_t other_idx = idx + j;
			struct unpacked *m;
			if (other_idx >= window)
				other_idx -= window;
			m = array + other_idx;
			if (!m->entry)
				break;
			ret = try_delta(n, m, max_depth, &mem_usage);
			if (ret < 0)
				break;
			else if (ret > 0)
				best_base = other_idx;
		}

		/* if we made n a delta, and if n is already at max
		 * depth, leaving it in the window is pointless.  we
		 * should evict it first.
		 */
		if (entry->delta && depth <= n->depth)
			continue;

		/*
		 * Move the best delta base up in the window, after the
		 * currently deltified object, to keep it longer.  It will
		 * be the first base object to be attempted next.
		 */
		if (entry->delta) {
			struct unpacked swap = array[best_base];
			int dist = (window + idx - best_base) % window;
			int dst = best_base;
			while (dist--) {
				int src = (dst + 1) % window;
				array[dst] = array[src];
				dst = src;
			}
			array[dst] = swap;
		}

		next:
		idx++;
		if (count + 1 < window)
			count++;
		if (idx >= window)
			idx = 0;
	} while (i > 0);

	for (i = 0; i < window; ++i) {
		free_delta_index(array[i].index);
		free(array[i].data);
	}
	free(array);
}

#ifdef THREADED_DELTA_SEARCH

struct thread_params {
	pthread_t thread;
	struct object_entry **list;
	unsigned list_size;
	int window;
	int depth;
	unsigned *processed;
};

static pthread_mutex_t data_request  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t data_ready    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t data_provider = PTHREAD_MUTEX_INITIALIZER;
static struct thread_params *data_requester;

static void *threaded_find_deltas(void *arg)
{
	struct thread_params *me = arg;

	for (;;) {
		pthread_mutex_lock(&data_request);
		data_requester = me;
		pthread_mutex_unlock(&data_provider);
		pthread_mutex_lock(&data_ready);
		pthread_mutex_unlock(&data_request);

		if (!me->list_size)
			return NULL;

		find_deltas(me->list, me->list_size,
			    me->window, me->depth, me->processed);
	}
}

static void ll_find_deltas(struct object_entry **list, unsigned list_size,
			   int window, int depth, unsigned *processed)
{
	struct thread_params *target, p[delta_search_threads];
	int i, ret;
	unsigned chunk_size;

	if (delta_search_threads <= 1) {
		find_deltas(list, list_size, window, depth, processed);
		return;
	}

	pthread_mutex_lock(&data_provider);
	pthread_mutex_lock(&data_ready);

	for (i = 0; i < delta_search_threads; i++) {
		p[i].window = window;
		p[i].depth = depth;
		p[i].processed = processed;
		ret = pthread_create(&p[i].thread, NULL,
				     threaded_find_deltas, &p[i]);
		if (ret)
			die("unable to create thread: %s", strerror(ret));
	}

	/* this should be auto-tuned somehow */
	chunk_size = window * 1000;

	do {
		unsigned sublist_size = chunk_size;
		if (sublist_size > list_size)
			sublist_size = list_size;

		/* try to split chunks on "path" boundaries */
		while (sublist_size < list_size && list[sublist_size]->hash &&
		       list[sublist_size]->hash == list[sublist_size-1]->hash)
			sublist_size++;

		pthread_mutex_lock(&data_provider);
		target = data_requester;
		target->list = list;
		target->list_size = sublist_size;
		pthread_mutex_unlock(&data_ready);

		list += sublist_size;
		list_size -= sublist_size;
		if (!sublist_size) {
			pthread_join(target->thread, NULL);
			i--;
		}
	} while (i);
}

#else
#define ll_find_deltas find_deltas
#endif

static void prepare_pack(int window, int depth)
{
	struct object_entry **delta_list;
	uint32_t i, n, nr_deltas;

	get_object_details();

	if (!nr_objects || !window || !depth)
		return;

	delta_list = xmalloc(nr_objects * sizeof(*delta_list));
	nr_deltas = n = 0;

	for (i = 0; i < nr_objects; i++) {
		struct object_entry *entry = objects + i;

		if (entry->delta)
			/* This happens if we decided to reuse existing
			 * delta from a pack.  "!no_reuse_delta &&" is implied.
			 */
			continue;

		if (entry->size < 50)
			continue;

		if (entry->no_try_delta)
			continue;

		if (!entry->preferred_base)
			nr_deltas++;

		delta_list[n++] = entry;
	}

	if (nr_deltas && n > 1) {
		unsigned nr_done = 0;
		if (progress)
			start_progress(&progress_state, "Compressing objects",
					nr_deltas);
		qsort(delta_list, n, sizeof(*delta_list), type_size_sort);
		ll_find_deltas(delta_list, n, window+1, depth, &nr_done);
		if (progress)
			stop_progress(&progress_state);
		if (nr_done != nr_deltas)
			die("inconsistency with delta count");
	}
	free(delta_list);
}

static int git_pack_config(const char *k, const char *v)
{
	if(!strcmp(k, "pack.window")) {
		window = git_config_int(k, v);
		return 0;
	}
	if (!strcmp(k, "pack.windowmemory")) {
		window_memory_limit = git_config_ulong(k, v);
		return 0;
	}
	if (!strcmp(k, "pack.depth")) {
		depth = git_config_int(k, v);
		return 0;
	}
	if (!strcmp(k, "pack.compression")) {
		int level = git_config_int(k, v);
		if (level == -1)
			level = Z_DEFAULT_COMPRESSION;
		else if (level < 0 || level > Z_BEST_COMPRESSION)
			die("bad pack compression level %d", level);
		pack_compression_level = level;
		pack_compression_seen = 1;
		return 0;
	}
	if (!strcmp(k, "pack.deltacachesize")) {
		max_delta_cache_size = git_config_int(k, v);
		return 0;
	}
	if (!strcmp(k, "pack.deltacachelimit")) {
		cache_max_small_delta_size = git_config_int(k, v);
		return 0;
	}
	if (!strcmp(k, "pack.threads")) {
		delta_search_threads = git_config_int(k, v);
		if (delta_search_threads < 1)
			die("invalid number of threads specified (%d)",
			    delta_search_threads);
#ifndef THREADED_DELTA_SEARCH
		if (delta_search_threads > 1)
			warning("no threads support, ignoring %s", k);
#endif
		return 0;
	}
	return git_default_config(k, v);
}

static void read_object_list_from_stdin(void)
{
	char line[40 + 1 + PATH_MAX + 2];
	unsigned char sha1[20];

	for (;;) {
		if (!fgets(line, sizeof(line), stdin)) {
			if (feof(stdin))
				break;
			if (!ferror(stdin))
				die("fgets returned NULL, not EOF, not error!");
			if (errno != EINTR)
				die("fgets: %s", strerror(errno));
			clearerr(stdin);
			continue;
		}
		if (line[0] == '-') {
			if (get_sha1_hex(line+1, sha1))
				die("expected edge sha1, got garbage:\n %s",
				    line);
			add_preferred_base(sha1);
			continue;
		}
		if (get_sha1_hex(line, sha1))
			die("expected sha1, got garbage:\n %s", line);

		add_preferred_base_object(line+41);
		add_object_entry(sha1, 0, line+41, 0);
	}
}

#define OBJECT_ADDED (1u<<20)

static void show_commit(struct commit *commit)
{
	add_object_entry(commit->object.sha1, OBJ_COMMIT, NULL, 0);
	commit->object.flags |= OBJECT_ADDED;
}

static void show_object(struct object_array_entry *p)
{
	add_preferred_base_object(p->name);
	add_object_entry(p->item->sha1, p->item->type, p->name, 0);
	p->item->flags |= OBJECT_ADDED;
}

static void show_edge(struct commit *commit)
{
	add_preferred_base(commit->object.sha1);
}

struct in_pack_object {
	off_t offset;
	struct object *object;
};

struct in_pack {
	int alloc;
	int nr;
	struct in_pack_object *array;
};

static void mark_in_pack_object(struct object *object, struct packed_git *p, struct in_pack *in_pack)
{
	in_pack->array[in_pack->nr].offset = find_pack_entry_one(object->sha1, p);
	in_pack->array[in_pack->nr].object = object;
	in_pack->nr++;
}

/*
 * Compare the objects in the offset order, in order to emulate the
 * "git-rev-list --objects" output that produced the pack originally.
 */
static int ofscmp(const void *a_, const void *b_)
{
	struct in_pack_object *a = (struct in_pack_object *)a_;
	struct in_pack_object *b = (struct in_pack_object *)b_;

	if (a->offset < b->offset)
		return -1;
	else if (a->offset > b->offset)
		return 1;
	else
		return hashcmp(a->object->sha1, b->object->sha1);
}

static void add_objects_in_unpacked_packs(struct rev_info *revs)
{
	struct packed_git *p;
	struct in_pack in_pack;
	uint32_t i;

	memset(&in_pack, 0, sizeof(in_pack));

	for (p = packed_git; p; p = p->next) {
		const unsigned char *sha1;
		struct object *o;

		for (i = 0; i < revs->num_ignore_packed; i++) {
			if (matches_pack_name(p, revs->ignore_packed[i]))
				break;
		}
		if (revs->num_ignore_packed <= i)
			continue;
		if (open_pack_index(p))
			die("cannot open pack index");

		ALLOC_GROW(in_pack.array,
			   in_pack.nr + p->num_objects,
			   in_pack.alloc);

		for (i = 0; i < p->num_objects; i++) {
			sha1 = nth_packed_object_sha1(p, i);
			o = lookup_unknown_object(sha1);
			if (!(o->flags & OBJECT_ADDED))
				mark_in_pack_object(o, p, &in_pack);
			o->flags |= OBJECT_ADDED;
		}
	}

	if (in_pack.nr) {
		qsort(in_pack.array, in_pack.nr, sizeof(in_pack.array[0]),
		      ofscmp);
		for (i = 0; i < in_pack.nr; i++) {
			struct object *o = in_pack.array[i].object;
			add_object_entry(o->sha1, o->type, "", 0);
		}
	}
	free(in_pack.array);
}

static void get_object_list(int ac, const char **av)
{
	struct rev_info revs;
	char line[1000];
	int flags = 0;

	init_revisions(&revs, NULL);
	save_commit_buffer = 0;
	track_object_refs = 0;
	setup_revisions(ac, av, &revs, NULL);

	while (fgets(line, sizeof(line), stdin) != NULL) {
		int len = strlen(line);
		if (line[len - 1] == '\n')
			line[--len] = 0;
		if (!len)
			break;
		if (*line == '-') {
			if (!strcmp(line, "--not")) {
				flags ^= UNINTERESTING;
				continue;
			}
			die("not a rev '%s'", line);
		}
		if (handle_revision_arg(line, &revs, flags, 1))
			die("bad revision '%s'", line);
	}

	prepare_revision_walk(&revs);
	mark_edges_uninteresting(revs.commits, &revs, show_edge);
	traverse_commit_list(&revs, show_commit, show_object);

	if (keep_unreachable)
		add_objects_in_unpacked_packs(&revs);
}

static int adjust_perm(const char *path, mode_t mode)
{
	if (chmod(path, mode))
		return -1;
	return adjust_shared_perm(path);
}

int cmd_pack_objects(int argc, const char **argv, const char *prefix)
{
	int use_internal_rev_list = 0;
	int thin = 0;
	uint32_t i;
	const char **rp_av;
	int rp_ac_alloc = 64;
	int rp_ac;

	rp_av = xcalloc(rp_ac_alloc, sizeof(*rp_av));

	rp_av[0] = "pack-objects";
	rp_av[1] = "--objects"; /* --thin will make it --objects-edge */
	rp_ac = 2;

	git_config(git_pack_config);
	if (!pack_compression_seen && core_compression_seen)
		pack_compression_level = core_compression_level;

	progress = isatty(2);
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (*arg != '-')
			break;

		if (!strcmp("--non-empty", arg)) {
			non_empty = 1;
			continue;
		}
		if (!strcmp("--local", arg)) {
			local = 1;
			continue;
		}
		if (!strcmp("--incremental", arg)) {
			incremental = 1;
			continue;
		}
		if (!prefixcmp(arg, "--compression=")) {
			char *end;
			int level = strtoul(arg+14, &end, 0);
			if (!arg[14] || *end)
				usage(pack_usage);
			if (level == -1)
				level = Z_DEFAULT_COMPRESSION;
			else if (level < 0 || level > Z_BEST_COMPRESSION)
				die("bad pack compression level %d", level);
			pack_compression_level = level;
			continue;
		}
		if (!prefixcmp(arg, "--max-pack-size=")) {
			char *end;
			pack_size_limit = strtoul(arg+16, &end, 0) * 1024 * 1024;
			if (!arg[16] || *end)
				usage(pack_usage);
			continue;
		}
		if (!prefixcmp(arg, "--window=")) {
			char *end;
			window = strtoul(arg+9, &end, 0);
			if (!arg[9] || *end)
				usage(pack_usage);
			continue;
		}
		if (!prefixcmp(arg, "--window-memory=")) {
			if (!git_parse_ulong(arg+16, &window_memory_limit))
				usage(pack_usage);
			continue;
		}
		if (!prefixcmp(arg, "--threads=")) {
			char *end;
			delta_search_threads = strtoul(arg+10, &end, 0);
			if (!arg[10] || *end || delta_search_threads < 1)
				usage(pack_usage);
#ifndef THREADED_DELTA_SEARCH
			if (delta_search_threads > 1)
				warning("no threads support, "
					"ignoring %s", arg);
#endif
			continue;
		}
		if (!prefixcmp(arg, "--depth=")) {
			char *end;
			depth = strtoul(arg+8, &end, 0);
			if (!arg[8] || *end)
				usage(pack_usage);
			continue;
		}
		if (!strcmp("--progress", arg)) {
			progress = 1;
			continue;
		}
		if (!strcmp("--all-progress", arg)) {
			progress = 2;
			continue;
		}
		if (!strcmp("-q", arg)) {
			progress = 0;
			continue;
		}
		if (!strcmp("--no-reuse-delta", arg)) {
			no_reuse_delta = 1;
			continue;
		}
		if (!strcmp("--no-reuse-object", arg)) {
			no_reuse_object = no_reuse_delta = 1;
			continue;
		}
		if (!strcmp("--delta-base-offset", arg)) {
			allow_ofs_delta = 1;
			continue;
		}
		if (!strcmp("--stdout", arg)) {
			pack_to_stdout = 1;
			continue;
		}
		if (!strcmp("--revs", arg)) {
			use_internal_rev_list = 1;
			continue;
		}
		if (!strcmp("--keep-unreachable", arg)) {
			keep_unreachable = 1;
			continue;
		}
		if (!strcmp("--unpacked", arg) ||
		    !prefixcmp(arg, "--unpacked=") ||
		    !strcmp("--reflog", arg) ||
		    !strcmp("--all", arg)) {
			use_internal_rev_list = 1;
			if (rp_ac >= rp_ac_alloc - 1) {
				rp_ac_alloc = alloc_nr(rp_ac_alloc);
				rp_av = xrealloc(rp_av,
						 rp_ac_alloc * sizeof(*rp_av));
			}
			rp_av[rp_ac++] = arg;
			continue;
		}
		if (!strcmp("--thin", arg)) {
			use_internal_rev_list = 1;
			thin = 1;
			rp_av[1] = "--objects-edge";
			continue;
		}
		if (!prefixcmp(arg, "--index-version=")) {
			char *c;
			pack_idx_default_version = strtoul(arg + 16, &c, 10);
			if (pack_idx_default_version > 2)
				die("bad %s", arg);
			if (*c == ',')
				pack_idx_off32_limit = strtoul(c+1, &c, 0);
			if (*c || pack_idx_off32_limit & 0x80000000)
				die("bad %s", arg);
			continue;
		}
		usage(pack_usage);
	}

	/* Traditionally "pack-objects [options] base extra" failed;
	 * we would however want to take refs parameter that would
	 * have been given to upstream rev-list ourselves, which means
	 * we somehow want to say what the base name is.  So the
	 * syntax would be:
	 *
	 * pack-objects [options] base <refs...>
	 *
	 * in other words, we would treat the first non-option as the
	 * base_name and send everything else to the internal revision
	 * walker.
	 */

	if (!pack_to_stdout)
		base_name = argv[i++];

	if (pack_to_stdout != !base_name)
		usage(pack_usage);

	if (pack_to_stdout && pack_size_limit)
		die("--max-pack-size cannot be used to build a pack for transfer.");

	if (!pack_to_stdout && thin)
		die("--thin cannot be used to build an indexable pack.");

	prepare_packed_git();

	if (progress)
		start_progress(&progress_state, "Counting objects", 0);
	if (!use_internal_rev_list)
		read_object_list_from_stdin();
	else {
		rp_av[rp_ac] = NULL;
		get_object_list(rp_ac, rp_av);
	}
	if (progress)
		stop_progress(&progress_state);

	if (non_empty && !nr_result)
		return 0;
	if (nr_result)
		prepare_pack(window, depth);
	write_pack_file();
	if (progress)
		fprintf(stderr, "Total %u (delta %u), reused %u (delta %u)\n",
			written, written_delta, reused, reused_delta);
	return 0;
}
