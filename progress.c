#include "git-compat-util.h"
#include "progress.h"

struct progress {
	const char *title;
	int last_value;
	unsigned total;
	unsigned last_percent;
	unsigned delay;
	unsigned delayed_percent_treshold;
};

static volatile sig_atomic_t progress_update;

static void progress_interval(int signum)
{
	progress_update = 1;
}

static void set_progress_signal(void)
{
	struct sigaction sa;
	struct itimerval v;

	progress_update = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = progress_interval;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGALRM, &sa, NULL);

	v.it_interval.tv_sec = 1;
	v.it_interval.tv_usec = 0;
	v.it_value = v.it_interval;
	setitimer(ITIMER_REAL, &v, NULL);
}

static void clear_progress_signal(void)
{
	struct itimerval v = {{0,},};
	setitimer(ITIMER_REAL, &v, NULL);
	signal(SIGALRM, SIG_IGN);
	progress_update = 0;
}

static int display(struct progress *progress, unsigned n, int done)
{
	char *eol;

	if (progress->delay) {
		if (!progress_update || --progress->delay)
			return 0;
		if (progress->total) {
			unsigned percent = n * 100 / progress->total;
			if (percent > progress->delayed_percent_treshold) {
				/* inhibit this progress report entirely */
				clear_progress_signal();
				progress->delay = -1;
				progress->total = 0;
				return 0;
			}
		}
	}

	progress->last_value = n;
	eol = done ? ", done.   \n" : "   \r";
	if (progress->total) {
		unsigned percent = n * 100 / progress->total;
		if (percent != progress->last_percent || progress_update) {
			progress->last_percent = percent;
			fprintf(stderr, "%s: %3u%% (%u/%u)%s", progress->title,
				percent, n, progress->total, eol);
			progress_update = 0;
			return 1;
		}
	} else if (progress_update) {
		fprintf(stderr, "%s: %u%s", progress->title, n, eol);
		progress_update = 0;
		return 1;
	}

	return 0;
}

int display_progress(struct progress *progress, unsigned n)
{
	return progress ? display(progress, n, 0) : 0;
}

struct progress *start_progress_delay(const char *title, unsigned total,
				       unsigned percent_treshold, unsigned delay)
{
	struct progress *progress = malloc(sizeof(*progress));
	if (!progress) {
		/* unlikely, but here's a good fallback */
		fprintf(stderr, "%s...\n", title);
		return NULL;
	}
	progress->title = title;
	progress->total = total;
	progress->last_value = -1;
	progress->last_percent = -1;
	progress->delayed_percent_treshold = percent_treshold;
	progress->delay = delay;
	set_progress_signal();
	return progress;
}

struct progress *start_progress(const char *title, unsigned total)
{
	return start_progress_delay(title, total, 0, 0);
}

void stop_progress(struct progress **p_progress)
{
	struct progress *progress = *p_progress;
	if (!progress)
		return;
	*p_progress = NULL;
	if (progress->last_value != -1) {
		/* Force the last update */
		progress_update = 1;
		display(progress, progress->last_value, 1);
	}
	clear_progress_signal();
	free(progress);
}
