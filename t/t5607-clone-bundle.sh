#!/bin/sh

test_description='some bundle related tests'
GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=master
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

test_expect_success 'setup' '
	test_oid_cache <<-EOF &&
	version sha1:2
	version sha256:3
	EOF
	test_commit initial &&
	test_tick &&
	git tag -m tag tag &&
	test_commit second &&
	test_commit third &&
	git tag -d initial &&
	git tag -d second &&
	git tag -d third
'

test_expect_success '"verify" needs a worktree' '
	git bundle create tip.bundle -1 master &&
	nongit test_must_fail git bundle verify ../tip.bundle 2>err &&
	test_i18ngrep "need a repository" err
'

test_expect_success 'annotated tags can be excluded by rev-list options' '
	git bundle create bundle --all --since=7.Apr.2005.15:14:00.-0700 &&
	git ls-remote bundle > output &&
	grep tag output &&
	git bundle create bundle --all --since=7.Apr.2005.15:16:00.-0700 &&
	git ls-remote bundle > output &&
	! grep tag output
'

test_expect_success 'die if bundle file cannot be created' '
	mkdir adir &&
	test_must_fail git bundle create adir --all
'

test_expect_failure 'bundle --stdin' '
	echo master | git bundle create stdin-bundle.bdl --stdin &&
	git ls-remote stdin-bundle.bdl >output &&
	grep master output
'

test_expect_failure 'bundle --stdin <rev-list options>' '
	echo master | git bundle create hybrid-bundle.bdl --stdin tag &&
	git ls-remote hybrid-bundle.bdl >output &&
	grep master output
'

test_expect_success 'empty bundle file is rejected' '
	: >empty-bundle &&
	test_must_fail git fetch empty-bundle
'

# This triggers a bug in older versions where the resulting line (with
# --pretty=oneline) was longer than a 1024-char buffer.
test_expect_success 'ridiculously long subject in boundary' '
	: >file4 &&
	test_tick &&
	git add file4 &&
	printf "%01200d\n" 0 | git commit -F - &&
	test_commit fifth &&
	git bundle create long-subject-bundle.bdl HEAD^..HEAD &&
	git bundle list-heads long-subject-bundle.bdl >heads &&
	test -s heads &&
	git fetch long-subject-bundle.bdl &&
	sed -n "/^-/{p;q;}" long-subject-bundle.bdl >boundary &&
	grep "^-$OID_REGEX " boundary
'

test_expect_success 'prerequisites with an empty commit message' '
	: >file1 &&
	git add file1 &&
	test_tick &&
	git commit --allow-empty-message -m "" &&
	test_commit file2 &&
	git bundle create bundle HEAD^.. &&
	git bundle verify bundle
'

test_expect_success 'failed bundle creation does not leave cruft' '
	# This fails because the bundle would be empty.
	test_must_fail git bundle create fail.bundle master..master &&
	test_path_is_missing fail.bundle.lock
'

test_expect_success 'fetch SHA-1 from bundle' '
	test_create_repo foo &&
	test_commit -C foo x &&
	git -C foo bundle create tip.bundle -1 master &&
	git -C foo rev-parse HEAD >hash &&

	# Exercise to ensure that fetching a SHA-1 from a bundle works with no
	# errors
	git fetch --no-tags foo/tip.bundle "$(cat hash)"
'

test_expect_success 'git bundle uses expected default format' '
	git bundle create bundle HEAD^.. &&
	head -n1 bundle | grep "^# v$(test_oid version) git bundle$"
'

test_expect_success 'git bundle v3 has expected contents' '
	git branch side HEAD &&
	git bundle create --version=3 bundle HEAD^..side &&
	head -n2 bundle >actual &&
	cat >expect <<-EOF &&
	# v3 git bundle
	@object-format=$(test_oid algo)
	EOF
	test_cmp expect actual &&
	git bundle verify bundle
'

test_expect_success 'git bundle v3 rejects unknown capabilities' '
	cat >new <<-EOF &&
	# v3 git bundle
	@object-format=$(test_oid algo)
	@unknown=silly
	EOF
	test_must_fail git bundle verify new 2>output &&
	test_i18ngrep "unknown capability .unknown=silly." output
'

test_done
