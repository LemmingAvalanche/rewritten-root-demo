#ifndef PROGRESS_H
#define PROGRESS_H

struct progress;

#ifdef GIT_TEST_PROGRESS_ONLY

extern int progress_testing;
extern uint64_t progress_test_ns;
void progress_test_force_update(void);

#endif

void display_throughput(struct progress *progress, uint64_t total);
void display_progress(struct progress *progress, uint64_t n);
struct progress *start_progress(const char *title, uint64_t total);
struct progress *start_sparse_progress(const char *title, uint64_t total);
struct progress *start_delayed_progress(const char *title, uint64_t total);
struct progress *start_delayed_sparse_progress(const char *title,
					       uint64_t total);
void stop_progress(struct progress **progress);
void stop_progress_msg(struct progress **progress, const char *msg);

#endif
