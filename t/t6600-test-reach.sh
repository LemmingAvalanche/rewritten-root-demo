#!/bin/sh

test_description='basic commit reachability tests'

. ./test-lib.sh

# Construct a grid-like commit graph with points (x,y)
# with 1 <= x <= 10, 1 <= y <= 10, where (x,y) has
# parents (x-1, y) and (x, y-1), keeping in mind that
# we drop a parent if a coordinate is nonpositive.
#
#             (10,10)
#            /       \
#         (10,9)    (9,10)
#        /     \   /      \
#    (10,8)    (9,9)      (8,10)
#   /     \    /   \      /    \
#         ( continued...)
#   \     /    \   /      \    /
#    (3,1)     (2,2)      (1,3)
#        \     /    \     /
#         (2,1)      (2,1)
#              \    /
#              (1,1)
#
# We use branch 'commit-x-y' to refer to (x,y).
# This grid allows interesting reachability and
# non-reachability queries: (x,y) can reach (x',y')
# if and only if x' <= x and y' <= y.
test_expect_success 'setup' '
	for i in $(test_seq 1 10)
	do
		test_commit "1-$i" &&
		git branch -f commit-1-$i
	done &&
	for j in $(test_seq 1 9)
	do
		git reset --hard commit-$j-1 &&
		x=$(($j + 1)) &&
		test_commit "$x-1" &&
		git branch -f commit-$x-1 &&

		for i in $(test_seq 2 10)
		do
			git merge commit-$j-$i -m "$x-$i" &&
			git branch -f commit-$x-$i
		done
	done &&
	git commit-graph write --reachable &&
	mv .git/objects/info/commit-graph commit-graph-full &&
	git show-ref -s commit-5-5 | git commit-graph write --stdin-commits &&
	mv .git/objects/info/commit-graph commit-graph-half &&
	git config core.commitGraph true
'

run_three_modes () {
	test_when_finished rm -rf .git/objects/info/commit-graph &&
	"$@" <input >actual &&
	test_cmp expect actual &&
	cp commit-graph-full .git/objects/info/commit-graph &&
	"$@" <input >actual &&
	test_cmp expect actual &&
	cp commit-graph-half .git/objects/info/commit-graph &&
	"$@" <input >actual &&
	test_cmp expect actual
}

test_three_modes () {
	run_three_modes test-tool reach "$@"
}

test_expect_success 'ref_newer:miss' '
	cat >input <<-\EOF &&
	A:commit-5-7
	B:commit-4-9
	EOF
	echo "ref_newer(A,B):0" >expect &&
	test_three_modes ref_newer
'

test_expect_success 'ref_newer:hit' '
	cat >input <<-\EOF &&
	A:commit-5-7
	B:commit-2-3
	EOF
	echo "ref_newer(A,B):1" >expect &&
	test_three_modes ref_newer
'

test_expect_success 'in_merge_bases:hit' '
	cat >input <<-\EOF &&
	A:commit-5-7
	B:commit-8-8
	EOF
	echo "in_merge_bases(A,B):1" >expect &&
	test_three_modes in_merge_bases
'

test_expect_success 'in_merge_bases:miss' '
	cat >input <<-\EOF &&
	A:commit-6-8
	B:commit-5-9
	EOF
	echo "in_merge_bases(A,B):0" >expect &&
	test_three_modes in_merge_bases
'

test_expect_success 'is_descendant_of:hit' '
	cat >input <<-\EOF &&
	A:commit-5-7
	X:commit-4-8
	X:commit-6-6
	X:commit-1-1
	EOF
	echo "is_descendant_of(A,X):1" >expect &&
	test_three_modes is_descendant_of
'

test_expect_success 'is_descendant_of:miss' '
	cat >input <<-\EOF &&
	A:commit-6-8
	X:commit-5-9
	X:commit-4-10
	X:commit-7-6
	EOF
	echo "is_descendant_of(A,X):0" >expect &&
	test_three_modes is_descendant_of
'

test_expect_success 'get_merge_bases_many' '
	cat >input <<-\EOF &&
	A:commit-5-7
	X:commit-4-8
	X:commit-6-6
	X:commit-8-3
	EOF
	{
		echo "get_merge_bases_many(A,X):" &&
		git rev-parse commit-5-6 \
			      commit-4-7 | sort
	} >expect &&
	test_three_modes get_merge_bases_many
'

test_expect_success 'reduce_heads' '
	cat >input <<-\EOF &&
	X:commit-1-10
	X:commit-2-8
	X:commit-3-6
	X:commit-4-4
	X:commit-1-7
	X:commit-2-5
	X:commit-3-3
	X:commit-5-1
	EOF
	{
		echo "reduce_heads(X):" &&
		git rev-parse commit-5-1 \
			      commit-4-4 \
			      commit-3-6 \
			      commit-2-8 \
			      commit-1-10 | sort
	} >expect &&
	test_three_modes reduce_heads
'

test_expect_success 'can_all_from_reach:hit' '
	cat >input <<-\EOF &&
	X:commit-2-10
	X:commit-3-9
	X:commit-4-8
	X:commit-5-7
	X:commit-6-6
	X:commit-7-5
	X:commit-8-4
	X:commit-9-3
	Y:commit-1-9
	Y:commit-2-8
	Y:commit-3-7
	Y:commit-4-6
	Y:commit-5-5
	Y:commit-6-4
	Y:commit-7-3
	Y:commit-8-1
	EOF
	echo "can_all_from_reach(X,Y):1" >expect &&
	test_three_modes can_all_from_reach
'

test_expect_success 'can_all_from_reach:miss' '
	cat >input <<-\EOF &&
	X:commit-2-10
	X:commit-3-9
	X:commit-4-8
	X:commit-5-7
	X:commit-6-6
	X:commit-7-5
	X:commit-8-4
	X:commit-9-3
	Y:commit-1-9
	Y:commit-2-8
	Y:commit-3-7
	Y:commit-4-6
	Y:commit-5-5
	Y:commit-6-4
	Y:commit-8-5
	EOF
	echo "can_all_from_reach(X,Y):0" >expect &&
	test_three_modes can_all_from_reach
'

test_expect_success 'commit_contains:hit' '
	cat >input <<-\EOF &&
	A:commit-7-7
	X:commit-2-10
	X:commit-3-9
	X:commit-4-8
	X:commit-5-7
	X:commit-6-6
	X:commit-7-5
	X:commit-8-4
	X:commit-9-3
	EOF
	echo "commit_contains(_,A,X,_):1" >expect &&
	test_three_modes commit_contains &&
	test_three_modes commit_contains --tag
'

test_expect_success 'commit_contains:miss' '
	cat >input <<-\EOF &&
	A:commit-6-5
	X:commit-2-10
	X:commit-3-9
	X:commit-4-8
	X:commit-5-7
	X:commit-6-6
	X:commit-7-5
	X:commit-8-4
	X:commit-9-3
	EOF
	echo "commit_contains(_,A,X,_):0" >expect &&
	test_three_modes commit_contains &&
	test_three_modes commit_contains --tag
'

test_expect_success 'rev-list: basic topo-order' '
	git rev-parse \
		commit-6-6 commit-5-6 commit-4-6 commit-3-6 commit-2-6 commit-1-6 \
		commit-6-5 commit-5-5 commit-4-5 commit-3-5 commit-2-5 commit-1-5 \
		commit-6-4 commit-5-4 commit-4-4 commit-3-4 commit-2-4 commit-1-4 \
		commit-6-3 commit-5-3 commit-4-3 commit-3-3 commit-2-3 commit-1-3 \
		commit-6-2 commit-5-2 commit-4-2 commit-3-2 commit-2-2 commit-1-2 \
		commit-6-1 commit-5-1 commit-4-1 commit-3-1 commit-2-1 commit-1-1 \
	>expect &&
	run_three_modes git rev-list --topo-order commit-6-6
'

test_expect_success 'rev-list: first-parent topo-order' '
	git rev-parse \
		commit-6-6 \
		commit-6-5 \
		commit-6-4 \
		commit-6-3 \
		commit-6-2 \
		commit-6-1 commit-5-1 commit-4-1 commit-3-1 commit-2-1 commit-1-1 \
	>expect &&
	run_three_modes git rev-list --first-parent --topo-order commit-6-6
'

test_expect_success 'rev-list: range topo-order' '
	git rev-parse \
		commit-6-6 commit-5-6 commit-4-6 commit-3-6 commit-2-6 commit-1-6 \
		commit-6-5 commit-5-5 commit-4-5 commit-3-5 commit-2-5 commit-1-5 \
		commit-6-4 commit-5-4 commit-4-4 commit-3-4 commit-2-4 commit-1-4 \
		commit-6-3 commit-5-3 commit-4-3 \
		commit-6-2 commit-5-2 commit-4-2 \
		commit-6-1 commit-5-1 commit-4-1 \
	>expect &&
	run_three_modes git rev-list --topo-order commit-3-3..commit-6-6
'

test_expect_success 'rev-list: range topo-order' '
	git rev-parse \
		commit-6-6 commit-5-6 commit-4-6 \
		commit-6-5 commit-5-5 commit-4-5 \
		commit-6-4 commit-5-4 commit-4-4 \
		commit-6-3 commit-5-3 commit-4-3 \
		commit-6-2 commit-5-2 commit-4-2 \
		commit-6-1 commit-5-1 commit-4-1 \
	>expect &&
	run_three_modes git rev-list --topo-order commit-3-8..commit-6-6
'

test_expect_success 'rev-list: first-parent range topo-order' '
	git rev-parse \
		commit-6-6 \
		commit-6-5 \
		commit-6-4 \
		commit-6-3 \
		commit-6-2 \
		commit-6-1 commit-5-1 commit-4-1 \
	>expect &&
	run_three_modes git rev-list --first-parent --topo-order commit-3-8..commit-6-6
'

test_expect_success 'rev-list: ancestry-path topo-order' '
	git rev-parse \
		commit-6-6 commit-5-6 commit-4-6 commit-3-6 \
		commit-6-5 commit-5-5 commit-4-5 commit-3-5 \
		commit-6-4 commit-5-4 commit-4-4 commit-3-4 \
		commit-6-3 commit-5-3 commit-4-3 \
	>expect &&
	run_three_modes git rev-list --topo-order --ancestry-path commit-3-3..commit-6-6
'

test_expect_success 'rev-list: symmetric difference topo-order' '
	git rev-parse \
		commit-6-6 commit-5-6 commit-4-6 \
		commit-6-5 commit-5-5 commit-4-5 \
		commit-6-4 commit-5-4 commit-4-4 \
		commit-6-3 commit-5-3 commit-4-3 \
		commit-6-2 commit-5-2 commit-4-2 \
		commit-6-1 commit-5-1 commit-4-1 \
		commit-3-8 commit-2-8 commit-1-8 \
		commit-3-7 commit-2-7 commit-1-7 \
	>expect &&
	run_three_modes git rev-list --topo-order commit-3-8...commit-6-6
'

test_done
