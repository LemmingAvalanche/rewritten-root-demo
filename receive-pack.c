#include "cache.h"
#include "pkt-line.h"
#include <sys/wait.h>

static const char receive_pack_usage[] = "git-receive-pack [--unpack=executable] <git-dir> [heads]";

static const char *unpacker = "git-unpack-objects";

static int path_match(const char *path, int nr, char **match)
{
	int i;
	int pathlen = strlen(path);

	for (i = 0; i < nr; i++) {
		char *s = match[i];
		int len = strlen(s);

		if (!len || len > pathlen)
			continue;
		if (memcmp(path + pathlen - len, s, len))
			continue;
		if (pathlen > len && path[pathlen - len - 1] != '/')
			continue;
		*s = 0;
		return 1;
	}
	return 0;
}

static void show_ref(const char *path, unsigned char *sha1)
{
	packet_write(1, "%s %s\n", sha1_to_hex(sha1), path);
}

static int read_ref(const char *path, unsigned char *sha1)
{
	int ret = -1;
	int fd = open(path, O_RDONLY);

	if (fd >= 0) {
		char buffer[60];
		if (read(fd, buffer, sizeof(buffer)) >= 40)
			ret = get_sha1_hex(buffer, sha1);
		close(fd);
	}
	return ret;
}

static void write_head_info(const char *base, int nr, char **match)
{
	DIR *dir = opendir(base);

	if (dir) {
		struct dirent *de;
		int baselen = strlen(base);
		char *path = xmalloc(baselen + 257);
		memcpy(path, base, baselen);

		while ((de = readdir(dir)) != NULL) {
			char sha1[20];
			struct stat st;
			int namelen;

			if (de->d_name[0] == '.')
				continue;
			namelen = strlen(de->d_name);
			if (namelen > 255)
				continue;
			memcpy(path + baselen, de->d_name, namelen+1);
			if (lstat(path, &st) < 0)
				continue;
			if (S_ISDIR(st.st_mode)) {
				path[baselen + namelen] = '/';
				path[baselen + namelen + 1] = 0;
				write_head_info(path, nr, match);
				continue;
			}
			if (read_ref(path, sha1) < 0)
				continue;
			if (nr && !path_match(path, nr, match))
				continue;
			show_ref(path, sha1);
		}
		free(path);
		closedir(dir);
	}
}

struct command {
	struct command *next;
	unsigned char old_sha1[20];
	unsigned char new_sha1[20];
	char ref_name[0];
};

struct command *commands = NULL;

/*
 * This gets called after(if) we've successfully
 * unpacked the data payload.
 */
static void execute_commands(void)
{
	struct command *cmd = commands;

	while (cmd) {
		char old_hex[60], *new_hex;
		strcpy(old_hex, sha1_to_hex(cmd->old_sha1));
		new_hex = sha1_to_hex(cmd->new_sha1);
		fprintf(stderr, "%s: %s -> %s\n", cmd->ref_name, old_hex, new_hex);
		cmd = cmd->next;
	}
}

static void read_head_info(void)
{
	struct command **p = &commands;
	for (;;) {
		static char line[1000];
		unsigned char old_sha1[20], new_sha1[20];
		struct command *cmd;
		int len;

		len = packet_read_line(0, line, sizeof(line));
		if (!len)
			break;
		if (line[len-1] == '\n')
			line[--len] = 0;
		if (len < 83 ||
		    line[40] != ' ' ||
		    line[81] != ' ' ||
		    get_sha1_hex(line, old_sha1) ||
		    get_sha1_hex(line + 41, new_sha1))
			die("protocol error: expected old/new/ref, got '%s'", line);
		cmd = xmalloc(sizeof(struct command) + len - 80);
		memcpy(cmd->old_sha1, old_sha1, 20);
		memcpy(cmd->new_sha1, new_sha1, 20);
		memcpy(cmd->ref_name, line + 82, len - 81);
		cmd->next = NULL;
		*p = cmd;
		p = &cmd->next;
	}
}

static void unpack(void)
{
	pid_t pid = fork();

	if (pid < 0)
		die("unpack fork failed");
	if (!pid) {
		setenv("GIT_DIR", ".", 1);
		execlp(unpacker, unpacker, NULL);
		die("unpack execute failed");
	}

	for (;;) {
		int status, code;
		int retval = waitpid(pid, &status, 0);

		if (retval < 0) {
			if (errno == EINTR)
				continue;
			die("waitpid failed (%s)", strerror(retval));
		}
		if (retval != pid)
			die("waitpid is confused");
		if (WIFSIGNALED(status))
			die("%s died of signal %d", unpacker, WTERMSIG(status));
		if (!WIFEXITED(status))
			die("%s died out of really strange complications", unpacker);
		code = WEXITSTATUS(status);
		if (code)
			die("%s exited with error code %d", unpacker, code);
		return;
	}
}

int main(int argc, char **argv)
{
	int i, nr_heads = 0;
	const char *dir = NULL;
	char **heads = NULL;

	argv++;
	for (i = 1; i < argc; i++) {
		const char *arg = *argv++;

		if (*arg == '-') {
			if (!strncmp(arg, "--unpack=", 9)) {
				unpacker = arg+9;
				continue;
			}
			/* Do flag handling here */
			usage(receive_pack_usage);
		}
		dir = arg;
		heads = argv;
		nr_heads = argc - i - 1;
		break;
	}
	if (!dir)
		usage(receive_pack_usage);

	/* chdir to the directory. If that fails, try appending ".git" */
	if (chdir(dir) < 0) {
		static char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s.git", dir);
		if (chdir(path) < 0)
			die("unable to cd to %s", dir);
	}

	/* If we have a ".git" directory, chdir to it */
	chdir(".git");

	if (access("objects", X_OK) < 0 || access("refs/heads", X_OK) < 0)
		die("%s doesn't appear to be a git directory", dir);
	write_head_info("refs/", nr_heads, heads);

	/* EOF */
	packet_flush(1);

	read_head_info();
	if (commands) {
		unpack();
		execute_commands();
	}
	return 0;
}
