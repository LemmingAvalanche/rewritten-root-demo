#ifndef FETCH_PACK_H
#define FETCH_PACK_H

struct fetch_pack_args
{
	const char *uploadpack;
	int unpacklimit;
	int depth;
	unsigned quiet:1,
		keep_pack:1,
		use_thin_pack:1,
		fetch_all:1,
		verbose:1,
		no_progress:1;
};

void setup_fetch_pack(struct fetch_pack_args *args);

struct ref *fetch_pack(const char *dest, int nr_heads, char **heads, char **pack_lockfile);

#endif
