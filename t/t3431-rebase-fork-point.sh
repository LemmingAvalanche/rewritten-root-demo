#!/bin/sh
#
# Copyright (c) 2019 Denton Liu
#

test_description='git rebase --fork-point test'

. ./test-lib.sh

# A---B---D---E    (master)
#      \
#       C*---F---G (side)
#
# C was formerly part of master but master was rewound to remove C
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

do_test_rebase () {
	expected="$1" &&
	shift &&
	git checkout master &&
	git reset --hard E &&
	git checkout side &&
	git reset --hard G &&
	git rebase $* &&
	test_write_lines $expected >expect &&
	git log --pretty=%s >actual &&
	test_cmp expect actual
}

test_rebase () {
	expected="$1" &&
	shift &&
	test_expect_success "git rebase $*" "do_test_rebase '$expected' $*"
}

test_rebase 'G F E D B A'
test_rebase 'G F D B A' --onto D
test_rebase 'G F B A' --keep-base
test_rebase 'G F C E D B A' --no-fork-point
test_rebase 'G F C D B A' --no-fork-point --onto D
test_rebase 'G F C B A' --no-fork-point --keep-base

test_rebase 'G F E D B A' --fork-point refs/heads/master
test_rebase 'G F E D B A' --fork-point master

test_rebase 'G F D B A' --fork-point --onto D refs/heads/master
test_rebase 'G F D B A' --fork-point --onto D master

test_rebase 'G F B A' --fork-point --keep-base refs/heads/master
test_rebase 'G F B A' --fork-point --keep-base master

test_rebase 'G F C E D B A' refs/heads/master
test_rebase 'G F C E D B A' master

test_rebase 'G F C D B A' --onto D refs/heads/master
test_rebase 'G F C D B A' --onto D master

test_rebase 'G F C B A' --keep-base refs/heads/master
test_rebase 'G F C B A' --keep-base master

test_expect_success 'git rebase --fork-point with ambigous refname' '
	git checkout master &&
	git checkout -b one &&
	git checkout side &&
	git tag one &&
	test_must_fail git rebase --fork-point --onto D one
'

test_expect_success '--fork-point and --root both given' '
	test_must_fail git rebase --fork-point --root 2>err &&
	test_i18ngrep "cannot combine" err
'

test_expect_success 'rebase.forkPoint set to false' '
	test_config rebase.forkPoint false &&
	do_test_rebase "G F C E D B A"
'

test_expect_success 'rebase.forkPoint set to false and then to true' '
	test_config_global rebase.forkPoint false &&
	test_config rebase.forkPoint true &&
	do_test_rebase "G F E D B A"
'

test_expect_success 'rebase.forkPoint set to false and command line says --fork-point' '
	test_config rebase.forkPoint false &&
	do_test_rebase "G F E D B A" --fork-point
'

test_expect_success 'rebase.forkPoint set to true and command line says --no-fork-point' '
	test_config rebase.forkPoint true &&
	do_test_rebase "G F C E D B A" --no-fork-point
'

test_expect_success 'rebase.forkPoint set to true and --root given' '
	test_config rebase.forkPoint true &&
	git rebase --root
'

test_done
