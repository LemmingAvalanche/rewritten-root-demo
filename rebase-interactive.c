#include "cache.h"
#include "commit.h"
#include "rebase-interactive.h"
#include "sequencer.h"
#include "strbuf.h"

int append_todo_help(unsigned edit_todo, unsigned keep_empty)
{
	struct strbuf buf = STRBUF_INIT;
	FILE *todo;
	int ret;
	const char *msg = _("\nCommands:\n"
"p, pick <commit> = use commit\n"
"r, reword <commit> = use commit, but edit the commit message\n"
"e, edit <commit> = use commit, but stop for amending\n"
"s, squash <commit> = use commit, but meld into previous commit\n"
"f, fixup <commit> = like \"squash\", but discard this commit's log message\n"
"x, exec <command> = run command (the rest of the line) using shell\n"
"d, drop <commit> = remove commit\n"
"l, label <label> = label current HEAD with a name\n"
"t, reset <label> = reset HEAD to a label\n"
"m, merge [-C <commit> | -c <commit>] <label> [# <oneline>]\n"
".       create a merge commit using the original merge commit's\n"
".       message (or the oneline, if no original merge commit was\n"
".       specified). Use -c <commit> to reword the commit message.\n"
"\n"
"These lines can be re-ordered; they are executed from top to bottom.\n");

	todo = fopen_or_warn(rebase_path_todo(), "a");
	if (!todo)
		return 1;

	strbuf_add_commented_lines(&buf, msg, strlen(msg));

	if (get_missing_commit_check_level() == MISSING_COMMIT_CHECK_ERROR)
		msg = _("\nDo not remove any line. Use 'drop' "
			 "explicitly to remove a commit.\n");
	else
		msg = _("\nIf you remove a line here "
			 "THAT COMMIT WILL BE LOST.\n");

	strbuf_add_commented_lines(&buf, msg, strlen(msg));

	if (edit_todo)
		msg = _("\nYou are editing the todo file "
			"of an ongoing interactive rebase.\n"
			"To continue rebase after editing, run:\n"
			"    git rebase --continue\n\n");
	else
		msg = _("\nHowever, if you remove everything, "
			"the rebase will be aborted.\n\n");

	strbuf_add_commented_lines(&buf, msg, strlen(msg));

	if (!keep_empty) {
		msg = _("Note that empty commits are commented out");
		strbuf_add_commented_lines(&buf, msg, strlen(msg));
	}

	ret = fputs(buf.buf, todo);
	if (ret < 0)
		error_errno(_("could not append help text to '%s'"), rebase_path_todo());

	fclose(todo);
	strbuf_release(&buf);

	return ret;
}
