#ifndef REACHEABLE_H
#define REACHEABLE_H

struct progress;
extern void mark_reachable_objects(struct rev_info *revs, int mark_reflog,
				   unsigned long mark_recent, struct progress *);

#endif
