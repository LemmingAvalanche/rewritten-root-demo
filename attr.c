/*
 * Handle git attributes.  See gitattributes(5) for a description of
 * the file syntax, and Documentation/technical/api-gitattributes.txt
 * for a description of the API.
 *
 * One basic design decision here is that we are not going to support
 * an insanely large number of attributes.
 */

#define NO_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"
#include "exec_cmd.h"
#include "attr.h"
#include "dir.h"
#include "utf8.h"
#include "quote.h"
#include "thread-utils.h"

const char git_attr__true[] = "(builtin)true";
const char git_attr__false[] = "\0(builtin)false";
static const char git_attr__unknown[] = "(builtin)unknown";
#define ATTR__TRUE git_attr__true
#define ATTR__FALSE git_attr__false
#define ATTR__UNSET NULL
#define ATTR__UNKNOWN git_attr__unknown

#ifndef DEBUG_ATTR
#define DEBUG_ATTR 0
#endif

struct git_attr {
	int attr_nr; /* unique attribute number */
	int maybe_macro;
	int maybe_real;
	char name[FLEX_ARRAY]; /* attribute name */
};

/*
 * NEEDSWORK: maybe-real, maybe-macro are not property of
 * an attribute, as it depends on what .gitattributes are
 * read.  Once we introduce per git_attr_check attr_stack
 * and check_all_attr, the optimization based on them will
 * become unnecessary and can go away.  So is this variable.
 */
static int cannot_trust_maybe_real;

const char *git_attr_name(const struct git_attr *attr)
{
	return attr->name;
}

struct attr_hashmap {
	struct hashmap map;
#ifndef NO_PTHREADS
	pthread_mutex_t mutex;
#endif
};

static inline void hashmap_lock(struct attr_hashmap *map)
{
#ifndef NO_PTHREADS
	pthread_mutex_lock(&map->mutex);
#endif
}

static inline void hashmap_unlock(struct attr_hashmap *map)
{
#ifndef NO_PTHREADS
	pthread_mutex_unlock(&map->mutex);
#endif
}

/*
 * The global dictionary of all interned attributes.  This
 * is a singleton object which is shared between threads.
 * Access to this dictionary must be surrounded with a mutex.
 */
static struct attr_hashmap g_attr_hashmap;

/* The container for objects stored in "struct attr_hashmap" */
struct attr_hash_entry {
	struct hashmap_entry ent; /* must be the first member! */
	const char *key; /* the key; memory should be owned by value */
	size_t keylen; /* length of the key */
	void *value; /* the stored value */
};

/* attr_hashmap comparison function */
static int attr_hash_entry_cmp(const struct attr_hash_entry *a,
			       const struct attr_hash_entry *b,
			       void *unused)
{
	return (a->keylen != b->keylen) || strncmp(a->key, b->key, a->keylen);
}

/* Initialize an 'attr_hashmap' object */
static void attr_hashmap_init(struct attr_hashmap *map)
{
	hashmap_init(&map->map, (hashmap_cmp_fn) attr_hash_entry_cmp, 0);
}

/*
 * Retrieve the 'value' stored in a hashmap given the provided 'key'.
 * If there is no matching entry, return NULL.
 */
static void *attr_hashmap_get(struct attr_hashmap *map,
			      const char *key, size_t keylen)
{
	struct attr_hash_entry k;
	struct attr_hash_entry *e;

	if (!map->map.tablesize)
		attr_hashmap_init(map);

	hashmap_entry_init(&k, memhash(key, keylen));
	k.key = key;
	k.keylen = keylen;
	e = hashmap_get(&map->map, &k, NULL);

	return e ? e->value : NULL;
}

/* Add 'value' to a hashmap based on the provided 'key'. */
static void attr_hashmap_add(struct attr_hashmap *map,
			     const char *key, size_t keylen,
			     void *value)
{
	struct attr_hash_entry *e;

	if (!map->map.tablesize)
		attr_hashmap_init(map);

	e = xmalloc(sizeof(struct attr_hash_entry));
	hashmap_entry_init(e, memhash(key, keylen));
	e->key = key;
	e->keylen = keylen;
	e->value = value;

	hashmap_add(&map->map, e);
}

struct all_attrs_item {
	const struct git_attr *attr;
	const char *value;
};

/*
 * Reallocate and reinitialize the array of all attributes (which is used in
 * the attribute collection process) in 'check' based on the global dictionary
 * of attributes.
 */
static void all_attrs_init(struct attr_hashmap *map, struct attr_check *check)
{
	int i;

	hashmap_lock(map);

	if (map->map.size < check->all_attrs_nr)
		die("BUG: interned attributes shouldn't be deleted");

	/*
	 * If the number of attributes in the global dictionary has increased
	 * (or this attr_check instance doesn't have an initialized all_attrs
	 * field), reallocate the provided attr_check instance's all_attrs
	 * field and fill each entry with its corresponding git_attr.
	 */
	if (map->map.size != check->all_attrs_nr) {
		struct attr_hash_entry *e;
		struct hashmap_iter iter;
		hashmap_iter_init(&map->map, &iter);

		REALLOC_ARRAY(check->all_attrs, map->map.size);
		check->all_attrs_nr = map->map.size;

		while ((e = hashmap_iter_next(&iter))) {
			const struct git_attr *a = e->value;
			check->all_attrs[a->attr_nr].attr = a;
		}
	}

	hashmap_unlock(map);

	/*
	 * Re-initialize every entry in check->all_attrs.
	 * This re-initialization can live outside of the locked region since
	 * the attribute dictionary is no longer being accessed.
	 */
	for (i = 0; i < check->all_attrs_nr; i++) {
		check->all_attrs[i].value = ATTR__UNKNOWN;
	}
}

static int attr_name_valid(const char *name, size_t namelen)
{
	/*
	 * Attribute name cannot begin with '-' and must consist of
	 * characters from [-A-Za-z0-9_.].
	 */
	if (namelen <= 0 || *name == '-')
		return 0;
	while (namelen--) {
		char ch = *name++;
		if (! (ch == '-' || ch == '.' || ch == '_' ||
		       ('0' <= ch && ch <= '9') ||
		       ('a' <= ch && ch <= 'z') ||
		       ('A' <= ch && ch <= 'Z')) )
			return 0;
	}
	return 1;
}

static void report_invalid_attr(const char *name, size_t len,
				const char *src, int lineno)
{
	struct strbuf err = STRBUF_INIT;
	strbuf_addf(&err, _("%.*s is not a valid attribute name"),
		    (int) len, name);
	fprintf(stderr, "%s: %s:%d\n", err.buf, src, lineno);
	strbuf_release(&err);
}

/*
 * Given a 'name', lookup and return the corresponding attribute in the global
 * dictionary.  If no entry is found, create a new attribute and store it in
 * the dictionary.
 */
static struct git_attr *git_attr_internal(const char *name, int namelen)
{
	struct git_attr *a;

	if (!attr_name_valid(name, namelen))
		return NULL;

	hashmap_lock(&g_attr_hashmap);

	a = attr_hashmap_get(&g_attr_hashmap, name, namelen);

	if (!a) {
		FLEX_ALLOC_MEM(a, name, name, namelen);
		a->attr_nr = g_attr_hashmap.map.size;
		a->maybe_real = 0;
		a->maybe_macro = 0;

		attr_hashmap_add(&g_attr_hashmap, a->name, namelen, a);
		assert(a->attr_nr == (g_attr_hashmap.map.size - 1));
	}

	hashmap_unlock(&g_attr_hashmap);

	return a;
}

struct git_attr *git_attr(const char *name)
{
	return git_attr_internal(name, strlen(name));
}

/* What does a matched pattern decide? */
struct attr_state {
	struct git_attr *attr;
	const char *setto;
};

struct pattern {
	const char *pattern;
	int patternlen;
	int nowildcardlen;
	unsigned flags;		/* EXC_FLAG_* */
};

/*
 * One rule, as from a .gitattributes file.
 *
 * If is_macro is true, then u.attr is a pointer to the git_attr being
 * defined.
 *
 * If is_macro is false, then u.pat is the filename pattern to which the
 * rule applies.
 *
 * In either case, num_attr is the number of attributes affected by
 * this rule, and state is an array listing them.  The attributes are
 * listed as they appear in the file (macros unexpanded).
 */
struct match_attr {
	union {
		struct pattern pat;
		struct git_attr *attr;
	} u;
	char is_macro;
	unsigned num_attr;
	struct attr_state state[FLEX_ARRAY];
};

static const char blank[] = " \t\r\n";

/*
 * Parse a whitespace-delimited attribute state (i.e., "attr",
 * "-attr", "!attr", or "attr=value") from the string starting at src.
 * If e is not NULL, write the results to *e.  Return a pointer to the
 * remainder of the string (with leading whitespace removed), or NULL
 * if there was an error.
 */
static const char *parse_attr(const char *src, int lineno, const char *cp,
			      struct attr_state *e)
{
	const char *ep, *equals;
	int len;

	ep = cp + strcspn(cp, blank);
	equals = strchr(cp, '=');
	if (equals && ep < equals)
		equals = NULL;
	if (equals)
		len = equals - cp;
	else
		len = ep - cp;
	if (!e) {
		if (*cp == '-' || *cp == '!') {
			cp++;
			len--;
		}
		if (!attr_name_valid(cp, len)) {
			report_invalid_attr(cp, len, src, lineno);
			return NULL;
		}
	} else {
		/*
		 * As this function is always called twice, once with
		 * e == NULL in the first pass and then e != NULL in
		 * the second pass, no need for attr_name_valid()
		 * check here.
		 */
		if (*cp == '-' || *cp == '!') {
			e->setto = (*cp == '-') ? ATTR__FALSE : ATTR__UNSET;
			cp++;
			len--;
		}
		else if (!equals)
			e->setto = ATTR__TRUE;
		else {
			e->setto = xmemdupz(equals + 1, ep - equals - 1);
		}
		e->attr = git_attr_internal(cp, len);
	}
	return ep + strspn(ep, blank);
}

static struct match_attr *parse_attr_line(const char *line, const char *src,
					  int lineno, int macro_ok)
{
	int namelen;
	int num_attr, i;
	const char *cp, *name, *states;
	struct match_attr *res = NULL;
	int is_macro;
	struct strbuf pattern = STRBUF_INIT;

	cp = line + strspn(line, blank);
	if (!*cp || *cp == '#')
		return NULL;
	name = cp;

	if (*cp == '"' && !unquote_c_style(&pattern, name, &states)) {
		name = pattern.buf;
		namelen = pattern.len;
	} else {
		namelen = strcspn(name, blank);
		states = name + namelen;
	}

	if (strlen(ATTRIBUTE_MACRO_PREFIX) < namelen &&
	    starts_with(name, ATTRIBUTE_MACRO_PREFIX)) {
		if (!macro_ok) {
			fprintf(stderr, "%s not allowed: %s:%d\n",
				name, src, lineno);
			goto fail_return;
		}
		is_macro = 1;
		name += strlen(ATTRIBUTE_MACRO_PREFIX);
		name += strspn(name, blank);
		namelen = strcspn(name, blank);
		if (!attr_name_valid(name, namelen)) {
			report_invalid_attr(name, namelen, src, lineno);
			goto fail_return;
		}
	}
	else
		is_macro = 0;

	states += strspn(states, blank);

	/* First pass to count the attr_states */
	for (cp = states, num_attr = 0; *cp; num_attr++) {
		cp = parse_attr(src, lineno, cp, NULL);
		if (!cp)
			goto fail_return;
	}

	res = xcalloc(1,
		      sizeof(*res) +
		      sizeof(struct attr_state) * num_attr +
		      (is_macro ? 0 : namelen + 1));
	if (is_macro) {
		res->u.attr = git_attr_internal(name, namelen);
		res->u.attr->maybe_macro = 1;
	} else {
		char *p = (char *)&(res->state[num_attr]);
		memcpy(p, name, namelen);
		res->u.pat.pattern = p;
		parse_exclude_pattern(&res->u.pat.pattern,
				      &res->u.pat.patternlen,
				      &res->u.pat.flags,
				      &res->u.pat.nowildcardlen);
		if (res->u.pat.flags & EXC_FLAG_NEGATIVE) {
			warning(_("Negative patterns are ignored in git attributes\n"
				  "Use '\\!' for literal leading exclamation."));
			goto fail_return;
		}
	}
	res->is_macro = is_macro;
	res->num_attr = num_attr;

	/* Second pass to fill the attr_states */
	for (cp = states, i = 0; *cp; i++) {
		cp = parse_attr(src, lineno, cp, &(res->state[i]));
		if (!is_macro)
			res->state[i].attr->maybe_real = 1;
		if (res->state[i].attr->maybe_macro)
			cannot_trust_maybe_real = 1;
	}

	strbuf_release(&pattern);
	return res;

fail_return:
	strbuf_release(&pattern);
	free(res);
	return NULL;
}

/*
 * Like info/exclude and .gitignore, the attribute information can
 * come from many places.
 *
 * (1) .gitattribute file of the same directory;
 * (2) .gitattribute file of the parent directory if (1) does not have
 *      any match; this goes recursively upwards, just like .gitignore.
 * (3) $GIT_DIR/info/attributes, which overrides both of the above.
 *
 * In the same file, later entries override the earlier match, so in the
 * global list, we would have entries from info/attributes the earliest
 * (reading the file from top to bottom), .gitattribute of the root
 * directory (again, reading the file from top to bottom) down to the
 * current directory, and then scan the list backwards to find the first match.
 * This is exactly the same as what is_excluded() does in dir.c to deal with
 * .gitignore file and info/excludes file as a fallback.
 */

/* NEEDSWORK: This will become per git_attr_check */
static struct attr_stack {
	struct attr_stack *prev;
	char *origin;
	size_t originlen;
	unsigned num_matches;
	unsigned alloc;
	struct match_attr **attrs;
} *attr_stack;

static void free_attr_elem(struct attr_stack *e)
{
	int i;
	free(e->origin);
	for (i = 0; i < e->num_matches; i++) {
		struct match_attr *a = e->attrs[i];
		int j;
		for (j = 0; j < a->num_attr; j++) {
			const char *setto = a->state[j].setto;
			if (setto == ATTR__TRUE ||
			    setto == ATTR__FALSE ||
			    setto == ATTR__UNSET ||
			    setto == ATTR__UNKNOWN)
				;
			else
				free((char *) setto);
		}
		free(a);
	}
	free(e->attrs);
	free(e);
}

struct attr_check *attr_check_alloc(void)
{
	return xcalloc(1, sizeof(struct attr_check));
}

struct attr_check *attr_check_initl(const char *one, ...)
{
	struct attr_check *check;
	int cnt;
	va_list params;
	const char *param;

	va_start(params, one);
	for (cnt = 1; (param = va_arg(params, const char *)) != NULL; cnt++)
		;
	va_end(params);

	check = attr_check_alloc();
	check->nr = cnt;
	check->alloc = cnt;
	check->items = xcalloc(cnt, sizeof(struct attr_check_item));

	check->items[0].attr = git_attr(one);
	va_start(params, one);
	for (cnt = 1; cnt < check->nr; cnt++) {
		const struct git_attr *attr;
		param = va_arg(params, const char *);
		if (!param)
			die("BUG: counted %d != ended at %d",
			    check->nr, cnt);
		attr = git_attr(param);
		if (!attr)
			die("BUG: %s: not a valid attribute name", param);
		check->items[cnt].attr = attr;
	}
	va_end(params);
	return check;
}

struct attr_check_item *attr_check_append(struct attr_check *check,
					  const struct git_attr *attr)
{
	struct attr_check_item *item;

	ALLOC_GROW(check->items, check->nr + 1, check->alloc);
	item = &check->items[check->nr++];
	item->attr = attr;
	return item;
}

void attr_check_reset(struct attr_check *check)
{
	check->nr = 0;
}

void attr_check_clear(struct attr_check *check)
{
	free(check->items);
	check->items = NULL;
	check->alloc = 0;
	check->nr = 0;

	free(check->all_attrs);
	check->all_attrs = NULL;
	check->all_attrs_nr = 0;
}

void attr_check_free(struct attr_check *check)
{
	attr_check_clear(check);
	free(check);
}

static const char *builtin_attr[] = {
	"[attr]binary -diff -merge -text",
	NULL,
};

static void handle_attr_line(struct attr_stack *res,
			     const char *line,
			     const char *src,
			     int lineno,
			     int macro_ok)
{
	struct match_attr *a;

	a = parse_attr_line(line, src, lineno, macro_ok);
	if (!a)
		return;
	ALLOC_GROW(res->attrs, res->num_matches + 1, res->alloc);
	res->attrs[res->num_matches++] = a;
}

static struct attr_stack *read_attr_from_array(const char **list)
{
	struct attr_stack *res;
	const char *line;
	int lineno = 0;

	res = xcalloc(1, sizeof(*res));
	while ((line = *(list++)) != NULL)
		handle_attr_line(res, line, "[builtin]", ++lineno, 1);
	return res;
}

/*
 * NEEDSWORK: these two are tricky.  The callers assume there is a
 * single, system-wide global state "where we read attributes from?"
 * and when the state is flipped by calling git_attr_set_direction(),
 * attr_stack is discarded so that subsequent attr_check will lazily
 * read from the right place.  And they do not know or care who called
 * by them uses the attribute subsystem, hence have no knowledge of
 * existing git_attr_check instances or future ones that will be
 * created).
 *
 * Probably we need a thread_local that holds these two variables,
 * and a list of git_attr_check instances (which need to be maintained
 * by hooking into git_attr_check_alloc(), git_attr_check_initl(), and
 * git_attr_check_clear().  Then git_attr_set_direction() updates the
 * fields in that thread_local for these two variables, iterate over
 * all the active git_attr_check instances and discard the attr_stack
 * they hold.  Yuck, but it sounds doable.
 */
static enum git_attr_direction direction;
static struct index_state *use_index;

static struct attr_stack *read_attr_from_file(const char *path, int macro_ok)
{
	FILE *fp = fopen(path, "r");
	struct attr_stack *res;
	char buf[2048];
	int lineno = 0;

	if (!fp) {
		if (errno != ENOENT && errno != ENOTDIR)
			warn_on_inaccessible(path);
		return NULL;
	}
	res = xcalloc(1, sizeof(*res));
	while (fgets(buf, sizeof(buf), fp)) {
		char *bufp = buf;
		if (!lineno)
			skip_utf8_bom(&bufp, strlen(bufp));
		handle_attr_line(res, bufp, path, ++lineno, macro_ok);
	}
	fclose(fp);
	return res;
}

static struct attr_stack *read_attr_from_index(const char *path, int macro_ok)
{
	struct attr_stack *res;
	char *buf, *sp;
	int lineno = 0;

	buf = read_blob_data_from_index(use_index ? use_index : &the_index, path, NULL);
	if (!buf)
		return NULL;

	res = xcalloc(1, sizeof(*res));
	for (sp = buf; *sp; ) {
		char *ep;
		int more;

		ep = strchrnul(sp, '\n');
		more = (*ep == '\n');
		*ep = '\0';
		handle_attr_line(res, sp, path, ++lineno, macro_ok);
		sp = ep + more;
	}
	free(buf);
	return res;
}

static struct attr_stack *read_attr(const char *path, int macro_ok)
{
	struct attr_stack *res;

	if (direction == GIT_ATTR_CHECKOUT) {
		res = read_attr_from_index(path, macro_ok);
		if (!res)
			res = read_attr_from_file(path, macro_ok);
	}
	else if (direction == GIT_ATTR_CHECKIN) {
		res = read_attr_from_file(path, macro_ok);
		if (!res)
			/*
			 * There is no checked out .gitattributes file there, but
			 * we might have it in the index.  We allow operation in a
			 * sparsely checked out work tree, so read from it.
			 */
			res = read_attr_from_index(path, macro_ok);
	}
	else
		res = read_attr_from_index(path, macro_ok);
	if (!res)
		res = xcalloc(1, sizeof(*res));
	return res;
}

#if DEBUG_ATTR
static void debug_info(const char *what, struct attr_stack *elem)
{
	fprintf(stderr, "%s: %s\n", what, elem->origin ? elem->origin : "()");
}
static void debug_set(const char *what, const char *match, struct git_attr *attr, const void *v)
{
	const char *value = v;

	if (ATTR_TRUE(value))
		value = "set";
	else if (ATTR_FALSE(value))
		value = "unset";
	else if (ATTR_UNSET(value))
		value = "unspecified";

	fprintf(stderr, "%s: %s => %s (%s)\n",
		what, attr->name, (char *) value, match);
}
#define debug_push(a) debug_info("push", (a))
#define debug_pop(a) debug_info("pop", (a))
#else
#define debug_push(a) do { ; } while (0)
#define debug_pop(a) do { ; } while (0)
#define debug_set(a,b,c,d) do { ; } while (0)
#endif /* DEBUG_ATTR */

static void drop_attr_stack(void)
{
	while (attr_stack) {
		struct attr_stack *elem = attr_stack;
		attr_stack = elem->prev;
		free_attr_elem(elem);
	}
}

static const char *git_etc_gitattributes(void)
{
	static const char *system_wide;
	if (!system_wide)
		system_wide = system_path(ETC_GITATTRIBUTES);
	return system_wide;
}

static int git_attr_system(void)
{
	return !git_env_bool("GIT_ATTR_NOSYSTEM", 0);
}

static GIT_PATH_FUNC(git_path_info_attributes, INFOATTRIBUTES_FILE)

static void push_stack(struct attr_stack **attr_stack_p,
		       struct attr_stack *elem, char *origin, size_t originlen)
{
	if (elem) {
		elem->origin = origin;
		if (origin)
			elem->originlen = originlen;
		elem->prev = *attr_stack_p;
		*attr_stack_p = elem;
	}
}

static void bootstrap_attr_stack(void)
{
	struct attr_stack *elem;

	if (attr_stack)
		return;

	push_stack(&attr_stack, read_attr_from_array(builtin_attr), NULL, 0);

	if (git_attr_system())
		push_stack(&attr_stack,
			   read_attr_from_file(git_etc_gitattributes(), 1),
			   NULL, 0);

	if (!git_attributes_file)
		git_attributes_file = xdg_config_home("attributes");
	if (git_attributes_file)
		push_stack(&attr_stack,
			   read_attr_from_file(git_attributes_file, 1),
			   NULL, 0);

	if (!is_bare_repository() || direction == GIT_ATTR_INDEX) {
		elem = read_attr(GITATTRIBUTES_FILE, 1);
		push_stack(&attr_stack, elem, xstrdup(""), 0);
		debug_push(elem);
	}

	if (startup_info->have_repository)
		elem = read_attr_from_file(git_path_info_attributes(), 1);
	else
		elem = NULL;

	if (!elem)
		elem = xcalloc(1, sizeof(*elem));
	push_stack(&attr_stack, elem, NULL, 0);
}

static void prepare_attr_stack(const char *path, int dirlen)
{
	struct attr_stack *elem, *info;
	const char *cp;

	/*
	 * At the bottom of the attribute stack is the built-in
	 * set of attribute definitions, followed by the contents
	 * of $(prefix)/etc/gitattributes and a file specified by
	 * core.attributesfile.  Then, contents from
	 * .gitattribute files from directories closer to the
	 * root to the ones in deeper directories are pushed
	 * to the stack.  Finally, at the very top of the stack
	 * we always keep the contents of $GIT_DIR/info/attributes.
	 *
	 * When checking, we use entries from near the top of the
	 * stack, preferring $GIT_DIR/info/attributes, then
	 * .gitattributes in deeper directories to shallower ones,
	 * and finally use the built-in set as the default.
	 */
	bootstrap_attr_stack();

	/*
	 * Pop the "info" one that is always at the top of the stack.
	 */
	info = attr_stack;
	attr_stack = info->prev;

	/*
	 * Pop the ones from directories that are not the prefix of
	 * the path we are checking. Break out of the loop when we see
	 * the root one (whose origin is an empty string "") or the builtin
	 * one (whose origin is NULL) without popping it.
	 */
	while (attr_stack->origin) {
		int namelen = strlen(attr_stack->origin);

		elem = attr_stack;
		if (namelen <= dirlen &&
		    !strncmp(elem->origin, path, namelen) &&
		    (!namelen || path[namelen] == '/'))
			break;

		debug_pop(elem);
		attr_stack = elem->prev;
		free_attr_elem(elem);
	}

	/*
	 * Read from parent directories and push them down
	 */
	if (!is_bare_repository() || direction == GIT_ATTR_INDEX) {
		/*
		 * bootstrap_attr_stack() should have added, and the
		 * above loop should have stopped before popping, the
		 * root element whose attr_stack->origin is set to an
		 * empty string.
		 */
		struct strbuf pathbuf = STRBUF_INIT;

		assert(attr_stack->origin);
		while (1) {
			size_t len = strlen(attr_stack->origin);
			char *origin;

			if (dirlen <= len)
				break;
			cp = memchr(path + len + 1, '/', dirlen - len - 1);
			if (!cp)
				cp = path + dirlen;
			strbuf_addf(&pathbuf,
				    "%.*s/%s", (int)(cp - path), path,
				    GITATTRIBUTES_FILE);
			elem = read_attr(pathbuf.buf, 0);
			strbuf_setlen(&pathbuf, cp - path);
			origin = strbuf_detach(&pathbuf, &len);
			push_stack(&attr_stack, elem, origin, len);
			debug_push(elem);
		}

		strbuf_release(&pathbuf);
	}

	/*
	 * Finally push the "info" one at the top of the stack.
	 */
	push_stack(&attr_stack, info, NULL, 0);
}

static int path_matches(const char *pathname, int pathlen,
			int basename_offset,
			const struct pattern *pat,
			const char *base, int baselen)
{
	const char *pattern = pat->pattern;
	int prefix = pat->nowildcardlen;
	int isdir = (pathlen && pathname[pathlen - 1] == '/');

	if ((pat->flags & EXC_FLAG_MUSTBEDIR) && !isdir)
		return 0;

	if (pat->flags & EXC_FLAG_NODIR) {
		return match_basename(pathname + basename_offset,
				      pathlen - basename_offset - isdir,
				      pattern, prefix,
				      pat->patternlen, pat->flags);
	}
	return match_pathname(pathname, pathlen - isdir,
			      base, baselen,
			      pattern, prefix, pat->patternlen, pat->flags);
}

static int macroexpand_one(struct all_attrs_item *all_attrs, int nr, int rem);

static int fill_one(const char *what, struct all_attrs_item *all_attrs,
		    struct match_attr *a, int rem)
{
	int i;

	for (i = a->num_attr - 1; rem > 0 && i >= 0; i--) {
		struct git_attr *attr = a->state[i].attr;
		const char **n = &(all_attrs[attr->attr_nr].value);
		const char *v = a->state[i].setto;

		if (*n == ATTR__UNKNOWN) {
			debug_set(what,
				  a->is_macro ? a->u.attr->name : a->u.pat.pattern,
				  attr, v);
			*n = v;
			rem--;
			rem = macroexpand_one(all_attrs, attr->attr_nr, rem);
		}
	}
	return rem;
}

static int fill(const char *path, int pathlen, int basename_offset,
		struct attr_stack *stk, struct all_attrs_item *all_attrs,
		int rem)
{
	int i;
	const char *base = stk->origin ? stk->origin : "";

	for (i = stk->num_matches - 1; 0 < rem && 0 <= i; i--) {
		struct match_attr *a = stk->attrs[i];
		if (a->is_macro)
			continue;
		if (path_matches(path, pathlen, basename_offset,
				 &a->u.pat, base, stk->originlen))
			rem = fill_one("fill", all_attrs, a, rem);
	}
	return rem;
}

static int macroexpand_one(struct all_attrs_item *all_attrs, int nr, int rem)
{
	struct attr_stack *stk;
	int i;

	if (all_attrs[nr].value != ATTR__TRUE ||
	    !all_attrs[nr].attr->maybe_macro)
		return rem;

	for (stk = attr_stack; stk; stk = stk->prev) {
		for (i = stk->num_matches - 1; 0 <= i; i--) {
			struct match_attr *ma = stk->attrs[i];
			if (!ma->is_macro)
				continue;
			if (ma->u.attr->attr_nr == nr)
				return fill_one("expand", all_attrs, ma, rem);
		}
	}

	return rem;
}

/*
 * Collect attributes for path into the array pointed to by check->all_attrs.
 * If check->check_nr is non-zero, only attributes in check[] are collected.
 * Otherwise all attributes are collected.
 */
static void collect_some_attrs(const char *path, struct attr_check *check)
{
	struct attr_stack *stk;
	int i, pathlen, rem, dirlen;
	const char *cp, *last_slash = NULL;
	int basename_offset;

	for (cp = path; *cp; cp++) {
		if (*cp == '/' && cp[1])
			last_slash = cp;
	}
	pathlen = cp - path;
	if (last_slash) {
		basename_offset = last_slash + 1 - path;
		dirlen = last_slash - path;
	} else {
		basename_offset = 0;
		dirlen = 0;
	}

	prepare_attr_stack(path, dirlen);
	all_attrs_init(&g_attr_hashmap, check);

	if (check->nr && !cannot_trust_maybe_real) {
		rem = 0;
		for (i = 0; i < check->nr; i++) {
			const struct git_attr *a = check->items[i].attr;
			if (!a->maybe_real) {
				struct all_attrs_item *c;
				c = check->all_attrs + a->attr_nr;
				c->value = ATTR__UNSET;
				rem++;
			}
		}
		if (rem == check->nr)
			return;
	}

	rem = check->all_attrs_nr;
	for (stk = attr_stack; 0 < rem && stk; stk = stk->prev)
		rem = fill(path, pathlen, basename_offset, stk, check->all_attrs, rem);
}

int git_check_attr(const char *path, struct attr_check *check)
{
	int i;

	collect_some_attrs(path, check);

	for (i = 0; i < check->nr; i++) {
		size_t n = check->items[i].attr->attr_nr;
		const char *value = check->all_attrs[n].value;
		if (value == ATTR__UNKNOWN)
			value = ATTR__UNSET;
		check->items[i].value = value;
	}

	return 0;
}

void git_all_attrs(const char *path, struct attr_check *check)
{
	int i;

	attr_check_reset(check);
	collect_some_attrs(path, check);

	for (i = 0; i < check->all_attrs_nr; i++) {
		const char *name = check->all_attrs[i].attr->name;
		const char *value = check->all_attrs[i].value;
		struct attr_check_item *item;
		if (value == ATTR__UNSET || value == ATTR__UNKNOWN)
			continue;
		item = attr_check_append(check, git_attr(name));
		item->value = value;
	}
}

void git_attr_set_direction(enum git_attr_direction new, struct index_state *istate)
{
	enum git_attr_direction old = direction;

	if (is_bare_repository() && new != GIT_ATTR_INDEX)
		die("BUG: non-INDEX attr direction in a bare repo");

	direction = new;
	if (new != old)
		drop_attr_stack();
	use_index = istate;
}

void attr_start(void)
{
#ifndef NO_PTHREADS
	pthread_mutex_init(&g_attr_hashmap.mutex, NULL);
#endif
}
