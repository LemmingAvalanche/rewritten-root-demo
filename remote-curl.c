#include "cache.h"
#include "remote.h"
#include "strbuf.h"
#include "walker.h"
#include "http.h"
#include "exec_cmd.h"
#include "run-command.h"

static struct remote *remote;
static const char *url;
static struct walker *walker;

struct options {
	int verbosity;
	unsigned long depth;
	unsigned progress : 1,
		followtags : 1,
		dry_run : 1;
};
static struct options options;

static void init_walker(void)
{
	if (!walker)
		walker = get_http_walker(url, remote);
}

static int set_option(const char *name, const char *value)
{
	if (!strcmp(name, "verbosity")) {
		char *end;
		int v = strtol(value, &end, 10);
		if (value == end || *end)
			return -1;
		options.verbosity = v;
		return 0;
	}
	else if (!strcmp(name, "progress")) {
		if (!strcmp(value, "true"))
			options.progress = 1;
		else if (!strcmp(value, "false"))
			options.progress = 0;
		else
			return -1;
		return 1 /* TODO implement later */;
	}
	else if (!strcmp(name, "depth")) {
		char *end;
		unsigned long v = strtoul(value, &end, 10);
		if (value == end || *end)
			return -1;
		options.depth = v;
		return 1 /* TODO implement later */;
	}
	else if (!strcmp(name, "followtags")) {
		if (!strcmp(value, "true"))
			options.followtags = 1;
		else if (!strcmp(value, "false"))
			options.followtags = 0;
		else
			return -1;
		return 1 /* TODO implement later */;
	}
	else if (!strcmp(name, "dry-run")) {
		if (!strcmp(value, "true"))
			options.dry_run = 1;
		else if (!strcmp(value, "false"))
			options.dry_run = 0;
		else
			return -1;
		return 0;
	}
	else {
		return 1 /* unsupported */;
	}
}

static struct ref *get_refs(void)
{
	struct strbuf buffer = STRBUF_INIT;
	char *data, *start, *mid;
	char *ref_name;
	char *refs_url;
	int i = 0;
	int http_ret;

	struct ref *refs = NULL;
	struct ref *ref = NULL;
	struct ref *last_ref = NULL;

	refs_url = xmalloc(strlen(url) + 11);
	sprintf(refs_url, "%s/info/refs", url);

	init_walker();
	http_ret = http_get_strbuf(refs_url, &buffer, HTTP_NO_CACHE);
	switch (http_ret) {
	case HTTP_OK:
		break;
	case HTTP_MISSING_TARGET:
		die("%s not found: did you run git update-server-info on the"
		    " server?", refs_url);
	default:
		http_error(refs_url, http_ret);
		die("HTTP request failed");
	}

	data = buffer.buf;
	start = NULL;
	mid = data;
	while (i < buffer.len) {
		if (!start) {
			start = &data[i];
		}
		if (data[i] == '\t')
			mid = &data[i];
		if (data[i] == '\n') {
			data[i] = 0;
			ref_name = mid + 1;
			ref = xmalloc(sizeof(struct ref) +
				      strlen(ref_name) + 1);
			memset(ref, 0, sizeof(struct ref));
			strcpy(ref->name, ref_name);
			get_sha1_hex(start, ref->old_sha1);
			if (!refs)
				refs = ref;
			if (last_ref)
				last_ref->next = ref;
			last_ref = ref;
			start = NULL;
		}
		i++;
	}

	strbuf_release(&buffer);

	ref = alloc_ref("HEAD");
	if (!walker->fetch_ref(walker, ref) &&
	    !resolve_remote_symref(ref, refs)) {
		ref->next = refs;
		refs = ref;
	} else {
		free(ref);
	}

	strbuf_release(&buffer);
	free(refs_url);
	return refs;
}

static void output_refs(struct ref *refs)
{
	struct ref *posn;
	for (posn = refs; posn; posn = posn->next) {
		if (posn->symref)
			printf("@%s %s\n", posn->symref, posn->name);
		else
			printf("%s %s\n", sha1_to_hex(posn->old_sha1), posn->name);
	}
	printf("\n");
	fflush(stdout);
	free_refs(refs);
}

static int fetch_dumb(int nr_heads, struct ref **to_fetch)
{
	char **targets = xmalloc(nr_heads * sizeof(char*));
	int ret, i;

	for (i = 0; i < nr_heads; i++)
		targets[i] = xstrdup(sha1_to_hex(to_fetch[i]->old_sha1));

	init_walker();
	walker->get_all = 1;
	walker->get_tree = 1;
	walker->get_history = 1;
	walker->get_verbosely = options.verbosity >= 3;
	walker->get_recover = 0;
	ret = walker_fetch(walker, nr_heads, targets, NULL, NULL);

	for (i = 0; i < nr_heads; i++)
		free(targets[i]);
	free(targets);

	return ret ? error("Fetch failed.") : 0;
}

static void parse_fetch(struct strbuf *buf)
{
	struct ref **to_fetch = NULL;
	struct ref *list_head = NULL;
	struct ref **list = &list_head;
	int alloc_heads = 0, nr_heads = 0;

	do {
		if (!prefixcmp(buf->buf, "fetch ")) {
			char *p = buf->buf + strlen("fetch ");
			char *name;
			struct ref *ref;
			unsigned char old_sha1[20];

			if (strlen(p) < 40 || get_sha1_hex(p, old_sha1))
				die("protocol error: expected sha/ref, got %s'", p);
			if (p[40] == ' ')
				name = p + 41;
			else if (!p[40])
				name = "";
			else
				die("protocol error: expected sha/ref, got %s'", p);

			ref = alloc_ref(name);
			hashcpy(ref->old_sha1, old_sha1);

			*list = ref;
			list = &ref->next;

			ALLOC_GROW(to_fetch, nr_heads + 1, alloc_heads);
			to_fetch[nr_heads++] = ref;
		}
		else
			die("http transport does not support %s", buf->buf);

		strbuf_reset(buf);
		if (strbuf_getline(buf, stdin, '\n') == EOF)
			return;
		if (!*buf->buf)
			break;
	} while (1);

	if (fetch_dumb(nr_heads, to_fetch))
		exit(128); /* error already reported */
	free_refs(list_head);
	free(to_fetch);

	printf("\n");
	fflush(stdout);
	strbuf_reset(buf);
}

static int push_dav(int nr_spec, char **specs)
{
	const char **argv = xmalloc((10 + nr_spec) * sizeof(char*));
	int argc = 0, i;

	argv[argc++] = "http-push";
	argv[argc++] = "--helper-status";
	if (options.dry_run)
		argv[argc++] = "--dry-run";
	if (options.verbosity > 1)
		argv[argc++] = "--verbose";
	argv[argc++] = url;
	for (i = 0; i < nr_spec; i++)
		argv[argc++] = specs[i];
	argv[argc++] = NULL;

	if (run_command_v_opt(argv, RUN_GIT_CMD))
		die("git-%s failed", argv[0]);
	free(argv);
	return 0;
}

static void parse_push(struct strbuf *buf)
{
	char **specs = NULL;
	int alloc_spec = 0, nr_spec = 0, i;

	do {
		if (!prefixcmp(buf->buf, "push ")) {
			ALLOC_GROW(specs, nr_spec + 1, alloc_spec);
			specs[nr_spec++] = xstrdup(buf->buf + 5);
		}
		else
			die("http transport does not support %s", buf->buf);

		strbuf_reset(buf);
		if (strbuf_getline(buf, stdin, '\n') == EOF)
			return;
		if (!*buf->buf)
			break;
	} while (1);

	if (push_dav(nr_spec, specs))
		exit(128); /* error already reported */
	for (i = 0; i < nr_spec; i++)
		free(specs[i]);
	free(specs);

	printf("\n");
	fflush(stdout);
}

int main(int argc, const char **argv)
{
	struct strbuf buf = STRBUF_INIT;

	git_extract_argv0_path(argv[0]);
	setup_git_directory();
	if (argc < 2) {
		fprintf(stderr, "Remote needed\n");
		return 1;
	}

	options.verbosity = 1;
	options.progress = !!isatty(2);

	remote = remote_get(argv[1]);

	if (argc > 2) {
		url = argv[2];
	} else {
		url = remote->url[0];
	}

	do {
		if (strbuf_getline(&buf, stdin, '\n') == EOF)
			break;
		if (!prefixcmp(buf.buf, "fetch ")) {
			parse_fetch(&buf);

		} else if (!strcmp(buf.buf, "list") || !prefixcmp(buf.buf, "list ")) {
			output_refs(get_refs());

		} else if (!prefixcmp(buf.buf, "push ")) {
			parse_push(&buf);

		} else if (!prefixcmp(buf.buf, "option ")) {
			char *name = buf.buf + strlen("option ");
			char *value = strchr(name, ' ');
			int result;

			if (value)
				*value++ = '\0';
			else
				value = "true";

			result = set_option(name, value);
			if (!result)
				printf("ok\n");
			else if (result < 0)
				printf("error invalid value\n");
			else
				printf("unsupported\n");
			fflush(stdout);

		} else if (!strcmp(buf.buf, "capabilities")) {
			printf("fetch\n");
			printf("option\n");
			printf("push\n");
			printf("\n");
			fflush(stdout);
		} else {
			return 1;
		}
		strbuf_reset(&buf);
	} while (1);
	return 0;
}
