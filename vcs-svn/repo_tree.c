/*
 * Licensed under a two-clause BSD-style license.
 * See LICENSE for details.
 */

#include "git-compat-util.h"
#include "strbuf.h"
#include "repo_tree.h"
#include "fast_export.h"

const char *repo_read_path(const uint32_t *path)
{
	int err;
	uint32_t dummy;
	static struct strbuf buf = STRBUF_INIT;

	strbuf_reset(&buf);
	err = fast_export_ls(REPO_MAX_PATH_DEPTH, path, &dummy, &buf);
	if (err) {
		if (errno != ENOENT)
			die_errno("BUG: unexpected fast_export_ls error");
		return NULL;
	}
	return buf.buf;
}

uint32_t repo_read_mode(const uint32_t *path)
{
	int err;
	uint32_t result;
	static struct strbuf dummy = STRBUF_INIT;

	strbuf_reset(&dummy);
	err = fast_export_ls(REPO_MAX_PATH_DEPTH, path, &result, &dummy);
	if (err) {
		if (errno != ENOENT)
			die_errno("BUG: unexpected fast_export_ls error");
		/* Treat missing paths as directories. */
		return REPO_MODE_DIR;
	}
	return result;
}

void repo_copy(uint32_t revision, const uint32_t *src, const uint32_t *dst)
{
	int err;
	uint32_t mode;
	static struct strbuf data = STRBUF_INIT;

	strbuf_reset(&data);
	err = fast_export_ls_rev(revision, REPO_MAX_PATH_DEPTH, src, &mode, &data);
	if (err) {
		if (errno != ENOENT)
			die_errno("BUG: unexpected fast_export_ls_rev error");
		fast_export_delete(REPO_MAX_PATH_DEPTH, dst);
		return;
	}
	fast_export_modify(REPO_MAX_PATH_DEPTH, dst, mode, data.buf);
}

void repo_delete(uint32_t *path)
{
	fast_export_delete(REPO_MAX_PATH_DEPTH, path);
}
