#!/bin/sh

echo "/* Automatically generated by generate-cmdlist.sh */
struct cmdname_help {
	char name[16];
	char help[80];
	unsigned char group;
};

static const char *common_cmd_groups[] = {"

grps=grps$$.tmp
match=match$$.tmp
trap "rm -f '$grps' '$match'" 0 1 2 3 15

sed -n '
	1,/^### common groups/b
	/^### command list/q
	/^#/b
	/^[ 	]*$/b
	h;s/^[^ 	][^ 	]*[ 	][ 	]*\(.*\)/	N_("\1"),/p
	g;s/^\([^ 	][^ 	]*\)[ 	].*/\1/w '$grps'
	' "$1"
printf '};\n\n'

n=0
substnum=
while read grp
do
	echo "^git-..*[ 	]$grp"
	substnum="$substnum${substnum:+;}s/[ 	]$grp/$n/"
	n=$(($n+1))
done <"$grps" >"$match"

printf 'static struct cmdname_help common_cmds[] = {\n'
grep -f "$match" "$1" |
sed 's/^git-//' |
sort |
while read cmd tags
do
	tag=$(echo "$tags" | sed "$substnum; s/[^0-9]//g")
	sed -n '
		/^NAME/,/git-'"$cmd"'/H
		${
			x
			s/.*git-'"$cmd"' - \(.*\)/	{"'"$cmd"'", N_("\1"), '$tag'},/
			p
		}' "Documentation/git-$cmd.txt"
done
echo "};"
