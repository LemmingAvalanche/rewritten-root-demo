#include "cache.h"
#include "attr.h"

/*
 * The basic design decision here is that we are not going to have
 * insanely large number of attributes.
 *
 * This is a randomly chosen prime.
 */
#define HASHSIZE 257

#ifndef DEBUG_ATTR
#define DEBUG_ATTR 0
#endif

struct git_attr {
	struct git_attr *next;
	unsigned h;
	int attr_nr;
	char name[FLEX_ARRAY];
};
static int attr_nr;

static struct git_attr_check *check_all_attr;
static struct git_attr *(git_attr_hash[HASHSIZE]);

static unsigned hash_name(const char *name, int namelen)
{
	unsigned val = 0;
	unsigned char c;

	while (namelen--) {
		c = *name++;
		val = ((val << 7) | (val >> 22)) ^ c;
	}
	return val;
}

struct git_attr *git_attr(const char *name, int len)
{
	unsigned hval = hash_name(name, len);
	unsigned pos = hval % HASHSIZE;
	struct git_attr *a;

	for (a = git_attr_hash[pos]; a; a = a->next) {
		if (a->h == hval &&
		    !memcmp(a->name, name, len) && !a->name[len])
			return a;
	}

	a = xmalloc(sizeof(*a) + len + 1);
	memcpy(a->name, name, len);
	a->name[len] = 0;
	a->h = hval;
	a->next = git_attr_hash[pos];
	a->attr_nr = attr_nr++;
	git_attr_hash[pos] = a;

	check_all_attr = xrealloc(check_all_attr,
				  sizeof(*check_all_attr) * attr_nr);
	check_all_attr[a->attr_nr].attr = a;
	return a;
}

/*
 * .gitattributes file is one line per record, each of which is
 *
 * (1) glob pattern.
 * (2) whitespace
 * (3) whitespace separated list of attribute names, each of which
 *     could be prefixed with '!' to mean "not set".
 */

struct attr_state {
	int unset;
	struct git_attr *attr;
};

struct match_attr {
	union {
		char *pattern;
		struct git_attr *attr;
	} u;
	char is_macro;
	unsigned num_attr;
	struct attr_state state[FLEX_ARRAY];
};

static const char blank[] = " \t\r\n";

static struct match_attr *parse_attr_line(const char *line, const char *src,
					  int lineno, int macro_ok)
{
	int namelen;
	int num_attr;
	const char *cp, *name;
	struct match_attr *res = res;
	int pass;
	int is_macro;

	cp = line + strspn(line, blank);
	if (!*cp || *cp == '#')
		return NULL;
	name = cp;
	namelen = strcspn(name, blank);
	if (strlen(ATTRIBUTE_MACRO_PREFIX) < namelen &&
	    !prefixcmp(name, ATTRIBUTE_MACRO_PREFIX)) {
		if (!macro_ok) {
			fprintf(stderr, "%s not allowed: %s:%d\n",
				name, src, lineno);
			return NULL;
		}
		is_macro = 1;
		name += strlen(ATTRIBUTE_MACRO_PREFIX);
		name += strspn(name, blank);
		namelen = strcspn(name, blank);
	}
	else
		is_macro = 0;

	for (pass = 0; pass < 2; pass++) {
		/* pass 0 counts and allocates, pass 1 fills */
		num_attr = 0;
		cp = name + namelen;
		cp = cp + strspn(cp, blank);
		while (*cp) {
			const char *ep;
			ep = cp + strcspn(cp, blank);
			if (pass) {
				struct attr_state *e;

				e = &(res->state[num_attr]);
				if (*cp == '!') {
					e->unset = 1;
					cp++;
				}
				e->attr = git_attr(cp, ep - cp);
			}
			num_attr++;
			cp = ep + strspn(ep, blank);
		}
		if (pass)
			break;

		res = xcalloc(1,
			      sizeof(*res) +
			      sizeof(struct attr_state) * num_attr +
			      (is_macro ? 0 : namelen + 1));
		if (is_macro)
			res->u.attr = git_attr(name, namelen);
		else {
			res->u.pattern = (char*)&(res->state[num_attr]);
			memcpy(res->u.pattern, name, namelen);
			res->u.pattern[namelen] = 0;
		}
		res->is_macro = is_macro;
		res->num_attr = num_attr;
	}
	return res;
}

/*
 * Like info/exclude and .gitignore, the attribute information can
 * come from many places.
 *
 * (1) .gitattribute file of the same directory;
 * (2) .gitattribute file of the parent directory if (1) does not have any match;
 *     this goes recursively upwards, just like .gitignore
 * (3) perhaps $GIT_DIR/info/attributes, as the final fallback.
 *
 * In the same file, later entries override the earlier match, so in the
 * global list, we would have entries from info/attributes the earliest
 * (reading the file from top to bottom), .gitattribute of the root
 * directory (again, reading the file from top to bottom) down to the
 * current directory, and then scan the list backwards to find the first match.
 * This is exactly the same as what excluded() does in dir.c to deal with
 * .gitignore
 */

static struct attr_stack {
	struct attr_stack *prev;
	char *origin;
	unsigned num_matches;
	struct match_attr **attrs;
} *attr_stack;

static void free_attr_elem(struct attr_stack *e)
{
	int i;
	free(e->origin);
	for (i = 0; i < e->num_matches; i++)
		free(e->attrs[i]);
	free(e);
}

static const char *builtin_attr[] = {
	NULL,
};

static struct attr_stack *read_attr_from_array(const char **list)
{
	struct attr_stack *res;
	const char *line;
	int lineno = 0;

	res = xcalloc(1, sizeof(*res));
	while ((line = *(list++)) != NULL) {
		struct match_attr *a;

		a = parse_attr_line(line, "[builtin]", ++lineno, 1);
		if (!a)
			continue;
		res->attrs = xrealloc(res->attrs, res->num_matches + 1);
		res->attrs[res->num_matches++] = a;
	}
	return res;
}

static struct attr_stack *read_attr_from_file(const char *path, int macro_ok)
{
	FILE *fp;
	struct attr_stack *res;
	char buf[2048];
	int lineno = 0;

	res = xcalloc(1, sizeof(*res));
	fp = fopen(path, "r");
	if (!fp)
		return res;

	while (fgets(buf, sizeof(buf), fp)) {
		struct match_attr *a;

		a = parse_attr_line(buf, path, ++lineno, macro_ok);
		if (!a)
			continue;
		res->attrs = xrealloc(res->attrs, res->num_matches + 1);
		res->attrs[res->num_matches++] = a;
	}
	fclose(fp);
	return res;
}

#if DEBUG_ATTR
static void debug_info(const char *what, struct attr_stack *elem)
{
	fprintf(stderr, "%s: %s\n", what, elem->origin ? elem->origin : "()");
}
static void debug_set(const char *what, const char *match, struct git_attr *attr, int set)
{
	fprintf(stderr, "%s: %s => %d (%s)\n",
		what, attr->name, set, match);
}
#define debug_push(a) debug_info("push", (a))
#define debug_pop(a) debug_info("pop", (a))
#else
#define debug_push(a) do { ; } while (0)
#define debug_pop(a) do { ; } while (0)
#define debug_set(a,b,c,d) do { ; } while (0)
#endif

static void bootstrap_attr_stack(void)
{
	if (!attr_stack) {
		struct attr_stack *elem;

		elem = read_attr_from_array(builtin_attr);
		elem->origin = NULL;
		elem->prev = attr_stack;
		attr_stack = elem;

		elem = read_attr_from_file(GITATTRIBUTES_FILE, 1);
		elem->origin = strdup("");
		elem->prev = attr_stack;
		attr_stack = elem;
		debug_push(elem);

		elem = read_attr_from_file(git_path(INFOATTRIBUTES_FILE), 1);
		elem->origin = NULL;
		elem->prev = attr_stack;
		attr_stack = elem;
	}
}

static void prepare_attr_stack(const char *path, int dirlen)
{
	struct attr_stack *elem, *info;
	int len;
	char pathbuf[PATH_MAX];

	/*
	 * At the bottom of the attribute stack is the built-in
	 * set of attribute definitions.  Then, contents from
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
	if (!attr_stack)
		bootstrap_attr_stack();

	/*
	 * Pop the "info" one that is always at the top of the stack.
	 */
	info = attr_stack;
	attr_stack = info->prev;

	/*
	 * Pop the ones from directories that are not the prefix of
	 * the path we are checking.
	 */
	while (attr_stack && attr_stack->origin) {
		int namelen = strlen(attr_stack->origin);

		elem = attr_stack;
		if (namelen <= dirlen &&
		    !strncmp(elem->origin, path, namelen))
			break;

		debug_pop(elem);
		attr_stack = elem->prev;
		free_attr_elem(elem);
	}

	/*
	 * Read from parent directories and push them down
	 */
	while (1) {
		char *cp;

		len = strlen(attr_stack->origin);
		if (dirlen <= len)
			break;
		memcpy(pathbuf, path, dirlen);
		memcpy(pathbuf + dirlen, "/", 2);
		cp = strchr(pathbuf + len + 1, '/');
		strcpy(cp + 1, GITATTRIBUTES_FILE);
		elem = read_attr_from_file(pathbuf, 0);
		*cp = '\0';
		elem->origin = strdup(pathbuf);
		elem->prev = attr_stack;
		attr_stack = elem;
		debug_push(elem);
	}

	/*
	 * Finally push the "info" one at the top of the stack.
	 */
	info->prev = attr_stack;
	attr_stack = info;
}

static int path_matches(const char *pathname, int pathlen,
			const char *pattern,
			const char *base, int baselen)
{
	if (!strchr(pattern, '/')) {
		/* match basename */
		const char *basename = strrchr(pathname, '/');
		basename = basename ? basename + 1 : pathname;
		return (fnmatch(pattern, basename, 0) == 0);
	}
	/*
	 * match with FNM_PATHNAME; the pattern has base implicitly
	 * in front of it.
	 */
	if (*pattern == '/')
		pattern++;
	if (pathlen < baselen ||
	    (baselen && pathname[baselen - 1] != '/') ||
	    strncmp(pathname, base, baselen))
		return 0;
	return fnmatch(pattern, pathname + baselen, FNM_PATHNAME) == 0;
}

static int fill(const char *path, int pathlen, struct attr_stack *stk, int rem)
{
	const char *base = stk->origin ? stk->origin : "";
	int i, j;
	struct git_attr_check *check = check_all_attr;

	for (i = stk->num_matches - 1; 0 < rem && 0 <= i; i--) {
		struct match_attr *a = stk->attrs[i];
		if (a->is_macro)
			continue;
		if (path_matches(path, pathlen,
				 a->u.pattern, base, strlen(base))) {
			for (j = 0; 0 < rem && j < a->num_attr; j++) {
				struct git_attr *attr = a->state[j].attr;
				int set = !a->state[j].unset;
				int *n = &(check[attr->attr_nr].isset);

				if (*n < 0) {
					debug_set("fill", a->u.pattern, attr, set);
					*n = set;
					rem--;
				}
			}
		}
	}
	return rem;
}

static int macroexpand(struct attr_stack *stk, int rem)
{
	int i, j;
	struct git_attr_check *check = check_all_attr;

	for (i = stk->num_matches - 1; 0 < rem && 0 <= i; i--) {
		struct match_attr *a = stk->attrs[i];
		if (!a->is_macro)
			continue;
		if (check[a->u.attr->attr_nr].isset < 0)
			continue;
		for (j = 0; 0 < rem && j < a->num_attr; j++) {
			struct git_attr *attr = a->state[j].attr;
			int set = !a->state[j].unset;
			int *n = &(check[attr->attr_nr].isset);

			if (*n < 0) {
				debug_set("expand", a->u.attr->name, attr, set);
				*n = set;
				rem--;
			}
		}
	}
	return rem;
}

int git_checkattr(const char *path, int num, struct git_attr_check *check)
{
	struct attr_stack *stk;
	const char *cp;
	int dirlen, pathlen, i, rem;

	bootstrap_attr_stack();
	for (i = 0; i < attr_nr; i++)
		check_all_attr[i].isset = -1;

	pathlen = strlen(path);
	cp = strrchr(path, '/');
	if (!cp)
		dirlen = 0;
	else
		dirlen = cp - path;
	prepare_attr_stack(path, dirlen);
	rem = attr_nr;
	for (stk = attr_stack; 0 < rem && stk; stk = stk->prev)
		rem = fill(path, pathlen, stk, rem);

	for (stk = attr_stack; 0 < rem && stk; stk = stk->prev)
		rem = macroexpand(stk, rem);

	for (i = 0; i < num; i++)
		check[i].isset = check_all_attr[check[i].attr->attr_nr].isset;

	return 0;
}
