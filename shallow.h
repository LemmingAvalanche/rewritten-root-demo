#ifndef SHALLOW_H
#define SHALLOW_H

#include "lockfile.h"
#include "object.h"
#include "repository.h"
#include "strbuf.h"

void set_alternate_shallow_file(struct repository *r, const char *path, int override);
int register_shallow(struct repository *r, const struct object_id *oid);
int unregister_shallow(const struct object_id *oid);
int is_repository_shallow(struct repository *r);
int commit_shallow_file(struct repository *r, struct lock_file *lk);
void rollback_shallow_file(struct repository *r, struct lock_file *lk);

struct commit_list *get_shallow_commits(struct object_array *heads,
					int depth, int shallow_flag, int not_shallow_flag);
struct commit_list *get_shallow_commits_by_rev_list(
		int ac, const char **av, int shallow_flag, int not_shallow_flag);
int write_shallow_commits(struct strbuf *out, int use_pack_protocol,
			  const struct oid_array *extra);

void setup_alternate_shallow(struct lock_file *shallow_lock,
			     const char **alternate_shallow_file,
			     const struct oid_array *extra);

const char *setup_temporary_shallow(const struct oid_array *extra);

void advertise_shallow_grafts(int);

#define PRUNE_SHOW_ONLY 1
#define PRUNE_QUICK 2
void prune_shallow(unsigned options);

/*
 * Initialize with prepare_shallow_info() or zero-initialize (equivalent to
 * prepare_shallow_info with a NULL oid_array).
 */
struct shallow_info {
	struct oid_array *shallow;
	int *ours, nr_ours;
	int *theirs, nr_theirs;
	struct oid_array *ref;

	/* for receive-pack */
	uint32_t **used_shallow;
	int *need_reachability_test;
	int *reachable;
	int *shallow_ref;
	struct commit **commits;
	int nr_commits;
};

void prepare_shallow_info(struct shallow_info *, struct oid_array *);
void clear_shallow_info(struct shallow_info *);
void remove_nonexistent_theirs_shallow(struct shallow_info *);
void assign_shallow_commits_to_refs(struct shallow_info *info,
				    uint32_t **used,
				    int *ref_status);
int delayed_reachability_test(struct shallow_info *si, int c);

extern struct trace_key trace_shallow;

#endif /* SHALLOW_H */
