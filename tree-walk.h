#ifndef TREE_WALK_H
#define TREE_WALK_H

struct name_entry {
	const unsigned char *sha1;
	const char *path;
	unsigned int mode;
};

struct tree_desc {
	const void *buffer;
	struct name_entry entry;
	unsigned int size;
};

static inline const unsigned char *tree_entry_extract(struct tree_desc *desc, const char **pathp, unsigned int *modep)
{
	*pathp = desc->entry.path;
	*modep = canon_mode(desc->entry.mode);
	return desc->entry.sha1;
}

static inline int tree_entry_len(const char *name, const unsigned char *sha1)
{
	return (char *)sha1 - (char *)name - 1;
}

void update_tree_entry(struct tree_desc *);
void init_tree_desc(struct tree_desc *desc, const void *buf, unsigned long size);

/* Helper function that does both of the above and returns true for success */
int tree_entry(struct tree_desc *, struct name_entry *);

void *fill_tree_descriptor(struct tree_desc *desc, const unsigned char *sha1);

typedef void (*traverse_callback_t)(int n, unsigned long mask, struct name_entry *entry, const char *base);

void traverse_trees(int n, struct tree_desc *t, const char *base, traverse_callback_t callback);

int get_tree_entry(const unsigned char *, const char *, unsigned char *, unsigned *);

#endif
