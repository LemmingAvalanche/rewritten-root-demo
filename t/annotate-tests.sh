# This file isn't used as a test script directly, instead it is
# sourced from t8001-annotate.sh and t8002-blame.sh.

check_count () {
	head= &&
	case "$1" in -h) head="$2"; shift; shift ;; esac &&
	echo "$PROG file $head" >&4 &&
	$PROG file $head >actual &&
	perl -e '
		my %expect = (@ARGV);
		my %count = map { $_ => 0 } keys %expect;
		while (<STDIN>) {
			if (/^[0-9a-f]+\t\(([^\t]+)\t/) {
				my $author = $1;
				for ($author) { s/^\s*//; s/\s*$//; }
				$count{$author}++;
			}
		}
		my $bad = 0;
		while (my ($author, $count) = each %count) {
			my $ok;
			my $value = 0;
			$value = $expect{$author} if defined $expect{$author};
			if ($value != $count) {
				$bad = 1;
				$ok = "bad";
			}
			else {
				$ok = "good";
			}
			print STDERR "Author $author (expected $value, attributed $count) $ok\n";
		}
		exit($bad);
	' "$@" <actual
}

test_expect_success 'setup A lines' '
	echo "1A quick brown fox jumps over the" >file &&
	echo "lazy dog" >>file &&
	git add file &&
	GIT_AUTHOR_NAME="A" GIT_AUTHOR_EMAIL="A@test.git" \
	git commit -a -m "Initial."
'

test_expect_success 'blame 1 author' '
	check_count A 2
'

test_expect_success 'setup B lines' '
	echo "2A quick brown fox jumps over the" >>file &&
	echo "lazy dog" >>file &&
	GIT_AUTHOR_NAME="B" GIT_AUTHOR_EMAIL="B@test.git" \
	git commit -a -m "Second."
'

test_expect_success 'blame 2 authors' '
	check_count A 2 B 2
'

test_expect_success 'setup B1 lines (branch1)' '
	git checkout -b branch1 master &&
	echo "3A slow green fox jumps into the" >>file &&
	echo "well." >>file &&
	GIT_AUTHOR_NAME="B1" GIT_AUTHOR_EMAIL="B1@test.git" \
	git commit -a -m "Branch1-1"
'

test_expect_success 'blame 2 authors + 1 branch1 author' '
	check_count A 2 B 2 B1 2
'

test_expect_success 'setup B2 lines (branch2)' '
	git checkout -b branch2 master &&
	sed -e "s/2A quick brown/4A quick brown lazy dog/" <file >file.new &&
	mv file.new file &&
	GIT_AUTHOR_NAME="B2" GIT_AUTHOR_EMAIL="B2@test.git" \
	git commit -a -m "Branch2-1"
'

test_expect_success 'blame 2 authors + 1 branch2 author' '
	check_count A 2 B 1 B2 1
'

test_expect_success 'merge branch1 & branch2' '
	git pull . branch1
'

test_expect_success 'blame 2 authors + 2 merged-in authors' '
	check_count A 2 B 1 B1 2 B2 1
'

test_expect_success 'blame ancestor' '
	check_count -h master A 2 B 2
'

test_expect_success 'blame great-ancestor' '
	check_count -h master^ A 2
'

test_expect_success 'setup evil merge' '
	echo "evil merge." >>file &&
	git commit -a --amend
'

test_expect_success 'blame evil merge' '
	check_count A 2 B 1 B1 2 B2 1 "A U Thor" 1
'

test_expect_success 'setup incomplete line' '
	echo "incomplete" | tr -d "\\012" >>file &&
	GIT_AUTHOR_NAME="C" GIT_AUTHOR_EMAIL="C@test.git" \
	git commit -a -m "Incomplete"
'

test_expect_success 'blame incomplete line' '
	check_count A 2 B 1 B1 2 B2 1 "A U Thor" 1 C 1
'

test_expect_success 'setup edits' '
	mv file file.orig &&
	{
		cat file.orig &&
		echo
	} | sed -e "s/^3A/99/" -e "/^1A/d" -e "/^incomplete/d" >file &&
	echo "incomplete" | tr -d "\\012" >>file &&
	GIT_AUTHOR_NAME="D" GIT_AUTHOR_EMAIL="D@test.git" \
	git commit -a -m "edit"
'

test_expect_success 'blame edits' '
	check_count A 1 B 1 B1 1 B2 1 "A U Thor" 1 C 1 D 1
'

test_expect_success 'setup obfuscated email' '
	echo "No robots allowed" >file.new &&
	cat file >>file.new &&
	mv file.new file &&
	GIT_AUTHOR_NAME="E" GIT_AUTHOR_EMAIL="E at test dot git" \
	git commit -a -m "norobots"
'

test_expect_success 'blame obfuscated email' '
	check_count A 1 B 1 B1 1 B2 1 "A U Thor" 1 C 1 D 1 E 1
'
