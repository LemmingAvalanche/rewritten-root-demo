#!/bin/sh
#
# Copyright (c) 2010 Junio C Hamano.
#

. git-sh-setup

prec=4

read_state () {
	onto_name=$(cat "$state_dir"/onto_name) &&
	end=$(cat "$state_dir"/end) &&
	msgnum=$(cat "$state_dir"/msgnum)
}

continue_merge () {
	test -d "$state_dir" || die "$state_dir directory does not exist"

	unmerged=$(git ls-files -u)
	if test -n "$unmerged"
	then
		echo "You still have unmerged paths in your index"
		echo "did you forget to use git add?"
		die "$resolvemsg"
	fi

	cmt=`cat "$state_dir/current"`
	if ! git diff-index --quiet --ignore-submodules HEAD --
	then
		if ! git commit --no-verify -C "$cmt"
		then
			echo "Commit failed, please do not call \"git commit\""
			echo "directly, but instead do one of the following: "
			die "$resolvemsg"
		fi
		if test -z "$GIT_QUIET"
		then
			printf "Committed: %0${prec}d " $msgnum
		fi
		echo "$cmt $(git rev-parse HEAD^0)" >> "$state_dir/rewritten"
	else
		if test -z "$GIT_QUIET"
		then
			printf "Already applied: %0${prec}d " $msgnum
		fi
	fi
	test -z "$GIT_QUIET" &&
	GIT_PAGER='' git log --format=%s -1 "$cmt"

	# onto the next patch:
	msgnum=$(($msgnum + 1))
	echo "$msgnum" >"$state_dir/msgnum"
}

call_merge () {
	cmt="$(cat "$state_dir/cmt.$1")"
	echo "$cmt" > "$state_dir/current"
	hd=$(git rev-parse --verify HEAD)
	cmt_name=$(git symbolic-ref HEAD 2> /dev/null || echo HEAD)
	msgnum=$(cat "$state_dir/msgnum")
	eval GITHEAD_$cmt='"${cmt_name##refs/heads/}~$(($end - $msgnum))"'
	eval GITHEAD_$hd='$onto_name'
	export GITHEAD_$cmt GITHEAD_$hd
	if test -n "$GIT_QUIET"
	then
		GIT_MERGE_VERBOSITY=1 && export GIT_MERGE_VERBOSITY
	fi
	test -z "$strategy" && strategy=recursive
	eval 'git-merge-$strategy' $strategy_opts '"$cmt^" -- "$hd" "$cmt"'
	rv=$?
	case "$rv" in
	0)
		unset GITHEAD_$cmt GITHEAD_$hd
		return
		;;
	1)
		git rerere $allow_rerere_autoupdate
		die "$resolvemsg"
		;;
	2)
		echo "Strategy: $rv $strategy failed, try another" 1>&2
		die "$resolvemsg"
		;;
	*)
		die "Unknown exit code ($rv) from command:" \
			"git-merge-$strategy $cmt^ -- HEAD $cmt"
		;;
	esac
}

finish_rb_merge () {
	move_to_original_branch
	git notes copy --for-rewrite=rebase < "$state_dir"/rewritten
	if test -x "$GIT_DIR"/hooks/post-rewrite &&
		test -s "$state_dir"/rewritten; then
		"$GIT_DIR"/hooks/post-rewrite rebase < "$state_dir"/rewritten
	fi
	rm -r "$state_dir"
	say All done.
}

case "$action" in
continue)
	read_state
	continue_merge
	while test "$msgnum" -le "$end"
	do
		call_merge "$msgnum"
		continue_merge
	done
	finish_rb_merge
	exit
	;;
skip)
	read_state
	git rerere clear
	msgnum=$(($msgnum + 1))
	while test "$msgnum" -le "$end"
	do
		call_merge "$msgnum"
		continue_merge
	done
	finish_rb_merge
	exit
	;;
esac

mkdir -p "$state_dir"
echo "$onto_name" > "$state_dir/onto_name"
echo "$head_name" > "$state_dir/head-name"
echo "$onto" > "$state_dir/onto"
echo "$orig_head" > "$state_dir/orig-head"
echo "$GIT_QUIET" > "$state_dir/quiet"

msgnum=0
for cmt in `git rev-list --reverse --no-merges "$revisions"`
do
	msgnum=$(($msgnum + 1))
	echo "$cmt" > "$state_dir/cmt.$msgnum"
done

echo 1 >"$state_dir/msgnum"
echo $msgnum >"$state_dir/end"

end=$msgnum
msgnum=1

while test "$msgnum" -le "$end"
do
	call_merge "$msgnum"
	continue_merge
done

finish_rb_merge
