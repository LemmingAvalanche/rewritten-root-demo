#!/bin/sh

USAGE='[help|start|bad|good|new|old|terms|skip|next|reset|visualize|view|replay|log|run]'
LONG_USAGE='git bisect help
	print this long help message.
git bisect start [--term-{new,bad}=<term> --term-{old,good}=<term>]
		 [--no-checkout] [--first-parent] [<bad> [<good>...]] [--] [<pathspec>...]
	reset bisect state and start bisection.
git bisect (bad|new) [<rev>]
	mark <rev> a known-bad revision/
		a revision after change in a given property.
git bisect (good|old) [<rev>...]
	mark <rev>... known-good revisions/
		revisions before change in a given property.
git bisect terms [--term-good | --term-bad]
	show the terms used for old and new commits (default: bad, good)
git bisect skip [(<rev>|<range>)...]
	mark <rev>... untestable revisions.
git bisect next
	find next bisection to test and check it out.
git bisect reset [<commit>]
	finish bisection search and go back to commit.
git bisect (visualize|view)
	show bisect status in gitk.
git bisect replay <logfile>
	replay bisection log.
git bisect log
	show bisect log.
git bisect run <cmd>...
	use <cmd>... to automatically bisect.

Please use "git help bisect" to get the full man page.'

OPTIONS_SPEC=
. git-sh-setup

_x40='[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]'
_x40="$_x40$_x40$_x40$_x40$_x40$_x40$_x40$_x40"
TERM_BAD=bad
TERM_GOOD=good

bisect_skip() {
	all=''
	for arg in "$@"
	do
		case "$arg" in
		*..*)
			revs=$(git rev-list "$arg") || die "$(eval_gettext "Bad rev input: \$arg")" ;;
		*)
			revs=$(git rev-parse --sq-quote "$arg") ;;
		esac
		all="$all $revs"
	done
	eval git bisect--helper --bisect-state 'skip' $all
}

bisect_visualize() {
	git bisect--helper --bisect-next-check $TERM_GOOD $TERM_BAD fail || exit

	if test $# = 0
	then
		if test -n "${DISPLAY+set}${SESSIONNAME+set}${MSYSTEM+set}${SECURITYSESSIONID+set}" &&
			type gitk >/dev/null 2>&1
		then
			set gitk
		else
			set git log
		fi
	else
		case "$1" in
		git*|tig) ;;
		-*)	set git log "$@" ;;
		*)	set git "$@" ;;
		esac
	fi

	eval '"$@"' --bisect -- $(cat "$GIT_DIR/BISECT_NAMES")
}

bisect_replay () {
	file="$1"
	test "$#" -eq 1 || die "$(gettext "No logfile given")"
	test -r "$file" || die "$(eval_gettext "cannot read \$file for replaying")"
	git bisect--helper --bisect-reset || exit
	oIFS="$IFS" IFS="$IFS$(printf '\015')"
	while read git bisect command rev tail
	do
		test "$git $bisect" = "git bisect" || test "$git" = "git-bisect" || continue
		if test "$git" = "git-bisect"
		then
			rev="$command"
			command="$bisect"
		fi
		get_terms
		git bisect--helper --check-and-set-terms "$command" "$TERM_GOOD" "$TERM_BAD" || exit
		get_terms
		case "$command" in
		start)
			eval "git bisect--helper --bisect-start $rev $tail" ;;
		"$TERM_GOOD"|"$TERM_BAD"|skip)
			git bisect--helper --bisect-write "$command" "$rev" "$TERM_GOOD" "$TERM_BAD" || exit;;
		terms)
			git bisect--helper --bisect-terms $rev || exit;;
		*)
			die "$(gettext "?? what are you talking about?")" ;;
		esac
	done <"$file"
	IFS="$oIFS"
	git bisect--helper --bisect-auto-next || exit
}

bisect_run () {
	git bisect--helper --bisect-next-check $TERM_GOOD $TERM_BAD fail || exit

	test -n "$*" || die "$(gettext "bisect run failed: no command provided.")"

	while true
	do
		command="$@"
		eval_gettextln "running \$command"
		"$@"
		res=$?

		# Check for really bad run error.
		if [ $res -lt 0 -o $res -ge 128 ]
		then
			eval_gettextln "bisect run failed:
exit code \$res from '\$command' is < 0 or >= 128" >&2
			exit $res
		fi

		# Find current state depending on run success or failure.
		# A special exit code of 125 means cannot test.
		if [ $res -eq 125 ]
		then
			state='skip'
		elif [ $res -gt 0 ]
		then
			state="$TERM_BAD"
		else
			state="$TERM_GOOD"
		fi

		git bisect--helper --bisect-state $state >"$GIT_DIR/BISECT_RUN"
		res=$?

		cat "$GIT_DIR/BISECT_RUN"

		if sane_grep "first $TERM_BAD commit could be any of" "$GIT_DIR/BISECT_RUN" \
			>/dev/null
		then
			gettextln "bisect run cannot continue any more" >&2
			exit $res
		fi

		if [ $res -ne 0 ]
		then
			eval_gettextln "bisect run failed:
'bisect-state \$state' exited with error code \$res" >&2
			exit $res
		fi

		if sane_grep "is the first $TERM_BAD commit" "$GIT_DIR/BISECT_RUN" >/dev/null
		then
			gettextln "bisect run success"
			exit 0;
		fi

	done
}

bisect_log () {
	test -s "$GIT_DIR/BISECT_LOG" || die "$(gettext "We are not bisecting.")"
	cat "$GIT_DIR/BISECT_LOG"
}

get_terms () {
	if test -s "$GIT_DIR/BISECT_TERMS"
	then
		{
		read TERM_BAD
		read TERM_GOOD
		} <"$GIT_DIR/BISECT_TERMS"
	fi
}

case "$#" in
0)
	usage ;;
*)
	cmd="$1"
	get_terms
	shift
	case "$cmd" in
	help)
		git bisect -h ;;
	start)
		git bisect--helper --bisect-start "$@" ;;
	bad|good|new|old|"$TERM_BAD"|"$TERM_GOOD")
		git bisect--helper --bisect-state "$cmd" "$@" ;;
	skip)
		bisect_skip "$@" ;;
	next)
		# Not sure we want "next" at the UI level anymore.
		git bisect--helper --bisect-next "$@" || exit ;;
	visualize|view)
		bisect_visualize "$@" ;;
	reset)
		git bisect--helper --bisect-reset "$@" ;;
	replay)
		bisect_replay "$@" ;;
	log)
		bisect_log ;;
	run)
		bisect_run "$@" ;;
	terms)
		git bisect--helper --bisect-terms "$@" || exit;;
	*)
		usage ;;
	esac
esac
