#!/bin/sh
#
# Copyright (c) 2012 Zbigniew Jędrzejewski-Szmek
#

test_description='test --stat output of various commands'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-terminal.sh

# 120 character name
name=aaaaaaaaaa
name=$name$name$name$name$name$name$name$name$name$name$name$name
test_expect_success 'preparation' '
	>"$name" &&
	git add "$name" &&
	git commit -m message &&
	echo a >"$name" &&
	git commit -m message "$name"
'

while read cmd args
do
	cat >expect <<-'EOF'
	 ...aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa |    1 +
	EOF
	test_expect_success "$cmd: small change with long name gives more space to the name" '
		git $cmd $args >output &&
		grep " | " output >actual &&
		test_cmp expect actual
	'

	cat >expect <<-'EOF'
	 ...aaaaaaaaaaaaaaaaaaaaaaaaaa |    1 +
	EOF
	test_expect_success "$cmd --stat=width: a long name is given more room when the bar is short" '
		git $cmd $args --stat=40 >output &&
		grep " | " output >actual &&
		test_cmp expect actual
	'

	test_expect_success "$cmd --stat-width=width with long name" '
		git $cmd $args --stat-width=40 >output &&
		grep " | " output >actual &&
		test_cmp expect actual
	'

	cat >expect <<-'EOF'
	 ...aaaaaaaaaaaaaaaaaaaaaaaaaaa |    1 +
	EOF
	test_expect_success "$cmd --stat=...,name-width with long name" '
		git $cmd $args --stat=60,30 >output &&
		grep " | " output >actual &&
		test_cmp expect actual
	'

	test_expect_success "$cmd --stat-name-width with long name" '
		git $cmd $args --stat-name-width=30 >output &&
		grep " | " output >actual &&
		test_cmp expect actual
	'
done <<\EOF
format-patch -1 --stdout
diff HEAD^ HEAD --stat
show --stat
log -1 --stat
EOF


test_expect_success 'preparation for big change tests' '
	>abcd &&
	git add abcd &&
	git commit -m message &&
	i=0 &&
	while test $i -lt 1000
	do
		echo $i && i=$(($i + 1))
	done >abcd &&
	git commit -m message abcd
'

cat >expect80 <<'EOF'
 abcd | 1000 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
EOF

cat >expect200 <<'EOF'
 abcd | 1000 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
EOF

while read verb expect cmd args
do
	test_expect_success "$cmd $verb COLUMNS (big change)" '
		COLUMNS=200 git $cmd $args >output
		grep " | " output >actual &&
		test_cmp "$expect" actual
	'
done <<\EOF
ignores expect80 format-patch -1 --stdout
respects expect200 diff HEAD^ HEAD --stat
respects expect200 show --stat
respects expect200 log -1 --stat
EOF

cat >expect <<'EOF'
 abcd | 1000 ++++++++++++++++++++++++++
EOF
while read cmd args
do
	test_expect_success "$cmd --stat=width with big change" '
		git $cmd $args --stat=40 >output
		grep " | " output >actual &&
		test_cmp expect actual
	'

	test_expect_success "$cmd --stat-width=width with big change" '
		git $cmd $args --stat-width=40 >output
		grep " | " output >actual &&
		test_cmp expect actual
	'
done <<\EOF
format-patch -1 --stdout
diff HEAD^ HEAD --stat
show --stat
log -1 --stat
EOF

test_expect_success 'preparation for long filename tests' '
	cp abcd aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa &&
	git add aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa &&
	git commit -m message
'

cat >expect <<'EOF'
 ...aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa | 1000 ++++++++++++
EOF
while read cmd args
do
	test_expect_success "$cmd --stat=width with big change is more balanced" '
		git $cmd $args --stat-width=60 >output &&
		grep " | " output >actual &&
		test_cmp expect actual
	'
done <<\EOF
format-patch -1 --stdout
diff HEAD^ HEAD --stat
show --stat
log -1 --stat
EOF

cat >expect80 <<'EOF'
 ...aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa | 1000 ++++++++++++++++++++
EOF
cat >expect200 <<'EOF'
 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa | 1000 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
EOF
while read verb expect cmd args
do
	test_expect_success "$cmd $verb COLUMNS (long filename)" '
		COLUMNS=200 git $cmd $args >output
		grep " | " output >actual &&
		test_cmp "$expect" actual
	'
done <<\EOF
ignores expect80 format-patch -1 --stdout
respects expect200 diff HEAD^ HEAD --stat
respects expect200 show --stat
respects expect200 log -1 --stat
EOF

cat >expect <<'EOF'
 abcd | 1000 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
EOF
test_expect_success 'merge --stat respects COLUMNS (big change)' '
	git checkout -b branch HEAD^^ &&
	COLUMNS=100 git merge --stat --no-ff master^ >output &&
	grep " | " output >actual
	test_cmp expect actual
'

cat >expect <<'EOF'
 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa | 1000 +++++++++++++++++++++++++++++++++++++++
EOF
test_expect_success 'merge --stat respects COLUMNS (long filename)' '
	COLUMNS=100 git merge --stat --no-ff master >output &&
	grep " | " output >actual
	test_cmp expect actual
'

test_done
