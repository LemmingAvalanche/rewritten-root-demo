# This shell script fragment is sourced by git-rebase to implement
# its interactive mode.  "git rebase --interactive" makes it easy
# to fix up commits in the middle of a series and rearrange commits.
#
# Copyright (c) 2006 Johannes E. Schindelin
#
# The original idea comes from Eric W. Biederman, in
# https://public-inbox.org/git/m1odwkyuf5.fsf_-_@ebiederm.dsl.xmission.com/
#
# The file containing rebase commands, comments, and empty lines.
# This file is created by "git rebase -i" then edited by the user.  As
# the lines are processed, they are removed from the front of this
# file and written to the tail of $done.
todo="$state_dir"/git-rebase-todo

GIT_CHERRY_PICK_HELP="$resolvemsg"
export GIT_CHERRY_PICK_HELP

comment_char=$(git config --get core.commentchar 2>/dev/null)
case "$comment_char" in
'' | auto)
	comment_char="#"
	;;
?)
	;;
*)
	comment_char=$(echo "$comment_char" | cut -c1)
	;;
esac

orig_reflog_action="$GIT_REFLOG_ACTION"

comment_for_reflog () {
	case "$orig_reflog_action" in
	''|rebase*)
		GIT_REFLOG_ACTION="rebase -i ($1)"
		export GIT_REFLOG_ACTION
		;;
	esac
}

die_abort () {
	apply_autostash
	rm -rf "$state_dir"
	die "$1"
}

has_action () {
	test -n "$(git stripspace --strip-comments <"$1")"
}

git_sequence_editor () {
	if test -z "$GIT_SEQUENCE_EDITOR"
	then
		GIT_SEQUENCE_EDITOR="$(git config sequence.editor)"
		if [ -z "$GIT_SEQUENCE_EDITOR" ]
		then
			GIT_SEQUENCE_EDITOR="$(git var GIT_EDITOR)" || return $?
		fi
	fi

	eval "$GIT_SEQUENCE_EDITOR" '"$@"'
}

expand_todo_ids() {
	git rebase--helper --expand-ids
}

collapse_todo_ids() {
	git rebase--helper --shorten-ids
}

# Switch to the branch in $into and notify it in the reflog
checkout_onto () {
	comment_for_reflog start
	GIT_REFLOG_ACTION="$GIT_REFLOG_ACTION: checkout $onto_name"
	output git checkout $onto || die_abort "$(gettext "could not detach HEAD")"
	git update-ref ORIG_HEAD $orig_head
}

get_missing_commit_check_level () {
	check_level=$(git config --get rebase.missingCommitsCheck)
	check_level=${check_level:-ignore}
	# Don't be case sensitive
	printf '%s' "$check_level" | tr 'A-Z' 'a-z'
}

# Initiate an action. If the cannot be any
# further action it  may exec a command
# or exit and not return.
#
# TODO: Consider a cleaner return model so it
# never exits and always return 0 if process
# is complete.
#
# Parameter 1 is the action to initiate.
#
# Returns 0 if the action was able to complete
# and if 1 if further processing is required.
initiate_action () {
	case "$1" in
	continue)
		exec git rebase--helper ${force_rebase:+--no-ff} $allow_empty_message \
		     --continue
		;;
	skip)
		git rerere clear
		exec git rebase--helper ${force_rebase:+--no-ff} $allow_empty_message \
		     --continue
		;;
	edit-todo)
		exec git rebase--helper --edit-todo
		;;
	show-current-patch)
		exec git show REBASE_HEAD --
		;;
	*)
		return 1 # continue
		;;
	esac
}

init_basic_state () {
	orig_head=$(git rev-parse --verify HEAD) || die "$(gettext "No HEAD?")"
	mkdir -p "$state_dir" || die "$(eval_gettext "Could not create temporary \$state_dir")"
	rm -f "$(git rev-parse --git-path REBASE_HEAD)"

	: > "$state_dir"/interactive || die "$(gettext "Could not mark as interactive")"
	write_basic_state
}

init_revisions_and_shortrevisions () {
	shorthead=$(git rev-parse --short $orig_head)
	shortonto=$(git rev-parse --short $onto)
	if test -z "$rebase_root"
		# this is now equivalent to ! -z "$upstream"
	then
		shortupstream=$(git rev-parse --short $upstream)
		revisions=$upstream...$orig_head
		shortrevisions=$shortupstream..$shorthead
	else
		revisions=$onto...$orig_head
		shortrevisions=$shorthead
		test -z "$squash_onto" ||
		echo "$squash_onto" >"$state_dir"/squash-onto
	fi
}

complete_action() {
	test -s "$todo" || echo noop >> "$todo"
	test -z "$autosquash" || git rebase--helper --rearrange-squash || exit
	test -n "$cmd" && git rebase--helper --add-exec-commands "$cmd"

	todocount=$(git stripspace --strip-comments <"$todo" | wc -l)
	todocount=${todocount##* }

cat >>"$todo" <<EOF

$comment_char $(eval_ngettext \
	"Rebase \$shortrevisions onto \$shortonto (\$todocount command)" \
	"Rebase \$shortrevisions onto \$shortonto (\$todocount commands)" \
	"$todocount")
EOF
	git rebase--helper --append-todo-help ${keep_empty:+--keep-empty}

	has_action "$todo" ||
		return 2

	cp "$todo" "$todo".backup
	collapse_todo_ids
	git_sequence_editor "$todo" ||
		die_abort "$(gettext "Could not execute editor")"

	has_action "$todo" ||
		return 2

	git rebase--helper --check-todo-list || {
		ret=$?
		checkout_onto
		exit $ret
	}

	expand_todo_ids

	test -n "$force_rebase" ||
	onto="$(git rebase--helper --skip-unnecessary-picks)" ||
	die "Could not skip unnecessary pick commands"

	checkout_onto
	require_clean_work_tree "rebase"
	exec git rebase--helper ${force_rebase:+--no-ff} $allow_empty_message \
	     --continue
}

git_rebase__interactive () {
	initiate_action "$action"
	ret=$?
	if test $ret = 0; then
		return 0
	fi

	git rebase--helper --prepare-branch "$switch_to" ${verbose:+--verbose}
	init_basic_state

	init_revisions_and_shortrevisions

	git rebase--helper --make-script ${keep_empty:+--keep-empty} \
		${rebase_merges:+--rebase-merges} \
		${rebase_cousins:+--rebase-cousins} \
		$revisions ${restrict_revision+^$restrict_revision} >"$todo" ||
	die "$(gettext "Could not generate todo list")"

	complete_action
}
