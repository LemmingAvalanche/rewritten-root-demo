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

test_three_modes () {
	test_when_finished rm -rf .git/objects/info/commit-graph &&
	test-tool reach $1 <input >actual &&
	test_cmp expect actual &&
	cp commit-graph-full .git/objects/info/commit-graph &&
	test-tool reach $1 <input >actual &&
	test_cmp expect actual &&
	cp commit-graph-half .git/objects/info/commit-graph &&
	test-tool reach $1 <input >actual &&
	test_cmp expect actual
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

test_done
