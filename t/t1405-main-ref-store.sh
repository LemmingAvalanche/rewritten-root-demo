#!/bin/sh

test_description='test main ref store api'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

. ./test-lib.sh

RUN="test-tool ref-store main"

test_expect_success 'pack_refs(PACK_REFS_ALL | PACK_REFS_PRUNE)' '
	test_commit one &&
	N=`find .git/refs -type f | wc -l` &&
	test "$N" != 0 &&
	$RUN pack-refs 3 &&
	N=`find .git/refs -type f | wc -l`
'

test_expect_success 'peel_ref(new-tag)' '
	git rev-parse HEAD >expected &&
	git tag -a -m new-tag new-tag HEAD &&
	$RUN peel-ref refs/tags/new-tag >actual &&
	test_cmp expected actual
'

test_expect_success 'create_symref(FOO, refs/heads/main)' '
	$RUN create-symref FOO refs/heads/main nothing &&
	echo refs/heads/main >expected &&
	git symbolic-ref FOO >actual &&
	test_cmp expected actual
'

test_expect_success 'delete_refs(FOO, refs/tags/new-tag)' '
	git rev-parse FOO -- &&
	git rev-parse refs/tags/new-tag -- &&
	m=$(git rev-parse main) &&
	REF_NO_DEREF=1 &&
	$RUN delete-refs $REF_NO_DEREF nothing FOO refs/tags/new-tag &&
	test_must_fail git rev-parse --symbolic-full-name FOO &&
	test_must_fail git rev-parse FOO -- &&
	test_must_fail git rev-parse refs/tags/new-tag --
'

test_expect_success 'rename_refs(main, new-main)' '
	git rev-parse main >expected &&
	$RUN rename-ref refs/heads/main refs/heads/new-main &&
	git rev-parse new-main >actual &&
	test_cmp expected actual &&
	test_commit recreate-main
'

test_expect_success 'for_each_ref(refs/heads/)' '
	$RUN for-each-ref refs/heads/ | cut -d" " -f 2- >actual &&
	cat >expected <<-\EOF &&
	main 0x0
	new-main 0x0
	EOF
	test_cmp expected actual
'

test_expect_success 'for_each_ref() is sorted' '
	$RUN for-each-ref refs/heads/ | cut -d" " -f 2- >actual &&
	sort actual > expected &&
	test_cmp expected actual
'

test_expect_success 'resolve_ref(new-main)' '
	SHA1=`git rev-parse new-main` &&
	echo "$SHA1 refs/heads/new-main 0x0" >expected &&
	$RUN resolve-ref refs/heads/new-main 0 >actual &&
	test_cmp expected actual
'

test_expect_success 'verify_ref(new-main)' '
	$RUN verify-ref refs/heads/new-main
'

test_expect_success 'for_each_reflog()' '
	$RUN for-each-reflog | sort -k2 | cut -d" " -f 2- >actual &&
	cat >expected <<-\EOF &&
	HEAD 0x1
	refs/heads/main 0x0
	refs/heads/new-main 0x0
	EOF
	test_cmp expected actual
'

test_expect_success 'for_each_reflog_ent()' '
	$RUN for-each-reflog-ent HEAD >actual &&
	head -n1 actual | grep one &&
	tail -n2 actual | head -n1 | grep recreate-main
'

test_expect_success 'for_each_reflog_ent_reverse()' '
	$RUN for-each-reflog-ent-reverse HEAD >actual &&
	head -n1 actual | grep recreate-main &&
	tail -n2 actual | head -n1 | grep one
'

test_expect_success 'reflog_exists(HEAD)' '
	$RUN reflog-exists HEAD
'

test_expect_success 'delete_reflog(HEAD)' '
	$RUN delete-reflog HEAD &&
	! test -f .git/logs/HEAD
'

test_expect_success 'create-reflog(HEAD)' '
	$RUN create-reflog HEAD 1 &&
	test -f .git/logs/HEAD
'

test_expect_success 'delete_ref(refs/heads/foo)' '
	git checkout -b foo &&
	FOO_SHA1=`git rev-parse foo` &&
	git checkout --detach &&
	test_commit bar-commit &&
	git checkout -b bar &&
	BAR_SHA1=`git rev-parse bar` &&
	$RUN update-ref updating refs/heads/foo $BAR_SHA1 $FOO_SHA1 0 &&
	echo $BAR_SHA1 >expected &&
	git rev-parse refs/heads/foo >actual &&
	test_cmp expected actual
'

test_expect_success 'delete_ref(refs/heads/foo)' '
	SHA1=`git rev-parse foo` &&
	git checkout --detach &&
	$RUN delete-ref msg refs/heads/foo $SHA1 0 &&
	test_must_fail git rev-parse refs/heads/foo --
'

test_done
