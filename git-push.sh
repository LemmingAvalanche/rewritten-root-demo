#!/bin/sh

USAGE='[--all] [--force] <repository> [<refspec>...]'
. git-sh-setup

# Parse out parameters and then stop at remote, so that we can
# translate it using .git/branches information
has_all=
has_force=
has_exec=
remote=
do_tags=

while case "$#" in 0) break ;; esac
do
	case "$1" in
	--all)
		has_all=--all ;;
	--tags)
		do_tags=yes ;;
	--force)
		has_force=--force ;;
	--exec=*)
		has_exec="$1" ;;
	-*)
                usage ;;
        *)
		set x "$@"
		shift
		break ;;
	esac
	shift
done
case "$#" in
0)
	echo "Where would you want to push today?"
        usage ;;
esac
if test ",$has_all,$do_tags," = ",--all,yes,"
then
	do_tags=
fi

. git-parse-remote
remote=$(get_remote_url "$@")
case "$has_all" in
--all) set x ;;
'')    set x $(get_remote_refs_for_push "$@") ;;
esac
shift

case "$do_tags" in
yes)
	set "$@" $(cd "$GIT_DIR/refs" && find tags -type f -print) ;;
esac

# Now we have explicit refs from the command line or from remotes/
# shorthand, or --tags.  Falling back on the current branch if we still
# do not have any may be an alternative, but prevent mistakes for now.

case "$#,$has_all" in
0,)
	die "No refs given to be pushed." ;;
esac

case "$remote" in
git://*)
	die "Cannot use READ-ONLY transport to push to $remote" ;;
rsync://*)
        die "Pushing with rsync transport is deprecated" ;;
esac

set x "$remote" "$@"; shift
test "$has_all" && set x "$has_all" "$@" && shift
test "$has_force" && set x "$has_force" "$@" && shift
test "$has_exec" && set x "$has_exec" "$@" && shift

case "$remote" in
http://* | https://*)
	exec git-http-push "$@";;
*)
	exec git-send-pack "$@";;
esac
