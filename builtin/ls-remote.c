#include "builtin.h"
#include "cache.h"
#include "transport.h"
#include "remote.h"
#include "refs.h"

static const char * const ls_remote_usage[] = {
	N_("git ls-remote [--heads] [--tags] [--refs] [--upload-pack=<exec>]\n"
	   "                     [-q | --quiet] [--exit-code] [--get-url]\n"
	   "                     [--symref] [<repository> [<refs>...]]"),
	NULL
};

/*
 * Is there one among the list of patterns that match the tail part
 * of the path?
 */
static int tail_match(const char **pattern, const char *path)
{
	const char *p;
	char *pathbuf;

	if (!pattern)
		return 1; /* no restriction */

	pathbuf = xstrfmt("/%s", path);
	while ((p = *(pattern++)) != NULL) {
		if (!wildmatch(p, pathbuf, 0)) {
			free(pathbuf);
			return 1;
		}
	}
	free(pathbuf);
	return 0;
}

int cmd_ls_remote(int argc, const char **argv, const char *prefix)
{
	const char *dest = NULL;
	unsigned flags = 0;
	int get_url = 0;
	int quiet = 0;
	int status = 0;
	int show_symref_target = 0;
	const char *uploadpack = NULL;
	const char **pattern = NULL;
	struct argv_array ref_prefixes = ARGV_ARRAY_INIT;
	struct string_list server_options = STRING_LIST_INIT_DUP;

	struct remote *remote;
	struct transport *transport;
	const struct ref *ref;

	struct option options[] = {
		OPT__QUIET(&quiet, N_("do not print remote URL")),
		OPT_STRING(0, "upload-pack", &uploadpack, N_("exec"),
			   N_("path of git-upload-pack on the remote host")),
		{ OPTION_STRING, 0, "exec", &uploadpack, N_("exec"),
			   N_("path of git-upload-pack on the remote host"),
			   PARSE_OPT_HIDDEN },
		OPT_BIT('t', "tags", &flags, N_("limit to tags"), REF_TAGS),
		OPT_BIT('h', "heads", &flags, N_("limit to heads"), REF_HEADS),
		OPT_BIT(0, "refs", &flags, N_("do not show peeled tags"), REF_NORMAL),
		OPT_BOOL(0, "get-url", &get_url,
			 N_("take url.<base>.insteadOf into account")),
		OPT_SET_INT_F(0, "exit-code", &status,
			      N_("exit with exit code 2 if no matching refs are found"),
			      2, PARSE_OPT_NOCOMPLETE),
		OPT_BOOL(0, "symref", &show_symref_target,
			 N_("show underlying ref in addition to the object pointed by it")),
		OPT_STRING_LIST('o', "server-option", &server_options, N_("server-specific"), N_("option to transmit")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, ls_remote_usage,
			     PARSE_OPT_STOP_AT_NON_OPTION);
	dest = argv[0];

	if (argc > 1) {
		int i;
		pattern = xcalloc(argc, sizeof(const char *));
		for (i = 1; i < argc; i++) {
			const char *glob;
			pattern[i - 1] = xstrfmt("*/%s", argv[i]);

			glob = strchr(argv[i], '*');
			if (glob)
				argv_array_pushf(&ref_prefixes, "%.*s",
						 (int)(glob - argv[i]), argv[i]);
			else
				expand_ref_prefix(&ref_prefixes, argv[i]);
		}
	}

	remote = remote_get(dest);
	if (!remote) {
		if (dest)
			die("bad repository '%s'", dest);
		die("No remote configured to list refs from.");
	}
	if (!remote->url_nr)
		die("remote %s has no configured URL", dest);

	if (get_url) {
		printf("%s\n", *remote->url);
		return 0;
	}

	transport = transport_get(remote, NULL);
	if (uploadpack != NULL)
		transport_set_option(transport, TRANS_OPT_UPLOADPACK, uploadpack);
	if (server_options.nr)
		transport->server_options = &server_options;

	ref = transport_get_remote_refs(transport, &ref_prefixes);
	if (transport_disconnect(transport))
		return 1;

	if (!dest && !quiet)
		fprintf(stderr, "From %s\n", *remote->url);
	for ( ; ref; ref = ref->next) {
		if (!check_ref_type(ref, flags))
			continue;
		if (!tail_match(pattern, ref->name))
			continue;
		if (show_symref_target && ref->symref)
			printf("ref: %s\t%s\n", ref->symref, ref->name);
		printf("%s\t%s\n", oid_to_hex(&ref->old_oid), ref->name);
		status = 0; /* we found something */
	}
	return status;
}
