// Fully migrated "the_repository" additions
@@
@@
(
// cache.h
- get_oid
+ repo_get_oid
|
- get_oid_commit
+ repo_get_oid_commit
|
- get_oid_committish
+ repo_get_oid_committish
|
- get_oid_tree
+ repo_get_oid_tree
|
- get_oid_treeish
+ repo_get_oid_treeish
|
- get_oid_blob
+ repo_get_oid_blob
|
- get_oid_mb
+ repo_get_oid_mb
|
- find_unique_abbrev
+ repo_find_unique_abbrev
|
- find_unique_abbrev_r
+ repo_find_unique_abbrev_r
|
- for_each_abbrev
+ repo_for_each_abbrev
|
- interpret_branch_name
+ repo_interpret_branch_name
|
- peel_to_type
+ repo_peel_to_type
)
  (
+ the_repository,
  ...)
