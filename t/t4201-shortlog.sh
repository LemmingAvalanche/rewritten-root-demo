#!/bin/sh
#
# Copyright (c) 2006 Johannes E. Schindelin
#

test_description='git shortlog
'

. ./test-lib.sh

test_expect_success 'setup' '
	echo 1 >a1 &&
	git add a1 &&
	tree=$(git write-tree) &&
	commit=$(printf "%s\n" "Test" "" | git commit-tree "$tree") &&
	git update-ref HEAD "$commit" &&

	echo 2 >a1 &&
	git commit --quiet -m "This is a very, very long first line for the commit message to see if it is wrapped correctly" a1 &&

	# test if the wrapping is still valid
	# when replacing all is by treble clefs.
	echo 3 >a1 &&
	git commit --quiet -m "$(
		echo "This is a very, very long first line for the commit message to see if it is wrapped correctly" |
		sed "s/i/1234/g" |
		tr 1234 "\360\235\204\236")" a1 &&

	# now fsck up the utf8
	git config i18n.commitencoding non-utf-8 &&
	echo 4 >a1 &&
	git commit --quiet -m "$(
		echo "This is a very, very long first line for the commit message to see if it is wrapped correctly" |
		sed "s/i/1234/g" |
		tr 1234 "\370\235\204\236")" a1 &&

	echo 5 >a1 &&
	git commit --quiet -m "a								12	34	56	78" a1 &&

	echo 6 >a1 &&
	git commit --quiet -m "Commit by someone else" \
		--author="Someone else <not!me>" a1 &&

	cat >expect.template <<-\EOF
	A U Thor (5):
	      SUBJECT
	      SUBJECT
	      SUBJECT
	      SUBJECT
	      SUBJECT

	Someone else (1):
	      SUBJECT

	EOF
'

fuzz() {
	file=$1 &&
	sed "
			s/$_x40/OBJECT_NAME/g
			s/$_x05/OBJID/g
			s/^ \{6\}[CTa].*/      SUBJECT/g
			s/^ \{8\}[^ ].*/        CONTINUATION/g
		" <"$file" >"$file.fuzzy" &&
	sed "/CONTINUATION/ d" <"$file.fuzzy"
}

test_expect_success 'default output format' '
	git shortlog HEAD >log &&
	fuzz log >log.predictable &&
	test_cmp expect.template log.predictable
'

test_expect_success 'pretty format' '
	sed s/SUBJECT/OBJECT_NAME/ expect.template >expect &&
	git shortlog --format="%H" HEAD >log &&
	fuzz log >log.predictable &&
	test_cmp expect log.predictable
'

test_expect_success '--abbrev' '
	sed s/SUBJECT/OBJID/ expect.template >expect &&
	git shortlog --format="%h" --abbrev=5 HEAD >log &&
	fuzz log >log.predictable &&
	test_cmp expect log.predictable
'

test_expect_success 'output from user-defined format is re-wrapped' '
	sed "s/SUBJECT/two lines/" expect.template >expect &&
	git shortlog --format="two%nlines" HEAD >log &&
	fuzz log >log.predictable &&
	test_cmp expect log.predictable
'

test_expect_success !MINGW 'shortlog wrapping' '
	cat >expect <<\EOF &&
A U Thor (5):
      Test
      This is a very, very long first line for the commit message to see if
         it is wrapped correctly
      Th𝄞s 𝄞s a very, very long f𝄞rst l𝄞ne for the comm𝄞t message to see 𝄞f
         𝄞t 𝄞s wrapped correctly
      Th����s ����s a very, very long f����rst l����ne for the comm����t
         message to see ����f ����t ����s wrapped correctly
      a								12	34
         56	78

Someone else (1):
      Commit by someone else

EOF
	git shortlog -w HEAD >out &&
	test_cmp expect out
'

test_expect_success !MINGW 'shortlog from non-git directory' '
	git log --no-expand-tabs HEAD >log &&
	GIT_DIR=non-existing git shortlog -w <log >out &&
	test_cmp expect out
'

test_expect_success !MINGW 'shortlog can read --format=raw output' '
	git log --format=raw HEAD >log &&
	GIT_DIR=non-existing git shortlog -w <log >out &&
	test_cmp expect out
'

test_expect_success 'shortlog should add newline when input line matches wraplen' '
	cat >expect <<\EOF &&
A U Thor (2):
      bbbbbbbbbbbbbbbbbb: bbbbbbbb bbb bbbb bbbbbbb bb bbbb bbb bbbbb bbbbbb
      aaaaaaaaaaaaaaaaaaaaaa: aaaaaa aaaaaaaaaa aaaa aaaaaaaa aa aaaa aa aaa

EOF
	git shortlog -w >out <<\EOF &&
commit 0000000000000000000000000000000000000001
Author: A U Thor <author@example.com>
Date:   Thu Apr 7 15:14:13 2005 -0700

    aaaaaaaaaaaaaaaaaaaaaa: aaaaaa aaaaaaaaaa aaaa aaaaaaaa aa aaaa aa aaa

commit 0000000000000000000000000000000000000002
Author: A U Thor <author@example.com>
Date:   Thu Apr 7 15:14:13 2005 -0700

    bbbbbbbbbbbbbbbbbb: bbbbbbbb bbb bbbb bbbbbbb bb bbbb bbb bbbbb bbbbbb

EOF
	test_cmp expect out
'

iconvfromutf8toiso88591() {
	printf "%s" "$*" | iconv -f UTF-8 -t ISO8859-1
}

DSCHO="Jöhännës \"Dschö\" Schindëlin"
DSCHOE="$DSCHO <Johannes.Schindelin@gmx.de>"
MSG1="set a1 to 2 and some non-ASCII chars: Äßø"
MSG2="set a1 to 3 and some non-ASCII chars: áæï"
cat > expect << EOF
$DSCHO (2):
      $MSG1
      $MSG2

EOF

test_expect_success !MINGW 'shortlog encoding' '
	git reset --hard "$commit" &&
	git config --unset i18n.commitencoding &&
	echo 2 > a1 &&
	git commit --quiet -m "$MSG1" --author="$DSCHOE" a1 &&
	git config i18n.commitencoding "ISO8859-1" &&
	echo 3 > a1 &&
	git commit --quiet -m "$(iconvfromutf8toiso88591 "$MSG2")" \
		--author="$(iconvfromutf8toiso88591 "$DSCHOE")" a1 &&
	git config --unset i18n.commitencoding &&
	git shortlog HEAD~2.. > out &&
test_cmp expect out'

test_expect_success 'shortlog with revision pseudo options' '
	git shortlog --all &&
	git shortlog --branches &&
	git shortlog --exclude=refs/heads/m* --all
'

test_expect_success 'shortlog with --output=<file>' '
	git shortlog --output=shortlog -1 master >output &&
	test ! -s output &&
	test_line_count = 3 shortlog
'

test_expect_success 'shortlog --committer (internal)' '
	cat >expect <<-\EOF &&
	     3	C O Mitter
	EOF
	git shortlog -nsc HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'shortlog --committer (external)' '
	git log --format=full | git shortlog -nsc >actual &&
	test_cmp expect actual
'

test_done
