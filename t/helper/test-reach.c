#include "test-tool.h"
#include "cache.h"
#include "commit.h"
#include "commit-reach.h"
#include "config.h"
#include "parse-options.h"
#include "string-list.h"
#include "tag.h"

static void print_sorted_commit_ids(struct commit_list *list)
{
	int i;
	struct string_list s = STRING_LIST_INIT_DUP;

	while (list) {
		string_list_append(&s, oid_to_hex(&list->item->object.oid));
		list = list->next;
	}

	string_list_sort(&s);

	for (i = 0; i < s.nr; i++)
		printf("%s\n", s.items[i].string);

	string_list_clear(&s, 0);
}

int cmd__reach(int ac, const char **av)
{
	struct object_id oid_A, oid_B;
	struct commit *A, *B;
	struct commit_list *X;
	struct commit **X_array;
	int X_nr, X_alloc;
	struct strbuf buf = STRBUF_INIT;
	struct repository *r = the_repository;

	setup_git_directory();

	if (ac < 2)
		exit(1);

	A = B = NULL;
	X = NULL;
	X_nr = 0;
	X_alloc = 16;
	ALLOC_ARRAY(X_array, X_alloc);

	while (strbuf_getline(&buf, stdin) != EOF) {
		struct object_id oid;
		struct object *o;
		struct commit *c;
		if (buf.len < 3)
			continue;

		if (get_oid_committish(buf.buf + 2, &oid))
			die("failed to resolve %s", buf.buf + 2);

		o = parse_object(r, &oid);
		o = deref_tag_noverify(o);

		if (!o)
			die("failed to load commit for input %s resulting in oid %s\n",
			    buf.buf, oid_to_hex(&oid));

		c = object_as_type(r, o, OBJ_COMMIT, 0);

		if (!c)
			die("failed to load commit for input %s resulting in oid %s\n",
			    buf.buf, oid_to_hex(&oid));

		switch (buf.buf[0]) {
			case 'A':
				oidcpy(&oid_A, &oid);
				A = c;
				break;

			case 'B':
				oidcpy(&oid_B, &oid);
				B = c;
				break;

			case 'X':
				commit_list_insert(c, &X);
				ALLOC_GROW(X_array, X_nr + 1, X_alloc);
				X_array[X_nr++] = c;
				break;

			default:
				die("unexpected start of line: %c", buf.buf[0]);
		}
	}
	strbuf_release(&buf);

	if (!strcmp(av[1], "ref_newer"))
		printf("%s(A,B):%d\n", av[1], ref_newer(&oid_A, &oid_B));
	else if (!strcmp(av[1], "in_merge_bases"))
		printf("%s(A,B):%d\n", av[1], in_merge_bases(A, B));
	else if (!strcmp(av[1], "is_descendant_of"))
		printf("%s(A,X):%d\n", av[1], is_descendant_of(A, X));
	else if (!strcmp(av[1], "get_merge_bases_many")) {
		struct commit_list *list = get_merge_bases_many(A, X_nr, X_array);
		printf("%s(A,X):\n", av[1]);
		print_sorted_commit_ids(list);
	} else if (!strcmp(av[1], "reduce_heads")) {
		struct commit_list *list = reduce_heads(X);
		printf("%s(X):\n", av[1]);
		print_sorted_commit_ids(list);
	}

	exit(0);
}
