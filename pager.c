#include "cache.h"

/*
 * This is split up from the rest of git so that we might do
 * something different on Windows, for example.
 */

static void run_pager(const char *pager)
{
	execlp(pager, pager, NULL);
}

void setup_pager(void)
{
	pid_t pid;
	int fd[2];
	const char *pager = getenv("PAGER");

	if (!isatty(1))
		return;
	if (!pager)
		pager = "less";
	else if (!*pager || !strcmp(pager, "cat"))
		return;

	if (pipe(fd) < 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return;
	}

	/* return in the child */
	if (!pid) {
		dup2(fd[1], 1);
		close(fd[0]);
		close(fd[1]);
		return;
	}

	/* The original process turns into the PAGER */
	dup2(fd[0], 0);
	close(fd[0]);
	close(fd[1]);

	setenv("LESS", "-S", 0);
	run_pager(pager);
	exit(255);
}
