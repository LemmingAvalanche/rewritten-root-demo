#include "cache.h"
#include "refs.h"
#include "pkt-line.h"
#include "tag.h"
#include "object.h"
#include "commit.h"

static const char upload_pack_usage[] = "git-upload-pack [--strict] [--timeout=nn] <dir>";

#define THEY_HAVE (1U << 0)
#define MAX_HAS 256
#define MAX_NEEDS 256
static int nr_has = 0, nr_needs = 0, multi_ack = 0;
static unsigned char has_sha1[MAX_HAS][20];
static unsigned char needs_sha1[MAX_NEEDS][20];
static unsigned int timeout = 0;

static void reset_timeout(void)
{
	alarm(timeout);
}

static int strip(char *line, int len)
{
	if (len && line[len-1] == '\n')
		line[--len] = 0;
	return len;
}

static void create_pack_file(void)
{
	int fd[2];
	pid_t pid;

	if (pipe(fd) < 0)
		die("git-upload-pack: unable to create pipe");
	pid = fork();
	if (pid < 0)
		die("git-upload-pack: unable to fork git-rev-list");

	if (!pid) {
		int i;
		int args;
		char **argv;
		char *buf;
		char **p;

		if (MAX_NEEDS <= nr_needs)
			args = nr_has + 10;
		else
			args = nr_has + nr_needs + 5;
		argv = xmalloc(args * sizeof(char *));
		buf = xmalloc(args * 45);
		p = argv;

		dup2(fd[1], 1);
		close(0);
		close(fd[0]);
		close(fd[1]);
		*p++ = "git-rev-list";
		*p++ = "--objects";
		if (MAX_NEEDS <= nr_needs)
			*p++ = "--all";
		else {
			for (i = 0; i < nr_needs; i++) {
				*p++ = buf;
				memcpy(buf, sha1_to_hex(needs_sha1[i]), 41);
				buf += 41;
			}
		}
		for (i = 0; i < nr_has; i++) {
			*p++ = buf;
			*buf++ = '^';
			memcpy(buf, sha1_to_hex(has_sha1[i]), 41);
			buf += 41;
		}
		*p++ = NULL;
		execvp("git-rev-list", argv);
		die("git-upload-pack: unable to exec git-rev-list");
	}
	dup2(fd[0], 0);
	close(fd[0]);
	close(fd[1]);
	execlp("git-pack-objects", "git-pack-objects", "--stdout", NULL);
	die("git-upload-pack: unable to exec git-pack-objects");
}

static int got_sha1(char *hex, unsigned char *sha1)
{
	if (get_sha1_hex(hex, sha1))
		die("git-upload-pack: expected SHA1 object, got '%s'", hex);
	if (!has_sha1_file(sha1))
		return 0;
	if (nr_has < MAX_HAS) {
		struct object *o = lookup_object(sha1);
		if (!(o && o->parsed))
			o = parse_object(sha1);
		if (!o)
			die("oops (%s)", sha1_to_hex(sha1));
		if (o->type == commit_type) {
			struct commit_list *parents;
			if (o->flags & THEY_HAVE)
				return 0;
			o->flags |= THEY_HAVE;
			for (parents = ((struct commit*)o)->parents;
			     parents;
			     parents = parents->next)
				parents->item->object.flags |= THEY_HAVE;
		}
		memcpy(has_sha1[nr_has++], sha1, 20);
	}
	return 1;
}

static int get_common_commits(void)
{
	static char line[1000];
	unsigned char sha1[20];
	int len;

	track_object_refs = 0;
	save_commit_buffer = 0;

	for(;;) {
		len = packet_read_line(0, line, sizeof(line));
		reset_timeout();

		if (!len) {
			if (multi_ack || nr_has == 0)
				packet_write(1, "NAK\n");
			continue;
		}
		len = strip(line, len);
		if (!strncmp(line, "have ", 5)) {
			if (got_sha1(line+5, sha1) &&
					(multi_ack || nr_has == 1))
				packet_write(1, "ACK %s%s\n",
					sha1_to_hex(sha1),
					multi_ack && nr_has < MAX_HAS ?
					" continue" : "");
			continue;
		}
		if (!strcmp(line, "done")) {
			if (nr_has > 0)
				return 0;
			packet_write(1, "NAK\n");
			return -1;
		}
		die("git-upload-pack: expected SHA1 list, got '%s'", line);
	}
}

static int receive_needs(void)
{
	static char line[1000];
	int len, needs;

	needs = 0;
	for (;;) {
		unsigned char dummy[20], *sha1_buf;
		len = packet_read_line(0, line, sizeof(line));
		reset_timeout();
		if (!len)
			return needs;

		sha1_buf = dummy;
		if (needs == MAX_NEEDS) {
			fprintf(stderr,
				"warning: supporting only a max of %d requests. "
				"sending everything instead.\n",
				MAX_NEEDS);
		}
		else if (needs < MAX_NEEDS)
			sha1_buf = needs_sha1[needs];

		if (strncmp("want ", line, 5) || get_sha1_hex(line+5, sha1_buf))
			die("git-upload-pack: protocol error, "
			    "expected to get sha, not '%s'", line);

		if (strstr(line+45, "multi_ack"))
			multi_ack = 1;

		needs++;
	}
}

static int send_ref(const char *refname, const unsigned char *sha1)
{
	struct object *o = parse_object(sha1);

	packet_write(1, "%s %s\n", sha1_to_hex(sha1), refname);
	if (o->type == tag_type) {
		o = deref_tag(o);
		packet_write(1, "%s %s^{}\n", sha1_to_hex(o->sha1), refname);
	}
	return 0;
}

static int upload_pack(void)
{
	reset_timeout();
	head_ref(send_ref);
	for_each_ref(send_ref);
	packet_flush(1);
	nr_needs = receive_needs();
	if (!nr_needs)
		return 0;
	get_common_commits();
	create_pack_file();
	return 0;
}

int main(int argc, char **argv)
{
	const char *dir;
	int i;
	int strict = 0;

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];

		if (arg[0] != '-')
			break;
		if (!strcmp(arg, "--strict")) {
			strict = 1;
			continue;
		}
		if (!strncmp(arg, "--timeout=", 10)) {
			timeout = atoi(arg+10);
			continue;
		}
		if (!strcmp(arg, "--")) {
			i++;
			break;
		}
	}
	
	if (i != argc-1)
		usage(upload_pack_usage);
	dir = argv[i];

	/* chdir to the directory. If that fails, try appending ".git" */
	if (chdir(dir) < 0) {
		if (strict || chdir(mkpath("%s.git", dir)) < 0)
			die("git-upload-pack unable to chdir to %s", dir);
	}
	if (!strict)
		chdir(".git");

	if (access("objects", X_OK) || access("refs", X_OK))
		die("git-upload-pack: %s doesn't seem to be a git archive", dir);

	putenv("GIT_DIR=.");
	upload_pack();
	return 0;
}
