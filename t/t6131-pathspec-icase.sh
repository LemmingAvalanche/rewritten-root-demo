#!/bin/sh

test_description='test case insensitive pathspec limiting'
. ./test-lib.sh

test_expect_success 'create commits with glob characters' '
	test_commit bar bar &&
	test_commit bAr bAr &&
	test_commit BAR BAR &&
	mkdir foo &&
	test_commit foo/bar foo/bar &&
	test_commit foo/bAr foo/bAr &&
	test_commit foo/BAR foo/BAR &&
	mkdir fOo &&
	test_commit fOo/bar fOo/bar &&
	test_commit fOo/bAr fOo/bAr &&
	test_commit fOo/BAR fOo/BAR &&
	mkdir FOO &&
	test_commit FOO/bar FOO/bar &&
	test_commit FOO/bAr FOO/bAr &&
	test_commit FOO/BAR FOO/BAR
'

test_expect_success 'tree_entry_interesting matches bar' '
	echo bar >expect &&
	git log --format=%s -- "bar" >actual &&
	test_cmp expect actual
'

test_expect_success 'tree_entry_interesting matches :(icase)bar' '
	cat <<-EOF >expect &&
	BAR
	bAr
	bar
	EOF
	git log --format=%s -- ":(icase)bar" >actual &&
	test_cmp expect actual
'

test_expect_success 'tree_entry_interesting matches :(icase)bar with prefix' '
	cat <<-EOF >expect &&
	fOo/BAR
	fOo/bAr
	fOo/bar
	EOF
	( cd fOo && git log --format=%s -- ":(icase)bar" ) >actual &&
	test_cmp expect actual
'

test_expect_success 'tree_entry_interesting matches :(icase)bar with empty prefix' '
	cat <<-EOF >expect &&
	FOO/BAR
	FOO/bAr
	FOO/bar
	fOo/BAR
	fOo/bAr
	fOo/bar
	foo/BAR
	foo/bAr
	foo/bar
	EOF
	( cd fOo && git log --format=%s -- ":(icase)../foo/bar" ) >actual &&
	test_cmp expect actual
'

test_expect_success 'match_pathspec_depth matches :(icase)bar' '
	cat <<-EOF >expect &&
	BAR
	bAr
	bar
	EOF
	git ls-files ":(icase)bar" >actual &&
	test_cmp expect actual
'

test_expect_success 'match_pathspec_depth matches :(icase)bar with prefix' '
	cat <<-EOF >expect &&
	fOo/BAR
	fOo/bAr
	fOo/bar
	EOF
	( cd fOo && git ls-files --full-name ":(icase)bar" ) >actual &&
	test_cmp expect actual
'

test_expect_success 'match_pathspec_depth matches :(icase)bar with empty prefix' '
	cat <<-EOF >expect &&
	bar
	fOo/BAR
	fOo/bAr
	fOo/bar
	EOF
	( cd fOo && git ls-files --full-name ":(icase)bar" ../bar ) >actual &&
	test_cmp expect actual
'

test_done
