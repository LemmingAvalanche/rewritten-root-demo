#!/bin/sh

test_description='--ancestry-path'

#          D---E-------F
#         /     \       \
#    B---C---G---H---I---J
#   /                     \
#  A-------K---------------L--M
#
#  D..M                 == E F G H I J K L M
#  --ancestry-path D..M == E F H I J L M
#
#  D..M -- M.t                 == M
#  --ancestry-path D..M -- M.t == M
#
#  F...I                 == F G H I
#  --ancestry-path F...I == F H I
#
#  G..M -- G.t                 == [nothing - was dropped in "-s ours" merge L]
#  --ancestry-path G..M -- G.t == L
#  --ancestry-path --simplify-merges G^..M -- G.t == G L

. ./test-lib.sh

test_merge () {
	test_tick &&
	git merge -s ours -m "$2" "$1" &&
	git tag "$2"
}

test_expect_success setup '
	test_commit A &&
	test_commit B &&
	test_commit C &&
	test_commit D &&
	test_commit E &&
	test_commit F &&
	git reset --hard C &&
	test_commit G &&
	test_merge E H &&
	test_commit I &&
	test_merge F J &&
	git reset --hard A &&
	test_commit K &&
	test_merge J L &&
	test_commit M
'

test_expect_success 'rev-list D..M' '
	for c in E F G H I J K L M; do echo $c; done >expect &&
	git rev-list --format=%s D..M |
	sed -e "/^commit /d" |
	sort >actual &&
	test_cmp expect actual
'

test_expect_success 'rev-list --ancestry-path D..M' '
	for c in E F H I J L M; do echo $c; done >expect &&
	git rev-list --ancestry-path --format=%s D..M |
	sed -e "/^commit /d" |
	sort >actual &&
	test_cmp expect actual
'

test_expect_success 'rev-list D..M -- M.t' '
	echo M >expect &&
	git rev-list --format=%s D..M -- M.t |
	sed -e "/^commit /d" >actual &&
	test_cmp expect actual
'

test_expect_success 'rev-list --ancestry-path D..M -- M.t' '
	echo M >expect &&
	git rev-list --ancestry-path --format=%s D..M -- M.t |
	sed -e "/^commit /d" >actual &&
	test_cmp expect actual
'

test_expect_success 'rev-list F...I' '
	for c in F G H I; do echo $c; done >expect &&
	git rev-list --format=%s F...I |
	sed -e "/^commit /d" |
	sort >actual &&
	test_cmp expect actual
'

test_expect_success 'rev-list --ancestry-path F...I' '
	for c in F H I; do echo $c; done >expect &&
	git rev-list --ancestry-path --format=%s F...I |
	sed -e "/^commit /d" |
	sort >actual &&
	test_cmp expect actual
'

# G.t is dropped in an "-s ours" merge
test_expect_success 'rev-list G..M -- G.t' '
	git rev-list --format=%s G..M -- G.t |
	sed -e "/^commit /d" >actual &&
	test_must_be_empty actual
'

test_expect_success 'rev-list --ancestry-path G..M -- G.t' '
	echo L >expect &&
	git rev-list --ancestry-path --format=%s G..M -- G.t |
	sed -e "/^commit /d" >actual &&
	test_cmp expect actual
'

test_expect_success 'rev-list --ancestry-path --simplify-merges G^..M -- G.t' '
	for c in G L; do echo $c; done >expect &&
	git rev-list --ancestry-path --simplify-merges --format=%s G^..M -- G.t |
	sed -e "/^commit /d" |
	sort >actual &&
	test_cmp expect actual
'

#   b---bc
#  / \ /
# a   X
#  \ / \
#   c---cb
#
# All refnames prefixed with 'x' to avoid confusion with the tags
# generated by test_commit on case-insensitive systems.
test_expect_success 'setup criss-cross' '
	mkdir criss-cross &&
	(cd criss-cross &&
	 git init &&
	 test_commit A &&
	 git checkout -b xb master &&
	 test_commit B &&
	 git checkout -b xc master &&
	 test_commit C &&
	 git checkout -b xbc xb -- &&
	 git merge xc &&
	 git checkout -b xcb xc -- &&
	 git merge xb &&
	 git checkout master)
'

# no commits in bc descend from cb
test_expect_success 'criss-cross: rev-list --ancestry-path cb..bc' '
	(cd criss-cross &&
	 git rev-list --ancestry-path xcb..xbc > actual &&
	 test_must_be_empty actual)
'

# no commits in repository descend from cb
test_expect_success 'criss-cross: rev-list --ancestry-path --all ^cb' '
	(cd criss-cross &&
	 git rev-list --ancestry-path --all ^xcb > actual &&
	 test_must_be_empty actual)
'

test_done
