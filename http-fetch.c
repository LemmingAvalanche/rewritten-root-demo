#include "cache.h"
#include "config.h"
#include "exec-cmd.h"
#include "http.h"
#include "walker.h"

static const char http_fetch_usage[] = "git http-fetch "
"[-c] [-t] [-a] [-v] [--recover] [-w ref] [--stdin] commit-id url";

static int fetch_using_walker(const char *raw_url, int get_verbosely,
			      int get_recover, int commits, char **commit_id,
			      const char **write_ref, int commits_on_stdin)
{
	char *url = NULL;
	struct walker *walker;
	int rc;

	str_end_url_with_slash(raw_url, &url);

	http_init(NULL, url, 0);

	walker = get_http_walker(url);
	walker->get_verbosely = get_verbosely;
	walker->get_recover = get_recover;
	walker->get_progress = 0;

	rc = walker_fetch(walker, commits, commit_id, write_ref, url);

	if (commits_on_stdin)
		walker_targets_free(commits, commit_id, write_ref);

	if (walker->corrupt_object_found) {
		fprintf(stderr,
"Some loose object were found to be corrupt, but they might be just\n"
"a false '404 Not Found' error message sent with incorrect HTTP\n"
"status code.  Suggest running 'git fsck'.\n");
	}

	walker_free(walker);
	http_cleanup();
	free(url);

	return rc;
}

int cmd_main(int argc, const char **argv)
{
	int commits_on_stdin = 0;
	int commits;
	const char **write_ref = NULL;
	char **commit_id;
	int arg = 1;
	int get_verbosely = 0;
	int get_recover = 0;

	while (arg < argc && argv[arg][0] == '-') {
		if (argv[arg][1] == 't') {
		} else if (argv[arg][1] == 'c') {
		} else if (argv[arg][1] == 'a') {
		} else if (argv[arg][1] == 'v') {
			get_verbosely = 1;
		} else if (argv[arg][1] == 'w') {
			write_ref = &argv[arg + 1];
			arg++;
		} else if (argv[arg][1] == 'h') {
			usage(http_fetch_usage);
		} else if (!strcmp(argv[arg], "--recover")) {
			get_recover = 1;
		} else if (!strcmp(argv[arg], "--stdin")) {
			commits_on_stdin = 1;
		}
		arg++;
	}
	if (argc != arg + 2 - commits_on_stdin)
		usage(http_fetch_usage);
	if (commits_on_stdin) {
		commits = walker_targets_stdin(&commit_id, &write_ref);
	} else {
		commit_id = (char **) &argv[arg++];
		commits = 1;
	}

	setup_git_directory();

	git_config(git_default_config, NULL);

	if (!argv[arg])
		BUG("must have one arg remaining");

	return fetch_using_walker(argv[arg], get_verbosely, get_recover,
				  commits, commit_id, write_ref,
				  commits_on_stdin);
}
