#!/bin/sh

test_description='git-p4 client view'

. ./lib-git-p4.sh

test_expect_success 'start p4d' '
	start_p4d
'

#
# Construct a client with this list of View lines
#
client_view() {
	(
		cat <<-EOF &&
		Client: client
		Description: client
		Root: $cli
		View:
		EOF
		for arg ; do
			printf "\t$arg\n"
		done
	) | p4 client -i
}

#
# Verify these files exist, exactly.  Caller creates
# a list of files in file "files".
#
check_files_exist() {
	ok=0 &&
	num=${#@} &&
	for arg ; do
		test_path_is_file "$arg" &&
		ok=$(($ok + 1))
	done &&
	test $ok -eq $num &&
	test_line_count = $num files
}

#
# Sync up the p4 client, make sure the given files (and only
# those) exist.
#
client_verify() {
	(
		cd "$cli" &&
		p4 sync &&
		find . -type f ! -name files >files &&
		check_files_exist "$@"
	)
}

#
# Make sure the named files, exactly, exist.
#
git_verify() {
	(
		cd "$git" &&
		git ls-files >files &&
		check_files_exist "$@"
	)
}

# //depot
#   - dir1
#     - file11
#     - file12
#   - dir2
#     - file21
#     - file22
test_expect_success 'init depot' '
	(
		cd "$cli" &&
		for d in 1 2 ; do
			mkdir -p dir$d &&
			for f in 1 2 ; do
				echo dir$d/file$d$f >dir$d/file$d$f &&
				p4 add dir$d/file$d$f &&
				p4 submit -d "dir$d/file$d$f"
			done
		done &&
		find . -type f ! -name files >files &&
		check_files_exist dir1/file11 dir1/file12 \
				  dir2/file21 dir2/file22
	)
'

# double % for printf
test_expect_success 'unsupported view wildcard %%n' '
	client_view "//depot/%%%%1/sub/... //client/sub/%%%%1/..." &&
	test_when_finished cleanup_git &&
	test_must_fail "$GITP4" clone --use-client-spec --dest="$git" //depot
'

test_expect_success 'unsupported view wildcard *' '
	client_view "//depot/*/bar/... //client/*/bar/..." &&
	test_when_finished cleanup_git &&
	test_must_fail "$GITP4" clone --use-client-spec --dest="$git" //depot
'

test_expect_success 'wildcard ... only supported at end of spec' '
	client_view "//depot/.../file11 //client/.../file11" &&
	test_when_finished cleanup_git &&
	test_must_fail "$GITP4" clone --use-client-spec --dest="$git" //depot
'

test_expect_failure 'basic map' '
	client_view "//depot/dir1/... //client/cli1/..." &&
	files="cli1/file11 cli1/file12" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_failure 'client view with no mappings' '
	client_view &&
	client_verify &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify
'

test_expect_failure 'single file map' '
	client_view "//depot/dir1/file11 //client/file11" &&
	files="file11" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_success 'later mapping takes precedence (entire repo)' '
	client_view "//depot/dir1/... //client/cli1/..." \
		    "//depot/... //client/cli2/..." &&
	files="cli2/dir1/file11 cli2/dir1/file12
	       cli2/dir2/file21 cli2/dir2/file22" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_failure 'later mapping takes precedence (partial repo)' '
	client_view "//depot/dir1/... //client/..." \
		    "//depot/dir2/... //client/..." &&
	files="file21 file22" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

# Reading the view backwards,
#   dir2 goes to cli12
#   dir1 cannot go to cli12 since it was filled by dir2
#   dir1 also does not go to cli3, since the second rule
#     noticed that it matched, but was already filled
test_expect_failure 'depot path matching rejected client path' '
	client_view "//depot/dir1/... //client/cli3/..." \
		    "//depot/dir1/... //client/cli12/..." \
		    "//depot/dir2/... //client/cli12/..." &&
	files="cli12/file21 cli12/file22" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

# since both have the same //client/..., the exclusion
# rule keeps everything out
test_expect_failure 'exclusion wildcard, client rhs same (odd)' '
	client_view "//depot/... //client/..." \
		    "-//depot/dir2/... //client/..." &&
	client_verify &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify
'

test_expect_success 'exclusion wildcard, client rhs different (normal)' '
	client_view "//depot/... //client/..." \
		    "-//depot/dir2/... //client/dir2/..." &&
	files="dir1/file11 dir1/file12" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_failure 'exclusion single file' '
	client_view "//depot/... //client/..." \
		    "-//depot/dir2/file22 //client/file22" &&
	files="dir1/file11 dir1/file12 dir2/file21" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_failure 'overlay wildcard' '
	client_view "//depot/dir1/... //client/cli/..." \
		    "+//depot/dir2/... //client/cli/...\n" &&
	files="cli/file11 cli/file12 cli/file21 cli/file22" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_failure 'overlay single file' '
	client_view "//depot/dir1/... //client/cli/..." \
		    "+//depot/dir2/file21 //client/cli/file21" &&
	files="cli/file11 cli/file12 cli/file21" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_failure 'exclusion with later inclusion' '
	client_view "//depot/... //client/..." \
		    "-//depot/dir2/... //client/dir2/..." \
		    "//depot/dir2/... //client/dir2incl/..." &&
	files="dir1/file11 dir1/file12 dir2incl/file21 dir2incl/file22" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify $files
'

test_expect_failure 'quotes on rhs only' '
	client_view "//depot/dir1/... \"//client/cdir 1/...\"" &&
	client_verify "cdir 1/file11" "cdir 1/file12" &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify "cdir 1/file11" "cdir 1/file12"
'

#
# Rename directories to test quoting in depot-side mappings
# //depot
#    - "dir 1"
#       - file11
#       - file12
#    - "dir 2"
#       - file21
#       - file22
#
test_expect_success 'rename files to introduce spaces' '
	client_view "//depot/... //client/..." &&
	client_verify dir1/file11 dir1/file12 \
		      dir2/file21 dir2/file22 &&
	(
		cd "$cli" &&
		p4 open dir1/... &&
		p4 move dir1/... "dir 1"/... &&
		p4 open dir2/... &&
		p4 move dir2/... "dir 2"/... &&
		p4 submit -d "rename with spaces"
	) &&
	client_verify "dir 1/file11" "dir 1/file12" \
		      "dir 2/file21" "dir 2/file22"
'

test_expect_failure 'quotes on lhs only' '
	client_view "\"//depot/dir 1/...\" //client/cdir1/..." &&
	files="cdir1/file11 cdir1/file12" &&
	client_verify $files &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	client_verify $files
'

test_expect_failure 'quotes on both sides' '
	client_view "\"//depot/dir 1/...\" \"//client/cdir 1/...\"" &&
	client_verify "cdir 1/file11" "cdir 1/file12" &&
	test_when_finished cleanup_git &&
	"$GITP4" clone --use-client-spec --dest="$git" //depot &&
	git_verify "cdir 1/file11" "cdir 1/file12"
'

test_expect_success 'kill p4d' '
	kill_p4d
'

test_done
