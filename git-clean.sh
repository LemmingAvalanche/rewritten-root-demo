#!/bin/sh
#
# Copyright (c) 2005-2006 Pavel Roskin
#

USAGE="[-d] [-n] [-q] [-x | -X]"
LONG_USAGE='Clean untracked files from the working directory
	-d	remove directories as well
	-n 	don'\''t remove anything, just show what would be done
	-q	be quiet, only report errors
	-x	remove ignored files as well
	-X	remove only ignored files as well'
SUBDIRECTORY_OK=Yes
. git-sh-setup

ignored=
ignoredonly=
cleandir=
quiet=
rmf="rm -f"
rmrf="rm -rf"
rm_refuse="echo Not removing"
echo1="echo"

while case "$#" in 0) break ;; esac
do
	case "$1" in
	-d)
		cleandir=1
		;;
	-n)
		quiet=1
		rmf="echo Would remove"
		rmrf="echo Would remove"
		rm_refuse="echo Would not remove"
		echo1=":"
		;;
	-q)
		quiet=1
		;;
	-x)
		ignored=1
		;;
	-X)
		ignoredonly=1
		;;
	*)
		usage
	esac
	shift
done

case "$ignored,$ignoredonly" in
	1,1) usage;;
esac

if [ -z "$ignored" ]; then
	excl="--exclude-per-directory=.gitignore"
	if [ -f "$GIT_DIR/info/exclude" ]; then
		excl_info="--exclude-from=$GIT_DIR/info/exclude"
	fi
	if [ "$ignoredonly" ]; then
		excl="$excl --ignored"
	fi
fi

git-ls-files --others --directory $excl ${excl_info:+"$excl_info"} |
while read -r file; do
	if [ -d "$file" -a ! -L "$file" ]; then
		if [ -z "$cleandir" ]; then
			$rm_refuse "$file"
			continue
		fi
		$echo1 "Removing $file"
		$rmrf "$file"
	else
		$echo1 "Removing $file"
		$rmf "$file"
	fi
done
