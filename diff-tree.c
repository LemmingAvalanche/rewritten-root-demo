#include "cache.h"

static int recursive = 0;

static int diff_tree_sha1(const unsigned char *old, const unsigned char *new, const char *src, const char *dst);

static void update_tree_entry(void **bufp, unsigned long *sizep)
{
	void *buf = *bufp;
	unsigned long size = *sizep;
	int len = strlen(buf) + 1 + 20;

	if (size < len)
		usage("corrupt tree file");
	*bufp = buf + len;
	*sizep = size - len;
}

static const unsigned char *extract(void *tree, unsigned long size, const char **pathp, unsigned int *modep)
{
	int len = strlen(tree)+1;
	const unsigned char *sha1 = tree + len;
	const char *path = strchr(tree, ' ');

	if (!path || size < len + 20 || sscanf(tree, "%o", modep) != 1)
		usage("corrupt tree file");
	*pathp = path+1;
	return sha1;
}

static void show_file(const char *prefix, void *tree, unsigned long size, const char *base)
{
	unsigned mode;
	const char *path;
	const unsigned char *sha1 = extract(tree, size, &path, &mode);
	printf("%s%o %s %s%s%c", prefix, mode, sha1_to_hex(sha1), base, path, 0);
}

static int compare_tree_entry(void *tree1, unsigned long size1, void *tree2, unsigned long size2, const char *src, const char *dst)
{
	unsigned mode1, mode2;
	const char *path1, *path2;
	const unsigned char *sha1, *sha2;
	int cmp, pathlen1, pathlen2;

	sha1 = extract(tree1, size1, &path1, &mode1);
	sha2 = extract(tree2, size2, &path2, &mode2);

	pathlen1 = strlen(path1);
	pathlen2 = strlen(path2);
	cmp = cache_name_compare(path1, pathlen1, path2, pathlen2);
	if (cmp < 0) {
		show_file("-", tree1, size1, src);
		return -1;
	}
	if (cmp > 0) {
		show_file("+", tree2, size2, dst);
		return 1;
	}
	if (!memcmp(sha1, sha2, 20) && mode1 == mode2)
		return 0;
	if (recursive && S_ISDIR(mode1) && S_ISDIR(mode2)) {
		int srclen = strlen(src);
		int dstlen = strlen(dst);
		char *srcbase = malloc(srclen + pathlen1 + 2);
		char *dstbase = malloc(srclen + pathlen1 + 2);
		memcpy(srcbase, src, srclen);
		memcpy(srcbase + srclen, path1, pathlen1);
		memcpy(srcbase + srclen + pathlen1, "/", 2);
		memcpy(dstbase, dst, dstlen);
		memcpy(dstbase + dstlen, path2, pathlen2);
		memcpy(dstbase + dstlen + pathlen2, "/", 2);
		return diff_tree_sha1(sha1, sha2, srcbase, dstbase);
	}

	show_file("<", tree1, size1, src);
	show_file(">", tree2, size2, dst);
	return 0;
}

static int diff_tree(void *tree1, unsigned long size1, void *tree2, unsigned long size2, const char *src, const char *dst)
{
	while (size1 | size2) {
		if (!size1) {
			show_file("+", tree2, size2, dst);
			update_tree_entry(&tree2, &size2);
			continue;
		}
		if (!size2) {
			show_file("-", tree1, size1, src);
			update_tree_entry(&tree1, &size1);
			continue;
		}
		switch (compare_tree_entry(tree1, size1, tree2, size2, src, dst)) {
		case -1:
			update_tree_entry(&tree1, &size1);
			continue;
		case 0:
			update_tree_entry(&tree1, &size1);
			/* Fallthrough */
		case 1:
			update_tree_entry(&tree2, &size2);
			continue;
		}
		usage("diff-tree: internal error");
	}
	return 0;
}

static int diff_tree_sha1(const unsigned char *old, const unsigned char *new, const char *src, const char *dst)
{
	void *tree1, *tree2;
	unsigned long size1, size2;
	char type[20];
	int retval;

	tree1 = read_sha1_file(old, type, &size1);
	if (!tree1 || strcmp(type, "tree"))
		usage("unable to read source tree");
	tree2 = read_sha1_file(new, type, &size2);
	if (!tree2 || strcmp(type, "tree"))
		usage("unable to read destination tree");
	retval = diff_tree(tree1, size1, tree2, size2, src, dst);
	free(tree1);
	free(tree2);
	return retval;
}

int main(int argc, char **argv)
{
	unsigned char old[20], new[20];

	while (argc > 3) {
		char *arg = argv[1];
		argv++;
		argc--;
		if (!strcmp(arg, "-R")) {
			recursive = 1;
			continue;
		}
		usage("diff-tree [-R] <tree sha1> <tree sha1>");
	}

	if (argc != 3 || get_sha1_hex(argv[1], old) || get_sha1_hex(argv[2], new))
		usage("diff-tree <tree sha1> <tree sha1>");
	return diff_tree_sha1(old, new, "", "");
}
