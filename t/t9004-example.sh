#!/bin/sh

test_description='check that example code compiles and runs'
. ./test-lib.sh

test_expect_success 'decorate' '
	test-example-decorate
'

test_done
