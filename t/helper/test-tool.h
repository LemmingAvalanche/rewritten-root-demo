#ifndef __TEST_TOOL_H__
#define __TEST_TOOL_H__

int cmd__chmtime(int argc, const char **argv);
int cmd__config(int argc, const char **argv);
int cmd__ctype(int argc, const char **argv);
int cmd__date(int argc, const char **argv);
int cmd__delta(int argc, const char **argv);
int cmd__drop_caches(int argc, const char **argv);
int cmd__dump_cache_tree(int argc, const char **argv);
int cmd__dump_split_index(int argc, const char **argv);
int cmd__example_decorate(int argc, const char **argv);
int cmd__genrandom(int argc, const char **argv);
int cmd__hashmap(int argc, const char **argv);
int cmd__index_version(int argc, const char **argv);
int cmd__lazy_init_name_hash(int argc, const char **argv);
int cmd__match_trees(int argc, const char **argv);
int cmd__mergesort(int argc, const char **argv);
int cmd__mktemp(int argc, const char **argv);
int cmd__online_cpus(int argc, const char **argv);
int cmd__path_utils(int argc, const char **argv);
int cmd__prio_queue(int argc, const char **argv);
int cmd__read_cache(int argc, const char **argv);
int cmd__ref_store(int argc, const char **argv);
int cmd__regex(int argc, const char **argv);
int cmd__revision_walking(int argc, const char **argv);
int cmd__run_command(int argc, const char **argv);
int cmd__scrap_cache_tree(int argc, const char **argv);
int cmd__sha1(int argc, const char **argv);

#endif
