/*
Format of STDIN stream:

  stream ::= cmd*;

  cmd ::= new_blob
        | new_commit
        | new_tag
        ;

  new_blob ::= 'blob' lf
	mark?
    file_content;
  file_content ::= data;

  new_commit ::= 'commit' sp ref_str lf
    mark?
    ('author' sp name '<' email '>' ts tz lf)?
    'committer' sp name '<' email '>' ts tz lf
    commit_msg
    ('from' sp (ref_str | hexsha1 | sha1exp_str | idnum) lf)?
    file_change*
    lf;
  commit_msg ::= data;

  file_change ::= 'M' sp mode sp (hexsha1 | idnum) sp path_str lf
                | 'D' sp path_str lf
                ;
  mode ::= '644' | '755';

  new_tag ::= 'tag' sp tag_str lf
    'from' sp (ref_str | hexsha1 | sha1exp_str | idnum) lf
	'tagger' sp name '<' email '>' ts tz lf
    tag_msg;
  tag_msg ::= data;

     # note: the first idnum in a stream should be 1 and subsequent
     # idnums should not have gaps between values as this will cause
     # the stream parser to reserve space for the gapped values.  An
	 # idnum can be updated in the future to a new object by issuing
     # a new mark directive with the old idnum.
	 #
  mark ::= 'mark' sp idnum lf;

     # note: declen indicates the length of binary_data in bytes.
     # declen does not include the lf preceeding or trailing the
     # binary data.
     #
  data ::= 'data' sp declen lf
    binary_data
	lf;

     # note: quoted strings are C-style quoting supporting \c for
     # common escapes of 'c' (e..g \n, \t, \\, \") or \nnn where nnn
	 # is the signed byte value in octal.  Note that the only
     # characters which must actually be escaped to protect the
     # stream formatting is: \, " and LF.  Otherwise these values
	 # are UTF8.
     #
  ref_str     ::= ref     | '"' quoted(ref)     '"' ;
  sha1exp_str ::= sha1exp | '"' quoted(sha1exp) '"' ;
  tag_str     ::= tag     | '"' quoted(tag)     '"' ;
  path_str    ::= path    | '"' quoted(path)    '"' ;

  declen ::= # unsigned 32 bit value, ascii base10 notation;
  binary_data ::= # file content, not interpreted;

  sp ::= # ASCII space character;
  lf ::= # ASCII newline (LF) character;

     # note: a colon (':') must precede the numerical value assigned to
	 # an idnum.  This is to distinguish it from a ref or tag name as
     # GIT does not permit ':' in ref or tag strings.
	 #
  idnum   ::= ':' declen;
  path    ::= # GIT style file path, e.g. "a/b/c";
  ref     ::= # GIT ref name, e.g. "refs/heads/MOZ_GECKO_EXPERIMENT";
  tag     ::= # GIT tag name, e.g. "FIREFOX_1_5";
  sha1exp ::= # Any valid GIT SHA1 expression;
  hexsha1 ::= # SHA1 in hexadecimal format;

     # note: name and email are UTF8 strings, however name must not
	 # contain '<' or lf and email must not contain any of the
     # following: '<', '>', lf.
	 #
  name  ::= # valid GIT author/committer name;
  email ::= # valid GIT author/committer email;
  ts    ::= # time since the epoch in seconds, ascii base10 notation;
  tz    ::= # GIT style timezone;
*/

#include "builtin.h"
#include "cache.h"
#include "object.h"
#include "blob.h"
#include "tree.h"
#include "delta.h"
#include "pack.h"
#include "refs.h"
#include "csum-file.h"
#include "strbuf.h"
#include "quote.h"

struct object_entry
{
	struct object_entry *next;
	enum object_type type;
	unsigned long offset;
	unsigned char sha1[20];
};

struct object_entry_pool
{
	struct object_entry_pool *next_pool;
	struct object_entry *next_free;
	struct object_entry *end;
	struct object_entry entries[FLEX_ARRAY]; /* more */
};

struct mark_set
{
	int shift;
	union {
		struct object_entry *marked[1024];
		struct mark_set *sets[1024];
	} data;
};

struct last_object
{
	void *data;
	unsigned int len;
	unsigned int depth;
	unsigned char sha1[20];
};

struct mem_pool
{
	struct mem_pool *next_pool;
	char *next_free;
	char *end;
	char space[FLEX_ARRAY]; /* more */
};

struct atom_str
{
	struct atom_str *next_atom;
	int str_len;
	char str_dat[FLEX_ARRAY]; /* more */
};

struct tree_content;
struct tree_entry
{
	struct tree_content *tree;
	struct atom_str* name;
	unsigned int mode;
	unsigned char sha1[20];
};

struct tree_content
{
	unsigned int entry_capacity; /* must match avail_tree_content */
	unsigned int entry_count;
	struct tree_entry *entries[FLEX_ARRAY]; /* more */
};

struct avail_tree_content
{
	unsigned int entry_capacity; /* must match tree_content */
	struct avail_tree_content *next_avail;
};

struct branch
{
	struct branch *table_next_branch;
	struct branch *active_next_branch;
	const char *name;
	unsigned long last_commit;
	struct tree_entry branch_tree;
	unsigned char sha1[20];
};

struct tag
{
	struct tag *next_tag;
	const char *name;
	unsigned char sha1[20];
};


/* Stats and misc. counters */
static unsigned long max_depth = 10;
static unsigned long alloc_count;
static unsigned long branch_count;
static unsigned long branch_load_count;
static unsigned long remap_count;
static unsigned long object_count;
static unsigned long duplicate_count;
static unsigned long marks_set_count;
static unsigned long object_count_by_type[9];
static unsigned long duplicate_count_by_type[9];

/* Memory pools */
static size_t mem_pool_alloc = 2*1024*1024 - sizeof(struct mem_pool);
static size_t total_allocd;
static struct mem_pool *mem_pool;

/* Atom management */
static unsigned int atom_table_sz = 4451;
static unsigned int atom_cnt;
static struct atom_str **atom_table;

/* The .pack file being generated */
static int pack_fd;
static unsigned long pack_size;
static unsigned char pack_sha1[20];
static void* pack_base;
static size_t pack_mlen;

/* Table of objects we've written. */
static unsigned int object_entry_alloc = 1000;
static struct object_entry_pool *blocks;
static struct object_entry *object_table[1 << 16];
static struct mark_set *marks;
static const char* mark_file;

/* Our last blob */
static struct last_object last_blob;

/* Tree management */
static unsigned int tree_entry_alloc = 1000;
static void *avail_tree_entry;
static unsigned int avail_tree_table_sz = 100;
static struct avail_tree_content **avail_tree_table;

/* Branch data */
static unsigned long max_active_branches = 5;
static unsigned long cur_active_branches;
static unsigned long branch_table_sz = 1039;
static struct branch **branch_table;
static struct branch *active_branches;

/* Tag data */
static struct tag *first_tag;
static struct tag *last_tag;

/* Input stream parsing */
static struct strbuf command_buf;
static unsigned long next_mark;
static FILE* branch_log;


static void alloc_objects(int cnt)
{
	struct object_entry_pool *b;

	b = xmalloc(sizeof(struct object_entry_pool)
		+ cnt * sizeof(struct object_entry));
	b->next_pool = blocks;
	b->next_free = b->entries;
	b->end = b->entries + cnt;
	blocks = b;
	alloc_count += cnt;
}

static struct object_entry* new_object(unsigned char *sha1)
{
	struct object_entry *e;

	if (blocks->next_free == blocks->end)
		alloc_objects(object_entry_alloc);

	e = blocks->next_free++;
	memcpy(e->sha1, sha1, sizeof(e->sha1));
	return e;
}

static struct object_entry* find_object(unsigned char *sha1)
{
	unsigned int h = sha1[0] << 8 | sha1[1];
	struct object_entry *e;
	for (e = object_table[h]; e; e = e->next)
		if (!memcmp(sha1, e->sha1, sizeof(e->sha1)))
			return e;
	return NULL;
}

static struct object_entry* insert_object(unsigned char *sha1)
{
	unsigned int h = sha1[0] << 8 | sha1[1];
	struct object_entry *e = object_table[h];
	struct object_entry *p = NULL;

	while (e) {
		if (!memcmp(sha1, e->sha1, sizeof(e->sha1)))
			return e;
		p = e;
		e = e->next;
	}

	e = new_object(sha1);
	e->next = NULL;
	e->offset = 0;
	if (p)
		p->next = e;
	else
		object_table[h] = e;
	return e;
}

static unsigned int hc_str(const char *s, size_t len)
{
	unsigned int r = 0;
	while (len-- > 0)
		r = r * 31 + *s++;
	return r;
}

static void* pool_alloc(size_t len)
{
	struct mem_pool *p;
	void *r;

	for (p = mem_pool; p; p = p->next_pool)
		if ((p->end - p->next_free >= len))
			break;

	if (!p) {
		if (len >= (mem_pool_alloc/2)) {
			total_allocd += len;
			return xmalloc(len);
		}
		total_allocd += sizeof(struct mem_pool) + mem_pool_alloc;
		p = xmalloc(sizeof(struct mem_pool) + mem_pool_alloc);
		p->next_pool = mem_pool;
		p->next_free = p->space;
		p->end = p->next_free + mem_pool_alloc;
		mem_pool = p;
	}

	r = p->next_free;
	/* round out to a pointer alignment */
	if (len & (sizeof(void*) - 1))
		len += sizeof(void*) - (len & (sizeof(void*) - 1));
	p->next_free += len;
	return r;
}

static void* pool_calloc(size_t count, size_t size)
{
	size_t len = count * size;
	void *r = pool_alloc(len);
	memset(r, 0, len);
	return r;
}

static char* pool_strdup(const char *s)
{
	char *r = pool_alloc(strlen(s) + 1);
	strcpy(r, s);
	return r;
}

static void insert_mark(unsigned long idnum, struct object_entry *oe)
{
	struct mark_set *s = marks;
	while ((idnum >> s->shift) >= 1024) {
		s = pool_calloc(1, sizeof(struct mark_set));
		s->shift = marks->shift + 10;
		s->data.sets[0] = marks;
		marks = s;
	}
	while (s->shift) {
		unsigned long i = idnum >> s->shift;
		idnum -= i << s->shift;
		if (!s->data.sets[i]) {
			s->data.sets[i] = pool_calloc(1, sizeof(struct mark_set));
			s->data.sets[i]->shift = s->shift - 10;
		}
		s = s->data.sets[i];
	}
	if (!s->data.marked[idnum])
		marks_set_count++;
	s->data.marked[idnum] = oe;
}

static struct object_entry* find_mark(unsigned long idnum)
{
	unsigned long orig_idnum = idnum;
	struct mark_set *s = marks;
	struct object_entry *oe = NULL;
	if ((idnum >> s->shift) < 1024) {
		while (s && s->shift) {
			unsigned long i = idnum >> s->shift;
			idnum -= i << s->shift;
			s = s->data.sets[i];
		}
		if (s)
			oe = s->data.marked[idnum];
	}
	if (!oe)
		die("mark :%lu not declared", orig_idnum);
	return oe;
}

static struct atom_str* to_atom(const char *s, size_t len)
{
	unsigned int hc = hc_str(s, len) % atom_table_sz;
	struct atom_str *c;

	for (c = atom_table[hc]; c; c = c->next_atom)
		if (c->str_len == len && !strncmp(s, c->str_dat, len))
			return c;

	c = pool_alloc(sizeof(struct atom_str) + len + 1);
	c->str_len = len;
	strncpy(c->str_dat, s, len);
	c->str_dat[len] = 0;
	c->next_atom = atom_table[hc];
	atom_table[hc] = c;
	atom_cnt++;
	return c;
}

static struct branch* lookup_branch(const char *name)
{
	unsigned int hc = hc_str(name, strlen(name)) % branch_table_sz;
	struct branch *b;

	for (b = branch_table[hc]; b; b = b->table_next_branch)
		if (!strcmp(name, b->name))
			return b;
	return NULL;
}

static struct branch* new_branch(const char *name)
{
	unsigned int hc = hc_str(name, strlen(name)) % branch_table_sz;
	struct branch* b = lookup_branch(name);

	if (b)
		die("Invalid attempt to create duplicate branch: %s", name);
	if (check_ref_format(name))
		die("Branch name doesn't conform to GIT standards: %s", name);

	b = pool_calloc(1, sizeof(struct branch));
	b->name = pool_strdup(name);
	b->table_next_branch = branch_table[hc];
	branch_table[hc] = b;
	branch_count++;
	return b;
}

static unsigned int hc_entries(unsigned int cnt)
{
	cnt = cnt & 7 ? (cnt / 8) + 1 : cnt / 8;
	return cnt < avail_tree_table_sz ? cnt : avail_tree_table_sz - 1;
}

static struct tree_content* new_tree_content(unsigned int cnt)
{
	struct avail_tree_content *f, *l = NULL;
	struct tree_content *t;
	unsigned int hc = hc_entries(cnt);

	for (f = avail_tree_table[hc]; f; l = f, f = f->next_avail)
		if (f->entry_capacity >= cnt)
			break;

	if (f) {
		if (l)
			l->next_avail = f->next_avail;
		else
			avail_tree_table[hc] = f->next_avail;
	} else {
		cnt = cnt & 7 ? ((cnt / 8) + 1) * 8 : cnt;
		f = pool_alloc(sizeof(*t) + sizeof(t->entries[0]) * cnt);
		f->entry_capacity = cnt;
	}

	t = (struct tree_content*)f;
	t->entry_count = 0;
	return t;
}

static void release_tree_entry(struct tree_entry *e);
static void release_tree_content(struct tree_content *t)
{
	struct avail_tree_content *f = (struct avail_tree_content*)t;
	unsigned int hc = hc_entries(f->entry_capacity);
	f->next_avail = avail_tree_table[hc];
	avail_tree_table[hc] = f;
}

static void release_tree_content_recursive(struct tree_content *t)
{
	unsigned int i;
	for (i = 0; i < t->entry_count; i++)
		release_tree_entry(t->entries[i]);
	release_tree_content(t);
}

static struct tree_content* grow_tree_content(
	struct tree_content *t,
	int amt)
{
	struct tree_content *r = new_tree_content(t->entry_count + amt);
	r->entry_count = t->entry_count;
	memcpy(r->entries,t->entries,t->entry_count*sizeof(t->entries[0]));
	release_tree_content(t);
	return r;
}

static struct tree_entry* new_tree_entry()
{
	struct tree_entry *e;

	if (!avail_tree_entry) {
		unsigned int n = tree_entry_alloc;
		total_allocd += n * sizeof(struct tree_entry);
		avail_tree_entry = e = xmalloc(n * sizeof(struct tree_entry));
		while (n-- > 1) {
			*((void**)e) = e + 1;
			e++;
		}
		*((void**)e) = NULL;
	}

	e = avail_tree_entry;
	avail_tree_entry = *((void**)e);
	return e;
}

static void release_tree_entry(struct tree_entry *e)
{
	if (e->tree)
		release_tree_content_recursive(e->tree);
	*((void**)e) = avail_tree_entry;
	avail_tree_entry = e;
}

static void yread(int fd, void *buffer, size_t length)
{
	ssize_t ret = 0;
	while (ret < length) {
		ssize_t size = xread(fd, (char *) buffer + ret, length - ret);
		if (!size)
			die("Read from descriptor %i: end of stream", fd);
		if (size < 0)
			die("Read from descriptor %i: %s", fd, strerror(errno));
		ret += size;
	}
}

static void ywrite(int fd, void *buffer, size_t length)
{
	ssize_t ret = 0;
	while (ret < length) {
		ssize_t size = xwrite(fd, (char *) buffer + ret, length - ret);
		if (!size)
			die("Write to descriptor %i: end of file", fd);
		if (size < 0)
			die("Write to descriptor %i: %s", fd, strerror(errno));
		ret += size;
	}
}

static size_t encode_header(
	enum object_type type,
	size_t size,
	unsigned char *hdr)
{
	int n = 1;
	unsigned char c;

	if (type < OBJ_COMMIT || type > OBJ_DELTA)
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

static int store_object(
	enum object_type type,
	void *dat,
	size_t datlen,
	struct last_object *last,
	unsigned char *sha1out,
	unsigned long mark)
{
	void *out, *delta;
	struct object_entry *e;
	unsigned char hdr[96];
	unsigned char sha1[20];
	unsigned long hdrlen, deltalen;
	SHA_CTX c;
	z_stream s;

	hdrlen = sprintf((char*)hdr,"%s %lu",type_names[type],datlen) + 1;
	SHA1_Init(&c);
	SHA1_Update(&c, hdr, hdrlen);
	SHA1_Update(&c, dat, datlen);
	SHA1_Final(sha1, &c);
	if (sha1out)
		memcpy(sha1out, sha1, sizeof(sha1));

	e = insert_object(sha1);
	if (mark)
		insert_mark(mark, e);
	if (e->offset) {
		duplicate_count++;
		duplicate_count_by_type[type]++;
		return 1;
	}
	e->type = type;
	e->offset = pack_size;
	object_count++;
	object_count_by_type[type]++;

	if (last && last->data && last->depth < max_depth)
		delta = diff_delta(last->data, last->len,
			dat, datlen,
			&deltalen, 0);
	else
		delta = 0;

	memset(&s, 0, sizeof(s));
	deflateInit(&s, zlib_compression_level);

	if (delta) {
		last->depth++;
		s.next_in = delta;
		s.avail_in = deltalen;
		hdrlen = encode_header(OBJ_DELTA, deltalen, hdr);
		ywrite(pack_fd, hdr, hdrlen);
		ywrite(pack_fd, last->sha1, sizeof(sha1));
		pack_size += hdrlen + sizeof(sha1);
	} else {
		if (last)
			last->depth = 0;
		s.next_in = dat;
		s.avail_in = datlen;
		hdrlen = encode_header(type, datlen, hdr);
		ywrite(pack_fd, hdr, hdrlen);
		pack_size += hdrlen;
	}

	s.avail_out = deflateBound(&s, s.avail_in);
	s.next_out = out = xmalloc(s.avail_out);
	while (deflate(&s, Z_FINISH) == Z_OK)
		/* nothing */;
	deflateEnd(&s);

	ywrite(pack_fd, out, s.total_out);
	pack_size += s.total_out;

	free(out);
	if (delta)
		free(delta);
	if (last) {
		if (last->data)
			free(last->data);
		last->data = dat;
		last->len = datlen;
		memcpy(last->sha1, sha1, sizeof(sha1));
	}
	return 0;
}

static void* map_pack(unsigned long offset)
{
	if (offset >= pack_size)
		die("object offset outside of pack file");
	if (offset >= pack_mlen) {
		if (pack_base)
			munmap(pack_base, pack_mlen);
		/* round out how much we map to 16 MB units */
		pack_mlen = pack_size;
		if (pack_mlen & ((1 << 24) - 1))
			pack_mlen = ((pack_mlen >> 24) + 1) << 24;
		pack_base = mmap(NULL,pack_mlen,PROT_READ,MAP_SHARED,pack_fd,0);
		if (pack_base == MAP_FAILED)
			die("Failed to map generated pack: %s", strerror(errno));
		remap_count++;
	}
	return (char*)pack_base + offset;
}

static unsigned long unpack_object_header(unsigned long offset,
	enum object_type *type,
	unsigned long *sizep)
{
	unsigned shift;
	unsigned char c;
	unsigned long size;

	c = *(unsigned char*)map_pack(offset++);
	*type = (c >> 4) & 7;
	size = c & 15;
	shift = 4;
	while (c & 0x80) {
		c = *(unsigned char*)map_pack(offset++);
		size += (c & 0x7f) << shift;
		shift += 7;
	}
	*sizep = size;
	return offset;
}

static void *unpack_non_delta_entry(unsigned long o, unsigned long sz)
{
	z_stream stream;
	unsigned char *result;

	result = xmalloc(sz + 1);
	result[sz] = 0;

	memset(&stream, 0, sizeof(stream));
	stream.next_in = map_pack(o);
	stream.avail_in = pack_mlen - o;
	stream.next_out = result;
	stream.avail_out = sz;

	inflateInit(&stream);
	for (;;) {
		int st = inflate(&stream, Z_FINISH);
		if (st == Z_STREAM_END)
			break;
		if (st == Z_OK) {
			o = stream.next_in - (unsigned char*)pack_base;
			stream.next_in = map_pack(o);
			stream.avail_in = pack_mlen - o;
			continue;
		}
		die("Error from zlib during inflate.");
	}
	inflateEnd(&stream);
	if (stream.total_out != sz)
		die("Error after inflate: sizes mismatch");
	return result;
}

static void *unpack_entry(unsigned long offset, unsigned long *sizep);

static void *unpack_delta_entry(unsigned long offset,
	unsigned long delta_size,
	unsigned long *sizep)
{
	struct object_entry *base_oe;
	unsigned char *base_sha1;
	void *delta_data, *base, *result;
	unsigned long base_size, result_size;

	base_sha1 = (unsigned char*)map_pack(offset + 20) - 20;
	base_oe = find_object(base_sha1);
	if (!base_oe)
		die("I'm broken; I can't find a base I know must be here.");
	base = unpack_entry(base_oe->offset, &base_size);
	delta_data = unpack_non_delta_entry(offset + 20, delta_size);
	result = patch_delta(base, base_size,
			     delta_data, delta_size,
			     &result_size);
	if (!result)
		die("failed to apply delta");
	free(delta_data);
	free(base);
	*sizep = result_size;
	return result;
}

static void *unpack_entry(unsigned long offset, unsigned long *sizep)
{
	unsigned long size;
	enum object_type kind;

	offset = unpack_object_header(offset, &kind, &size);
	switch (kind) {
	case OBJ_DELTA:
		return unpack_delta_entry(offset, size, sizep);
	case OBJ_COMMIT:
	case OBJ_TREE:
	case OBJ_BLOB:
	case OBJ_TAG:
		*sizep = size;
		return unpack_non_delta_entry(offset, size);
	default:
		die("I created an object I can't read!");
	}
}

static const char *get_mode(const char *str, unsigned int *modep)
{
	unsigned char c;
	unsigned int mode = 0;

	while ((c = *str++) != ' ') {
		if (c < '0' || c > '7')
			return NULL;
		mode = (mode << 3) + (c - '0');
	}
	*modep = mode;
	return str;
}

static void load_tree(struct tree_entry *root)
{
	struct object_entry *myoe;
	struct tree_content *t;
	unsigned long size;
	char *buf;
	const char *c;

	root->tree = t = new_tree_content(8);
	if (!memcmp(root->sha1, null_sha1, 20))
		return;

	myoe = find_object(root->sha1);
	if (myoe) {
		if (myoe->type != OBJ_TREE)
			die("Not a tree: %s", sha1_to_hex(root->sha1));
		buf = unpack_entry(myoe->offset, &size);
	} else {
		char type[20];
		buf = read_sha1_file(root->sha1, type, &size);
		if (!buf || strcmp(type, tree_type))
			die("Can't load tree %s", sha1_to_hex(root->sha1));
	}

	c = buf;
	while (c != (buf + size)) {
		struct tree_entry *e = new_tree_entry();

		if (t->entry_count == t->entry_capacity)
			root->tree = t = grow_tree_content(t, 8);
		t->entries[t->entry_count++] = e;

		e->tree = NULL;
		c = get_mode(c, &e->mode);
		if (!c)
			die("Corrupt mode in %s", sha1_to_hex(root->sha1));
		e->name = to_atom(c, strlen(c));
		c += e->name->str_len + 1;
		memcpy(e->sha1, c, sizeof(e->sha1));
		c += 20;
	}
	free(buf);
}

static int tecmp (const void *_a, const void *_b)
{
	struct tree_entry *a = *((struct tree_entry**)_a);
	struct tree_entry *b = *((struct tree_entry**)_b);
	return base_name_compare(
		a->name->str_dat, a->name->str_len, a->mode,
		b->name->str_dat, b->name->str_len, b->mode);
}

static void store_tree(struct tree_entry *root)
{
	struct tree_content *t = root->tree;
	unsigned int i;
	size_t maxlen;
	char *buf, *c;

	if (memcmp(root->sha1, null_sha1, 20))
		return;

	maxlen = 0;
	for (i = 0; i < t->entry_count; i++) {
		maxlen += t->entries[i]->name->str_len + 34;
		if (t->entries[i]->tree)
			store_tree(t->entries[i]);
	}

	qsort(t->entries, t->entry_count, sizeof(t->entries[0]), tecmp);
	buf = c = xmalloc(maxlen);
	for (i = 0; i < t->entry_count; i++) {
		struct tree_entry *e = t->entries[i];
		c += sprintf(c, "%o", e->mode);
		*c++ = ' ';
		strcpy(c, e->name->str_dat);
		c += e->name->str_len + 1;
		memcpy(c, e->sha1, 20);
		c += 20;
	}
	store_object(OBJ_TREE, buf, c - buf, NULL, root->sha1, 0);
	free(buf);
}

static int tree_content_set(
	struct tree_entry *root,
	const char *p,
	const unsigned char *sha1,
	const unsigned int mode)
{
	struct tree_content *t = root->tree;
	const char *slash1;
	unsigned int i, n;
	struct tree_entry *e;

	slash1 = strchr(p, '/');
	if (slash1)
		n = slash1 - p;
	else
		n = strlen(p);

	for (i = 0; i < t->entry_count; i++) {
		e = t->entries[i];
		if (e->name->str_len == n && !strncmp(p, e->name->str_dat, n)) {
			if (!slash1) {
				if (e->mode == mode && !memcmp(e->sha1, sha1, 20))
					return 0;
				e->mode = mode;
				memcpy(e->sha1, sha1, 20);
				if (e->tree) {
					release_tree_content_recursive(e->tree);
					e->tree = NULL;
				}
				memcpy(root->sha1, null_sha1, 20);
				return 1;
			}
			if (!S_ISDIR(e->mode)) {
				e->tree = new_tree_content(8);
				e->mode = S_IFDIR;
			}
			if (!e->tree)
				load_tree(e);
			if (tree_content_set(e, slash1 + 1, sha1, mode)) {
				memcpy(root->sha1, null_sha1, 20);
				return 1;
			}
			return 0;
		}
	}

	if (t->entry_count == t->entry_capacity)
		root->tree = t = grow_tree_content(t, 8);
	e = new_tree_entry();
	e->name = to_atom(p, n);
	t->entries[t->entry_count++] = e;
	if (slash1) {
		e->tree = new_tree_content(8);
		e->mode = S_IFDIR;
		tree_content_set(e, slash1 + 1, sha1, mode);
	} else {
		e->tree = NULL;
		e->mode = mode;
		memcpy(e->sha1, sha1, 20);
	}
	memcpy(root->sha1, null_sha1, 20);
	return 1;
}

static int tree_content_remove(struct tree_entry *root, const char *p)
{
	struct tree_content *t = root->tree;
	const char *slash1;
	unsigned int i, n;
	struct tree_entry *e;

	slash1 = strchr(p, '/');
	if (slash1)
		n = slash1 - p;
	else
		n = strlen(p);

	for (i = 0; i < t->entry_count; i++) {
		e = t->entries[i];
		if (e->name->str_len == n && !strncmp(p, e->name->str_dat, n)) {
			if (!slash1 || !S_ISDIR(e->mode))
				goto del_entry;
			if (!e->tree)
				load_tree(e);
			if (tree_content_remove(e, slash1 + 1)) {
				if (!e->tree->entry_count)
					goto del_entry;
				memcpy(root->sha1, null_sha1, 20);
				return 1;
			}
			return 0;
		}
	}
	return 0;

del_entry:
	for (i++; i < t->entry_count; i++)
		t->entries[i-1] = t->entries[i];
	t->entry_count--;
	release_tree_entry(e);
	memcpy(root->sha1, null_sha1, 20);
	return 1;
}

static void init_pack_header()
{
	struct pack_header hdr;

	hdr.hdr_signature = htonl(PACK_SIGNATURE);
	hdr.hdr_version = htonl(2);
	hdr.hdr_entries = 0;

	ywrite(pack_fd, &hdr, sizeof(hdr));
	pack_size = sizeof(hdr);
}

static void fixup_header_footer()
{
	SHA_CTX c;
	char hdr[8];
	unsigned long cnt;
	char *buf;
	size_t n;

	if (lseek(pack_fd, 0, SEEK_SET) != 0)
		die("Failed seeking to start: %s", strerror(errno));

	SHA1_Init(&c);
	yread(pack_fd, hdr, 8);
	SHA1_Update(&c, hdr, 8);

	cnt = htonl(object_count);
	SHA1_Update(&c, &cnt, 4);
	ywrite(pack_fd, &cnt, 4);

	buf = xmalloc(128 * 1024);
	for (;;) {
		n = xread(pack_fd, buf, 128 * 1024);
		if (n <= 0)
			break;
		SHA1_Update(&c, buf, n);
	}
	free(buf);

	SHA1_Final(pack_sha1, &c);
	ywrite(pack_fd, pack_sha1, sizeof(pack_sha1));
}

static int oecmp (const void *_a, const void *_b)
{
	struct object_entry *a = *((struct object_entry**)_a);
	struct object_entry *b = *((struct object_entry**)_b);
	return memcmp(a->sha1, b->sha1, sizeof(a->sha1));
}

static void write_index(const char *idx_name)
{
	struct sha1file *f;
	struct object_entry **idx, **c, **last;
	struct object_entry *e;
	struct object_entry_pool *o;
	unsigned int array[256];
	int i;

	/* Build the sorted table of object IDs. */
	idx = xmalloc(object_count * sizeof(struct object_entry*));
	c = idx;
	for (o = blocks; o; o = o->next_pool)
		for (e = o->entries; e != o->next_free; e++)
			*c++ = e;
	last = idx + object_count;
	qsort(idx, object_count, sizeof(struct object_entry*), oecmp);

	/* Generate the fan-out array. */
	c = idx;
	for (i = 0; i < 256; i++) {
		struct object_entry **next = c;;
		while (next < last) {
			if ((*next)->sha1[0] != i)
				break;
			next++;
		}
		array[i] = htonl(next - idx);
		c = next;
	}

	f = sha1create("%s", idx_name);
	sha1write(f, array, 256 * sizeof(int));
	for (c = idx; c != last; c++) {
		unsigned int offset = htonl((*c)->offset);
		sha1write(f, &offset, 4);
		sha1write(f, (*c)->sha1, sizeof((*c)->sha1));
	}
	sha1write(f, pack_sha1, sizeof(pack_sha1));
	sha1close(f, NULL, 1);
	free(idx);
}

static void dump_branches()
{
	static const char *msg = "fast-import";
	unsigned int i;
	struct branch *b;
	struct ref_lock *lock;

	for (i = 0; i < branch_table_sz; i++) {
		for (b = branch_table[i]; b; b = b->table_next_branch) {
			lock = lock_any_ref_for_update(b->name, NULL, 0);
			if (!lock || write_ref_sha1(lock, b->sha1, msg) < 0)
				die("Can't write %s", b->name);
		}
	}
}

static void dump_tags()
{
	static const char *msg = "fast-import";
	struct tag *t;
	struct ref_lock *lock;
	char path[PATH_MAX];

	for (t = first_tag; t; t = t->next_tag) {
		sprintf(path, "refs/tags/%s", t->name);
		lock = lock_any_ref_for_update(path, NULL, 0);
		if (!lock || write_ref_sha1(lock, t->sha1, msg) < 0)
			die("Can't write %s", path);
	}
}

static void dump_marks_helper(FILE *f,
	unsigned long base,
	struct mark_set *m)
{
	int k;
	if (m->shift) {
		for (k = 0; k < 1024; k++) {
			if (m->data.sets[k])
				dump_marks_helper(f, (base + k) << m->shift,
					m->data.sets[k]);
		}
	} else {
		for (k = 0; k < 1024; k++) {
			if (m->data.marked[k])
				fprintf(f, ":%lu %s\n", base + k,
					sha1_to_hex(m->data.marked[k]->sha1));
		}
	}
}

static void dump_marks()
{
	if (mark_file)
	{
		FILE *f = fopen(mark_file, "w");
		dump_marks_helper(f, 0, marks);
		fclose(f);
	}
}

static void read_next_command()
{
	read_line(&command_buf, stdin, '\n');
}

static void cmd_mark()
{
	if (!strncmp("mark :", command_buf.buf, 6)) {
		next_mark = strtoul(command_buf.buf + 6, NULL, 10);
		read_next_command();
	}
	else
		next_mark = 0;
}

static void* cmd_data (size_t *size)
{
	size_t n = 0;
	void *buffer;
	size_t length;

	if (strncmp("data ", command_buf.buf, 5))
		die("Expected 'data n' command, found: %s", command_buf.buf);

	length = strtoul(command_buf.buf + 5, NULL, 10);
	buffer = xmalloc(length);

	while (n < length) {
		size_t s = fread((char*)buffer + n, 1, length - n, stdin);
		if (!s && feof(stdin))
			die("EOF in data (%lu bytes remaining)", length - n);
		n += s;
	}

	if (fgetc(stdin) != '\n')
		die("An lf did not trail the binary data as expected.");

	*size = length;
	return buffer;
}

static void cmd_new_blob()
{
	size_t l;
	void *d;

	read_next_command();
	cmd_mark();
	d = cmd_data(&l);

	if (store_object(OBJ_BLOB, d, l, &last_blob, NULL, next_mark))
		free(d);
}

static void unload_one_branch()
{
	while (cur_active_branches
		&& cur_active_branches >= max_active_branches) {
		unsigned long min_commit = ULONG_MAX;
		struct branch *e, *l = NULL, *p = NULL;

		for (e = active_branches; e; e = e->active_next_branch) {
			if (e->last_commit < min_commit) {
				p = l;
				min_commit = e->last_commit;
			}
			l = e;
		}

		if (p) {
			e = p->active_next_branch;
			p->active_next_branch = e->active_next_branch;
		} else {
			e = active_branches;
			active_branches = e->active_next_branch;
		}
		e->active_next_branch = NULL;
		if (e->branch_tree.tree) {
			release_tree_content_recursive(e->branch_tree.tree);
			e->branch_tree.tree = NULL;
		}
		cur_active_branches--;
	}
}

static void load_branch(struct branch *b)
{
	load_tree(&b->branch_tree);
	b->active_next_branch = active_branches;
	active_branches = b;
	cur_active_branches++;
	branch_load_count++;
}

static void file_change_m(struct branch *b)
{
	const char *p = command_buf.buf + 2;
	char *p_uq;
	const char *endp;
	struct object_entry *oe;
	unsigned char sha1[20];
	unsigned int mode;
	char type[20];

	p = get_mode(p, &mode);
	if (!p)
		die("Corrupt mode: %s", command_buf.buf);
	switch (mode) {
	case S_IFREG | 0644:
	case S_IFREG | 0755:
	case S_IFLNK:
	case 0644:
	case 0755:
		/* ok */
		break;
	default:
		die("Corrupt mode: %s", command_buf.buf);
	}

	if (*p == ':') {
		char *x;
		oe = find_mark(strtoul(p + 1, &x, 10));
		p = x;
	} else {
		if (get_sha1_hex(p, sha1))
			die("Invalid SHA1: %s", command_buf.buf);
		oe = find_object(sha1);
		p += 40;
	}
	if (*p++ != ' ')
		die("Missing space after SHA1: %s", command_buf.buf);

	p_uq = unquote_c_style(p, &endp);
	if (p_uq) {
		if (*endp)
			die("Garbage after path in: %s", command_buf.buf);
		p = p_uq;
	}

	if (oe) {
		if (oe->type != OBJ_BLOB)
			die("Not a blob (actually a %s): %s",
				command_buf.buf, type_names[oe->type]);
	} else {
		if (sha1_object_info(sha1, type, NULL))
			die("Blob not found: %s", command_buf.buf);
		if (strcmp(blob_type, type))
			die("Not a blob (actually a %s): %s",
				command_buf.buf, type);
	}

	tree_content_set(&b->branch_tree, p, sha1, S_IFREG | mode);

	if (p_uq)
		free(p_uq);
}

static void file_change_d(struct branch *b)
{
	const char *p = command_buf.buf + 2;
	char *p_uq;
	const char *endp;

	p_uq = unquote_c_style(p, &endp);
	if (p_uq) {
		if (*endp)
			die("Garbage after path in: %s", command_buf.buf);
		p = p_uq;
	}
	tree_content_remove(&b->branch_tree, p);
	if (p_uq)
		free(p_uq);
}

static void cmd_from(struct branch *b)
{
	const char *from, *endp;
	char *str_uq;
	struct branch *s;

	if (strncmp("from ", command_buf.buf, 5))
		return;

	if (b->last_commit)
		die("Can't reinitailize branch %s", b->name);

	from = strchr(command_buf.buf, ' ') + 1;
	str_uq = unquote_c_style(from, &endp);
	if (str_uq) {
		if (*endp)
			die("Garbage after string in: %s", command_buf.buf);
		from = str_uq;
	}

	s = lookup_branch(from);
	if (b == s)
		die("Can't create a branch from itself: %s", b->name);
	else if (s) {
		memcpy(b->sha1, s->sha1, 20);
		memcpy(b->branch_tree.sha1, s->branch_tree.sha1, 20);
	} else if (*from == ':') {
		unsigned long idnum = strtoul(from + 1, NULL, 10);
		struct object_entry *oe = find_mark(idnum);
		unsigned long size;
		char *buf;
		if (oe->type != OBJ_COMMIT)
			die("Mark :%lu not a commit", idnum);
		memcpy(b->sha1, oe->sha1, 20);
		buf = unpack_entry(oe->offset, &size);
		if (!buf || size < 46)
			die("Not a valid commit: %s", from);
		if (memcmp("tree ", buf, 5)
			|| get_sha1_hex(buf + 5, b->branch_tree.sha1))
			die("The commit %s is corrupt", sha1_to_hex(b->sha1));
		free(buf);
	} else if (!get_sha1(from, b->sha1)) {
		if (!memcmp(b->sha1, null_sha1, 20))
			memcpy(b->branch_tree.sha1, null_sha1, 20);
		else {
			unsigned long size;
			char *buf;

			buf = read_object_with_reference(b->sha1,
				type_names[OBJ_COMMIT], &size, b->sha1);
			if (!buf || size < 46)
				die("Not a valid commit: %s", from);
			if (memcmp("tree ", buf, 5)
				|| get_sha1_hex(buf + 5, b->branch_tree.sha1))
				die("The commit %s is corrupt", sha1_to_hex(b->sha1));
			free(buf);
		}
	} else
		die("Invalid ref name or SHA1 expression: %s", from);

	read_next_command();
}

static void cmd_new_commit()
{
	struct branch *b;
	void *msg;
	size_t msglen;
	char *str_uq;
	const char *endp;
	char *sp;
	char *author = NULL;
	char *committer = NULL;
	char *body;

	/* Obtain the branch name from the rest of our command */
	sp = strchr(command_buf.buf, ' ') + 1;
	str_uq = unquote_c_style(sp, &endp);
	if (str_uq) {
		if (*endp)
			die("Garbage after ref in: %s", command_buf.buf);
		sp = str_uq;
	}
	b = lookup_branch(sp);
	if (!b)
		b = new_branch(sp);
	if (str_uq)
		free(str_uq);

	read_next_command();
	cmd_mark();
	if (!strncmp("author ", command_buf.buf, 7)) {
		author = strdup(command_buf.buf);
		read_next_command();
	}
	if (!strncmp("committer ", command_buf.buf, 10)) {
		committer = strdup(command_buf.buf);
		read_next_command();
	}
	if (!committer)
		die("Expected committer but didn't get one");
	msg = cmd_data(&msglen);
	read_next_command();
	cmd_from(b);

	/* ensure the branch is active/loaded */
	if (!b->branch_tree.tree || !max_active_branches) {
		unload_one_branch();
		load_branch(b);
	}

	/* file_change* */
	for (;;) {
		if (1 == command_buf.len)
			break;
		else if (!strncmp("M ", command_buf.buf, 2))
			file_change_m(b);
		else if (!strncmp("D ", command_buf.buf, 2))
			file_change_d(b);
		else
			die("Unsupported file_change: %s", command_buf.buf);
		read_next_command();
	}

	/* build the tree and the commit */
	store_tree(&b->branch_tree);
	body = xmalloc(97 + msglen
		+ (author
			? strlen(author) + strlen(committer)
			: 2 * strlen(committer)));
	sp = body;
	sp += sprintf(sp, "tree %s\n", sha1_to_hex(b->branch_tree.sha1));
	if (memcmp(b->sha1, null_sha1, 20))
		sp += sprintf(sp, "parent %s\n", sha1_to_hex(b->sha1));
	if (author)
		sp += sprintf(sp, "%s\n", author);
	else
		sp += sprintf(sp, "author %s\n", committer + 10);
	sp += sprintf(sp, "%s\n\n", committer);
	memcpy(sp, msg, msglen);
	sp += msglen;
	if (author)
		free(author);
	free(committer);
	free(msg);

	store_object(OBJ_COMMIT, body, sp - body, NULL, b->sha1, next_mark);
	free(body);
	b->last_commit = object_count_by_type[OBJ_COMMIT];

	if (branch_log) {
		int need_dq = quote_c_style(b->name, NULL, NULL, 0);
		fprintf(branch_log, "commit ");
		if (need_dq) {
			fputc('"', branch_log);
			quote_c_style(b->name, NULL, branch_log, 0);
			fputc('"', branch_log);
		} else
			fprintf(branch_log, "%s", b->name);
		fprintf(branch_log," :%lu %s\n",next_mark,sha1_to_hex(b->sha1));
	}
}

static void cmd_new_tag()
{
	char *str_uq;
	const char *endp;
	char *sp;
	const char *from;
	char *tagger;
	struct branch *s;
	void *msg;
	size_t msglen;
	char *body;
	struct tag *t;
	unsigned long from_mark = 0;
	unsigned char sha1[20];

	/* Obtain the new tag name from the rest of our command */
	sp = strchr(command_buf.buf, ' ') + 1;
	str_uq = unquote_c_style(sp, &endp);
	if (str_uq) {
		if (*endp)
			die("Garbage after tag name in: %s", command_buf.buf);
		sp = str_uq;
	}
	t = pool_alloc(sizeof(struct tag));
	t->next_tag = NULL;
	t->name = pool_strdup(sp);
	if (last_tag)
		last_tag->next_tag = t;
	else
		first_tag = t;
	last_tag = t;
	if (str_uq)
		free(str_uq);
	read_next_command();

	/* from ... */
	if (strncmp("from ", command_buf.buf, 5))
		die("Expected from command, got %s", command_buf.buf);

	from = strchr(command_buf.buf, ' ') + 1;
	str_uq = unquote_c_style(from, &endp);
	if (str_uq) {
		if (*endp)
			die("Garbage after string in: %s", command_buf.buf);
		from = str_uq;
	}

	s = lookup_branch(from);
	if (s) {
		memcpy(sha1, s->sha1, 20);
	} else if (*from == ':') {
		from_mark = strtoul(from + 1, NULL, 10);
		struct object_entry *oe = find_mark(from_mark);
		if (oe->type != OBJ_COMMIT)
			die("Mark :%lu not a commit", from_mark);
		memcpy(sha1, oe->sha1, 20);
	} else if (!get_sha1(from, sha1)) {
		unsigned long size;
		char *buf;

		buf = read_object_with_reference(sha1,
			type_names[OBJ_COMMIT], &size, sha1);
		if (!buf || size < 46)
			die("Not a valid commit: %s", from);
		free(buf);
	} else
		die("Invalid ref name or SHA1 expression: %s", from);

	if (str_uq)
		free(str_uq);
	read_next_command();

	/* tagger ... */
	if (strncmp("tagger ", command_buf.buf, 7))
		die("Expected tagger command, got %s", command_buf.buf);
	tagger = strdup(command_buf.buf);

	/* tag payload/message */
	read_next_command();
	msg = cmd_data(&msglen);

	/* build the tag object */
	body = xmalloc(67 + strlen(t->name) + strlen(tagger) + msglen);
	sp = body;
	sp += sprintf(sp, "object %s\n", sha1_to_hex(sha1));
	sp += sprintf(sp, "type %s\n", type_names[OBJ_COMMIT]);
	sp += sprintf(sp, "tag %s\n", t->name);
	sp += sprintf(sp, "%s\n\n", tagger);
	memcpy(sp, msg, msglen);
	sp += msglen;
	free(tagger);
	free(msg);

	store_object(OBJ_TAG, body, sp - body, NULL, t->sha1, 0);
	free(body);

	if (branch_log) {
		int need_dq = quote_c_style(t->name, NULL, NULL, 0);
		fprintf(branch_log, "tag ");
		if (need_dq) {
			fputc('"', branch_log);
			quote_c_style(t->name, NULL, branch_log, 0);
			fputc('"', branch_log);
		} else
			fprintf(branch_log, "%s", t->name);
		fprintf(branch_log," :%lu %s\n",from_mark,sha1_to_hex(t->sha1));
	}
}

static const char fast_import_usage[] =
"git-fast-import [--objects=n] [--depth=n] [--active-branches=n] [--export-marks=marks.file] [--branch-log=log] temp.pack";

int main(int argc, const char **argv)
{
	const char *base_name;
	int i;
	unsigned long est_obj_cnt = 1000;
	char *pack_name;
	char *idx_name;
	struct stat sb;

	setup_ident();
	git_config(git_default_config);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (*a != '-' || !strcmp(a, "--"))
			break;
		else if (!strncmp(a, "--objects=", 10))
			est_obj_cnt = strtoul(a + 10, NULL, 0);
		else if (!strncmp(a, "--depth=", 8))
			max_depth = strtoul(a + 8, NULL, 0);
		else if (!strncmp(a, "--active-branches=", 18))
			max_active_branches = strtoul(a + 18, NULL, 0);
		else if (!strncmp(a, "--export-marks=", 15))
			mark_file = a + 15;
		else if (!strncmp(a, "--branch-log=", 13)) {
			branch_log = fopen(a + 13, "w");
			if (!branch_log)
				die("Can't create %s: %s", a + 13, strerror(errno));
		}
		else
			die("unknown option %s", a);
	}
	if ((i+1) != argc)
		usage(fast_import_usage);
	base_name = argv[i];

	pack_name = xmalloc(strlen(base_name) + 6);
	sprintf(pack_name, "%s.pack", base_name);
	idx_name = xmalloc(strlen(base_name) + 5);
	sprintf(idx_name, "%s.idx", base_name);

	pack_fd = open(pack_name, O_RDWR|O_CREAT|O_EXCL, 0666);
	if (pack_fd < 0)
		die("Can't create %s: %s", pack_name, strerror(errno));

	init_pack_header();
	alloc_objects(est_obj_cnt);
	strbuf_init(&command_buf);

	atom_table = xcalloc(atom_table_sz, sizeof(struct atom_str*));
	branch_table = xcalloc(branch_table_sz, sizeof(struct branch*));
	avail_tree_table = xcalloc(avail_tree_table_sz, sizeof(struct avail_tree_content*));
	marks = pool_calloc(1, sizeof(struct mark_set));

	for (;;) {
		read_next_command();
		if (command_buf.eof)
			break;
		else if (!strcmp("blob", command_buf.buf))
			cmd_new_blob();
		else if (!strncmp("commit ", command_buf.buf, 7))
			cmd_new_commit();
		else if (!strncmp("tag ", command_buf.buf, 4))
			cmd_new_tag();
		else
			die("Unsupported command: %s", command_buf.buf);
	}

	fixup_header_footer();
	close(pack_fd);
	write_index(idx_name);
	dump_branches();
	dump_tags();
	dump_marks();
	fclose(branch_log);

	fprintf(stderr, "%s statistics:\n", argv[0]);
	fprintf(stderr, "---------------------------------------------------\n");
	fprintf(stderr, "Alloc'd objects: %10lu (%10lu overflow  )\n", alloc_count, alloc_count - est_obj_cnt);
	fprintf(stderr, "Total objects:   %10lu (%10lu duplicates)\n", object_count, duplicate_count);
	fprintf(stderr, "      blobs  :   %10lu (%10lu duplicates)\n", object_count_by_type[OBJ_BLOB], duplicate_count_by_type[OBJ_BLOB]);
	fprintf(stderr, "      trees  :   %10lu (%10lu duplicates)\n", object_count_by_type[OBJ_TREE], duplicate_count_by_type[OBJ_TREE]);
	fprintf(stderr, "      commits:   %10lu (%10lu duplicates)\n", object_count_by_type[OBJ_COMMIT], duplicate_count_by_type[OBJ_COMMIT]);
	fprintf(stderr, "      tags   :   %10lu (%10lu duplicates)\n", object_count_by_type[OBJ_TAG], duplicate_count_by_type[OBJ_TAG]);
	fprintf(stderr, "Total branches:  %10lu (%10lu loads     )\n", branch_count, branch_load_count);
	fprintf(stderr, "      marks:     %10u (%10lu unique    )\n", (1 << marks->shift) * 1024, marks_set_count);
	fprintf(stderr, "      atoms:     %10u\n", atom_cnt);
	fprintf(stderr, "Memory total:    %10lu KiB\n", (total_allocd + alloc_count*sizeof(struct object_entry))/1024);
	fprintf(stderr, "       pools:    %10lu KiB\n", total_allocd/1024);
	fprintf(stderr, "     objects:    %10lu KiB\n", (alloc_count*sizeof(struct object_entry))/1024);
	fprintf(stderr, "Pack remaps:     %10lu\n", remap_count);
	fprintf(stderr, "---------------------------------------------------\n");

	stat(pack_name, &sb);
	fprintf(stderr, "Pack size:       %10lu KiB\n", (unsigned long)(sb.st_size/1024));
	stat(idx_name, &sb);
	fprintf(stderr, "Index size:      %10lu KiB\n", (unsigned long)(sb.st_size/1024));

	fprintf(stderr, "\n");

	return 0;
}
