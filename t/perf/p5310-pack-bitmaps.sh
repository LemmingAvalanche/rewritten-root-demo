#!/bin/sh

test_description='Tests pack performance using bitmaps'
. ./perf-lib.sh
. "${TEST_DIRECTORY}/perf/lib-bitmap.sh"

test_lookup_pack_bitmap () {
	test_expect_success 'start the test from scratch' '
		rm -rf * .git
	'

	test_perf_large_repo

	# note that we do everything through config,
	# since we want to be able to compare bitmap-aware
	# git versus non-bitmap git
	#
	# We intentionally use the deprecated pack.writebitmaps
	# config so that we can test against older versions of git.
	test_expect_success 'setup bitmap config' '
		git config pack.writebitmaps true
	'

	# we need to create the tag up front such that it is covered by the repack and
	# thus by generated bitmaps.
	test_expect_success 'create tags' '
		git tag --message="tag pointing to HEAD" perf-tag HEAD
	'

	test_perf "enable lookup table: $1" '
		git config pack.writeBitmapLookupTable '"$1"'
	'

	test_pack_bitmap
}

test_lookup_pack_bitmap false
test_lookup_pack_bitmap true

test_done
