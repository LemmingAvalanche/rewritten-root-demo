#!/bin/bash
#
# git-subtree.sh: split/join git repositories in subdirectories of this one
#
# Copyright (C) 2009 Avery Pennarun <apenwarr@gmail.com>
#
if [ $# -eq 0 ]; then
    set -- -h
fi
OPTS_SPEC="\
git subtree add   --prefix=<prefix> <commit>
git subtree merge --prefix=<prefix> <commit>
git subtree pull  --prefix=<prefix> <repository> <refspec...>
git subtree split --prefix=<prefix> <commit...>
--
h,help        show the help
q             quiet
d             show debug messages
prefix=       the name of the subdir to split out
 options for 'split'
annotate=     add a prefix to commit message of new commits
b,branch=     create a new branch from the split subtree
ignore-joins  ignore prior --rejoin commits
onto=         try connecting new tree to an existing one
rejoin        merge the new branch back into HEAD
 options for 'add', 'merge', and 'pull'
squash        merge subtree changes as a single commit
"
eval $(echo "$OPTS_SPEC" | git rev-parse --parseopt -- "$@" || echo exit $?)
PATH=$(git --exec-path):$PATH
. git-sh-setup
require_work_tree

quiet=
branch=
debug=
command=
onto=
rejoin=
ignore_joins=
annotate=
squash=

debug()
{
	if [ -n "$debug" ]; then
		echo "$@" >&2
	fi
}

say()
{
	if [ -z "$quiet" ]; then
		echo "$@" >&2
	fi
}

assert()
{
	if "$@"; then
		:
	else
		die "assertion failed: " "$@"
	fi
}


#echo "Options: $*"

while [ $# -gt 0 ]; do
	opt="$1"
	shift
	case "$opt" in
		-q) quiet=1 ;;
		-d) debug=1 ;;
		--annotate) annotate="$1"; shift ;;
		--no-annotate) annotate= ;;
		-b) branch="$1"; shift ;;
		--prefix) prefix="$1"; shift ;;
		--no-prefix) prefix= ;;
		--onto) onto="$1"; shift ;;
		--no-onto) onto= ;;
		--rejoin) rejoin=1 ;;
		--no-rejoin) rejoin= ;;
		--ignore-joins) ignore_joins=1 ;;
		--no-ignore-joins) ignore_joins= ;;
		--squash) squash=1 ;;
		--no-squash) squash= ;;
		--) break ;;
		*) die "Unexpected option: $opt" ;;
	esac
done

command="$1"
shift
case "$command" in
	add|merge|pull) default= ;;
	split) default="--default HEAD" ;;
	*) die "Unknown command '$command'" ;;
esac

if [ -z "$prefix" ]; then
	die "You must provide the --prefix option."
fi
dir="$prefix"

if [ "$command" != "pull" ]; then
	revs=$(git rev-parse $default --revs-only "$@") || exit $?
	dirs="$(git rev-parse --no-revs --no-flags "$@")" || exit $?
	if [ -n "$dirs" ]; then
		die "Error: Use --prefix instead of bare filenames."
	fi
fi

debug "command: {$command}"
debug "quiet: {$quiet}"
debug "revs: {$revs}"
debug "dir: {$dir}"
debug "opts: {$*}"
debug

cache_setup()
{
	cachedir="$GIT_DIR/subtree-cache/$$"
	rm -rf "$cachedir" || die "Can't delete old cachedir: $cachedir"
	mkdir -p "$cachedir" || die "Can't create new cachedir: $cachedir"
	debug "Using cachedir: $cachedir" >&2
}

cache_get()
{
	for oldrev in $*; do
		if [ -r "$cachedir/$oldrev" ]; then
			read newrev <"$cachedir/$oldrev"
			echo $newrev
		fi
	done
}

cache_set()
{
	oldrev="$1"
	newrev="$2"
	if [ "$oldrev" != "latest_old" \
	     -a "$oldrev" != "latest_new" \
	     -a -e "$cachedir/$oldrev" ]; then
		die "cache for $oldrev already exists!"
	fi
	echo "$newrev" >"$cachedir/$oldrev"
}

rev_exists()
{
	if git rev-parse "$1" >/dev/null 2>&1; then
		return 0
	else
		return 1
	fi
}

# if a commit doesn't have a parent, this might not work.  But we only want
# to remove the parent from the rev-list, and since it doesn't exist, it won't
# be there anyway, so do nothing in that case.
try_remove_previous()
{
	if rev_exists "$1^"; then
		echo "^$1^"
	fi
}

find_latest_squash()
{
	debug "Looking for latest squash ($dir)..."
	dir="$1"
	sq=
	main=
	sub=
	git log --grep="^git-subtree-dir: $dir\$" \
		--pretty=format:'START %H%n%s%n%n%b%nEND%n' HEAD |
	while read a b junk; do
		debug "$a $b $junk"
		debug "{{$sq/$main/$sub}}"
		case "$a" in
			START) sq="$b" ;;
			git-subtree-mainline:) main="$b" ;;
			git-subtree-split:) sub="$b" ;;
			END)
				if [ -n "$sub" ]; then
					if [ -n "$main" ]; then
						# a rejoin commit?
						# Pretend its sub was a squash.
						sq="$sub"
					fi
					debug "Squash found: $sq $sub"
					echo "$sq" "$sub"
					break
				fi
				sq=
				main=
				sub=
				;;
		esac
	done
}

find_existing_splits()
{
	debug "Looking for prior splits..."
	dir="$1"
	revs="$2"
	main=
	sub=
	git log --grep="^git-subtree-dir: $dir\$" \
		--pretty=format:'START %H%n%s%n%n%b%nEND%n' $revs |
	while read a b junk; do
		case "$a" in
			START) main="$b"; sq="$b" ;;
			git-subtree-mainline:) main="$b" ;;
			git-subtree-split:) sub="$b" ;;
			END)
				if [ -z "$main" -a -n "$sub" ]; then
					# squash commits refer to a subtree
					cache_set "$sq" "$sub"
				fi
				if [ -n "$main" -a -n "$sub" ]; then
					debug "  Prior: $main -> $sub"
					cache_set $main $sub
					try_remove_previous "$main"
					try_remove_previous "$sub"
				fi
				main=
				sub=
				;;
		esac
	done
}

copy_commit()
{
	# We're going to set some environment vars here, so
	# do it in a subshell to get rid of them safely later
	debug copy_commit "{$1}" "{$2}" "{$3}"
	git log -1 --pretty=format:'%an%n%ae%n%ad%n%cn%n%ce%n%cd%n%s%n%n%b' "$1" |
	(
		read GIT_AUTHOR_NAME
		read GIT_AUTHOR_EMAIL
		read GIT_AUTHOR_DATE
		read GIT_COMMITTER_NAME
		read GIT_COMMITTER_EMAIL
		read GIT_COMMITTER_DATE
		export  GIT_AUTHOR_NAME \
			GIT_AUTHOR_EMAIL \
			GIT_AUTHOR_DATE \
			GIT_COMMITTER_NAME \
			GIT_COMMITTER_EMAIL \
			GIT_COMMITTER_DATE
		(echo -n "$annotate"; cat ) |
		git commit-tree "$2" $3  # reads the rest of stdin
	) || die "Can't copy commit $1"
}

add_msg()
{
	dir="$1"
	latest_old="$2"
	latest_new="$3"
	cat <<-EOF
		Add '$dir/' from commit '$latest_new'
		
		git-subtree-dir: $dir
		git-subtree-mainline: $latest_old
		git-subtree-split: $latest_new
	EOF
}

rejoin_msg()
{
	dir="$1"
	latest_old="$2"
	latest_new="$3"
	cat <<-EOF
		Split '$dir/' into commit '$latest_new'
		
		git-subtree-dir: $dir
		git-subtree-mainline: $latest_old
		git-subtree-split: $latest_new
	EOF
}

squash_msg()
{
	dir="$1"
	oldsub="$2"
	newsub="$3"
	newsub_short=$(git rev-parse --short "$newsub")
	
	if [ -n "$oldsub" ]; then
		oldsub_short=$(git rev-parse --short "$oldsub")
		echo "Squashed '$dir/' changes from $oldsub_short..$newsub_short"
		echo
		git log --pretty=tformat:'%h %s' "$oldsub..$newsub"
		git log --pretty=tformat:'REVERT: %h %s' "$newsub..$oldsub"
	else
		echo "Squashed '$dir/' content from commit $newsub_short"
	fi
	
	echo
	echo "git-subtree-dir: $dir"
	echo "git-subtree-split: $newsub"
}

toptree_for_commit()
{
	commit="$1"
	git log -1 --pretty=format:'%T' "$commit" -- || exit $?
}

subtree_for_commit()
{
	commit="$1"
	dir="$2"
	git ls-tree "$commit" -- "$dir" |
	while read mode type tree name; do
		assert [ "$name" = "$dir" ]
		echo $tree
		break
	done
}

tree_changed()
{
	tree=$1
	shift
	if [ $# -ne 1 ]; then
		return 0   # weird parents, consider it changed
	else
		ptree=$(toptree_for_commit $1)
		if [ "$ptree" != "$tree" ]; then
			return 0   # changed
		else
			return 1   # not changed
		fi
	fi
}

new_squash_commit()
{
	old="$1"
	oldsub="$2"
	newsub="$3"
	tree=$(toptree_for_commit $newsub) || exit $?
	if [ -n "$old" ]; then
		squash_msg "$dir" "$oldsub" "$newsub" | 
			git commit-tree "$tree" -p "$old" || exit $?
	else
		squash_msg "$dir" "" "$newsub" |
			git commit-tree "$tree" || exit $?
	fi
}

copy_or_skip()
{
	rev="$1"
	tree="$2"
	newparents="$3"
	assert [ -n "$tree" ]

	identical=
	nonidentical=
	p=
	gotparents=
	for parent in $newparents; do
		ptree=$(toptree_for_commit $parent) || exit $?
		[ -z "$ptree" ] && continue
		if [ "$ptree" = "$tree" ]; then
			# an identical parent could be used in place of this rev.
			identical="$parent"
		else
			nonidentical="$parent"
		fi
		
		# sometimes both old parents map to the same newparent;
		# eliminate duplicates
		is_new=1
		for gp in $gotparents; do
			if [ "$gp" = "$parent" ]; then
				is_new=
				break
			fi
		done
		if [ -n "$is_new" ]; then
			gotparents="$gotparents $parent"
			p="$p -p $parent"
		fi
	done
	
	if [ -n "$identical" ]; then
		echo $identical
	else
		copy_commit $rev $tree "$p" || exit $?
	fi
}

ensure_clean()
{
	if ! git diff-index HEAD --exit-code --quiet; then
		die "Working tree has modifications.  Cannot add."
	fi
	if ! git diff-index --cached HEAD --exit-code --quiet; then
		die "Index has modifications.  Cannot add."
	fi
}

cmd_add()
{
	if [ -e "$dir" ]; then
		die "'$dir' already exists.  Cannot add."
	fi
	ensure_clean
	
	set -- $revs
	if [ $# -ne 1 ]; then
		die "You must provide exactly one revision.  Got: '$revs'"
	fi
	rev="$1"
	
	debug "Adding $dir as '$rev'..."
	git read-tree --prefix="$dir" $rev || exit $?
	git checkout "$dir" || exit $?
	tree=$(git write-tree) || exit $?
	
	headrev=$(git rev-parse HEAD) || exit $?
	if [ -n "$headrev" -a "$headrev" != "$rev" ]; then
		headp="-p $headrev"
	else
		headp=
	fi
	
	if [ -n "$squash" ]; then
		rev=$(new_squash_commit "" "" "$rev") || exit $?
		commit=$(echo "Merge commit '$rev' as '$dir'" |
			 git commit-tree $tree $headp -p "$rev") || exit $?
	else
		commit=$(add_msg "$dir" "$headrev" "$rev" |
			 git commit-tree $tree $headp -p "$rev") || exit $?
	fi
	git reset "$commit" || exit $?
	
	say "Added dir '$dir'"
}

cmd_split()
{
	if [ -n "$branch" ] && rev_exists "refs/heads/$branch"; then
		die "Branch '$branch' already exists."
	fi

	debug "Splitting $dir..."
	cache_setup || exit $?
	
	if [ -n "$onto" ]; then
		debug "Reading history for --onto=$onto..."
		git rev-list $onto |
		while read rev; do
			# the 'onto' history is already just the subdir, so
			# any parent we find there can be used verbatim
			debug "  cache: $rev"
			cache_set $rev $rev
		done
	fi
	
	if [ -n "$ignore_joins" ]; then
		unrevs=
	else
		unrevs="$(find_existing_splits "$dir" "$revs")"
	fi
	
	# We can't restrict rev-list to only $dir here, because some of our
	# parents have the $dir contents the root, and those won't match.
	# (and rev-list --follow doesn't seem to solve this)
	grl='git rev-list --reverse --parents $revs $unrevs'
	revmax=$(eval "$grl" | wc -l)
	revcount=0
	createcount=0
	eval "$grl" |
	while read rev parents; do
		revcount=$(($revcount + 1))
		say -n "$revcount/$revmax ($createcount)"
		debug "Processing commit: $rev"
		exists=$(cache_get $rev)
		if [ -n "$exists" ]; then
			debug "  prior: $exists"
			continue
		fi
		createcount=$(($createcount + 1))
		debug "  parents: $parents"
		newparents=$(cache_get $parents)
		debug "  newparents: $newparents"
		
		tree=$(subtree_for_commit $rev "$dir")
		debug "  tree is: $tree"
		
		# ugly.  is there no better way to tell if this is a subtree
		# vs. a mainline commit?  Does it matter?
		[ -z $tree ] && continue

		newrev=$(copy_or_skip "$rev" "$tree" "$newparents") || exit $?
		debug "  newrev is: $newrev"
		cache_set $rev $newrev
		cache_set latest_new $newrev
		cache_set latest_old $rev
	done || exit $?
	latest_new=$(cache_get latest_new)
	if [ -z "$latest_new" ]; then
		die "No new revisions were found"
	fi
	
	if [ -n "$rejoin" ]; then
		debug "Merging split branch into HEAD..."
		latest_old=$(cache_get latest_old)
		git merge -s ours \
			-m "$(rejoin_msg $dir $latest_old $latest_new)" \
			$latest_new >&2 || exit $?
	fi
	if [ -n "$branch" ]; then
		git update-ref -m 'subtree split' "refs/heads/$branch" \
			$latest_new "" || exit $?
		say "Created branch '$branch'"
	fi
	echo $latest_new
	exit 0
}

cmd_merge()
{
	ensure_clean
	
	set -- $revs
	if [ $# -ne 1 ]; then
		die "You must provide exactly one revision.  Got: '$revs'"
	fi
	rev="$1"
	
	if [ -n "$squash" ]; then
		first_split="$(find_latest_squash "$dir")"
		if [ -z "$first_split" ]; then
			die "Can't squash-merge: '$dir' was never added."
		fi
		set $first_split
		old=$1
		sub=$2
		if [ "$sub" = "$rev" ]; then
			say "Subtree is already at commit $rev."
			exit 0
		fi
		new=$(new_squash_commit "$old" "$sub" "$rev") || exit $?
		debug "New squash commit: $new"
		rev="$new"
	fi
	
	git merge -s subtree $rev
}

cmd_pull()
{
	ensure_clean
	set -x
	git pull -s subtree "$@"
}

"cmd_$command" "$@"
