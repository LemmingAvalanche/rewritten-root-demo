#!/bin/sh
#
# Copyright (c) 2019 Denton Liu
#

test_description='git rebase --fork-point test'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

# A---B---D---E    (main)
#      \
#       C*---F---G (side)
#
# C was formerly part of main but main was rewound to remove C
#
test_expect_success setup '
	test_commit A &&
	test_commit B &&
	test_commit C &&
	git branch -t side &&
	git reset --hard HEAD^ &&
	test_commit D &&
	test_commit E &&
	git checkout side &&
	test_commit F &&
	test_commit G
'

test_rebase () {
	expected="$1" &&
	shift &&
	test_expect_success "git rebase $*" "
		git checkout main &&
		git reset --hard E &&
		git checkout side &&
		git reset --hard G &&
		git rebase $* &&
		test_write_lines $expected >expect &&
		git log --pretty=%s >actual &&
		test_cmp expect actual
	"
}

test_rebase 'G F E D B A'
test_rebase 'G F D B A' --onto D
test_rebase 'G F B A' --keep-base
test_rebase 'G F C E D B A' --no-fork-point
test_rebase 'G F C D B A' --no-fork-point --onto D
test_rebase 'G F C B A' --no-fork-point --keep-base

test_rebase 'G F E D B A' --fork-point refs/heads/main
test_rebase 'G F E D B A' --fork-point main

test_rebase 'G F D B A' --fork-point --onto D refs/heads/main
test_rebase 'G F D B A' --fork-point --onto D main

test_rebase 'G F B A' --fork-point --keep-base refs/heads/main
test_rebase 'G F B A' --fork-point --keep-base main

test_rebase 'G F C E D B A' refs/heads/main
test_rebase 'G F C E D B A' main

test_rebase 'G F C D B A' --onto D refs/heads/main
test_rebase 'G F C D B A' --onto D main

test_rebase 'G F C B A' --keep-base refs/heads/main
test_rebase 'G F C B A' --keep-base main

test_expect_success 'git rebase --fork-point with ambigous refname' '
	git checkout main &&
	git checkout -b one &&
	git checkout side &&
	git tag one &&
	test_must_fail git rebase --fork-point --onto D one
'

test_done
