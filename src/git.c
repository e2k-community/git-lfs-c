/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "git.h"
#include "run.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *git_dir(void)
{
	static int done;
	static char *dir;
	struct strbuf sb = STRBUF_INIT;

	if (done)
		return dir;
	done = 1;
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "rev-parse", "--git-common-dir", NULL)) {
		sb_rtrim(&sb);
		dir = realpath(sb_str(&sb), NULL);
	}
	sb_free(&sb);
	return dir;
}

const char *git_toplevel(void)
{
	static int done;
	static char *top;
	struct strbuf sb = STRBUF_INIT;

	if (done)
		return top;
	done = 1;
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "rev-parse", "--show-toplevel", NULL)) {
		sb_rtrim(&sb);
		if (sb.len)
			top = sb_detach(&sb);
	}
	sb_free(&sb);
	return top;
}

char *git_config(const char *key)
{
	struct strbuf sb = STRBUF_INIT;

	if (run_git(NULL, 0, &sb, RUN_QUIET, "config", "--get", key, NULL)) {
		sb_free(&sb);
		return NULL;
	}
	sb_rtrim(&sb);
	return sb_detach(&sb);
}

int git_config_bool(const char *key, int def)
{
	struct strbuf sb = STRBUF_INIT;
	int ret = def;

	if (!run_git(NULL, 0, &sb, RUN_QUIET, "config", "--bool", "--get", key, NULL)) {
		sb_rtrim(&sb);
		ret = !strcmp(sb_str(&sb), "true");
	}
	sb_free(&sb);
	return ret;
}

char *lfs_config(const char *key)
{
	struct strbuf sb = STRBUF_INIT;
	const char *top;
	char *v = git_config(key);
	int rc;

	if (v)
		return v;
	top = git_toplevel();
	if (top) {
		struct strbuf path = STRBUF_INIT;

		sb_addf(&path, "%s/.lfsconfig", top);
		if (file_size(path.buf) >= 0) {
			rc = run_git(NULL, 0, &sb, RUN_QUIET, "config", "-f", path.buf,
				     "--get", key, NULL);
			sb_free(&path);
			goto out;
		}
		sb_free(&path);
	}
	/* рабочего дерева или файла нет (bare, sparse) — берём .lfsconfig из HEAD */
	rc = run_git(NULL, 0, &sb, RUN_QUIET, "config", "--blob", "HEAD:.lfsconfig",
		     "--get", key, NULL);
out:
	if (rc) {
		sb_free(&sb);
		return NULL;
	}
	sb_rtrim(&sb);
	return sb_detach(&sb);
}

const char *lfs_default_remote(void)
{
	static char *remote;
	struct strbuf sb = STRBUF_INIT;

	if (remote)
		return remote;
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "symbolic-ref", "--short", "-q", "HEAD", NULL)) {
		struct strbuf key = STRBUF_INIT;

		sb_rtrim(&sb);
		sb_addf(&key, "branch.%s.remote", sb_str(&sb));
		remote = git_config(key.buf);
		sb_free(&key);
	}
	sb_reset(&sb);
	if (!remote && !run_git(NULL, 0, &sb, RUN_QUIET, "remote", NULL)) {
		sb_rtrim(&sb);
		if (sb.len && !strchr(sb.buf, '\n'))
			remote = xstrdup(sb.buf);
	}
	sb_free(&sb);
	if (!remote)
		remote = xstrdup("origin");
	return remote;
}

int lfs_concurrency(void)
{
	static int n;
	char *v;

	if (n)
		return n;
	v = lfs_config("lfs.concurrenttransfers");
	n = v ? atoi(v) : 8;
	if (n < 1)
		n = 1;
	if (n > 64)
		n = 64;
	free(v);
	return n;
}

char *git_current_ref(void)
{
	struct strbuf sb = STRBUF_INIT;

	if (run_git(NULL, 0, &sb, RUN_QUIET, "symbolic-ref", "-q", "HEAD", NULL)) {
		sb_free(&sb);
		return NULL;
	}
	sb_rtrim(&sb);
	return sb_detach(&sb);
}

const char *lfs_dir(void)
{
	static char *dir;
	const char *gd;

	if (dir)
		return dir;
	gd = git_dir();
	if (!gd)
		die("not in a git repository");
	dir = xmalloc(strlen(gd) + 5);
	sprintf(dir, "%s/lfs", gd);
	return dir;
}

char *lfs_object_path(const char *oid)
{
	struct strbuf sb = STRBUF_INIT;

	sb_addf(&sb, "%s/objects/%.2s/%.2s/%s", lfs_dir(), oid, oid + 2, oid);
	return sb_detach(&sb);
}

int lfs_object_ok(const char *oid, long long size)
{
	char *path = lfs_object_path(oid);
	long long sz = file_size(path);

	free(path);
	return sz >= 0 && sz == size;
}

int lfs_tmpfile(char **path)
{
	struct strbuf sb = STRBUF_INIT;
	int fd;

	sb_addf(&sb, "%s/tmp", lfs_dir());
	if (mkdir_p(sb.buf))
		die("cannot create %s: %s", sb.buf, strerror(errno));
	sb_adds(&sb, "/obj-XXXXXX");
	fd = mkstemp(sb.buf);
	if (fd < 0)
		die("cannot create temporary file in %s/tmp: %s", lfs_dir(), strerror(errno));
	fchmod(fd, 0644);
	*path = sb_detach(&sb);
	return fd;
}

int lfs_install_object(const char *tmp, const char *oid)
{
	char *path = lfs_object_path(oid);
	char *slash = strrchr(path, '/');
	int ret = 0;

	*slash = '\0';
	if (mkdir_p(path)) {
		warnf("cannot create %s: %s", path, strerror(errno));
		ret = -1;
	}
	*slash = '/';
	if (!ret && rename(tmp, path)) {
		warnf("cannot move object to %s: %s", path, strerror(errno));
		ret = -1;
	}
	if (ret)
		unlink(tmp);
	free(path);
	return ret;
}
