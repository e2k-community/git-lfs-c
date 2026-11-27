/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "commands.h"
#include "git.h"
#include "pathfilter.h"
#include "pointer.h"
#include "run.h"
#include "scan.h"
#include "store.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct fetch_opts {
	const char *include, *exclude;
	int all;
	const char *remote;
	const char **refs;
	int nrefs;
};

/* [-I <pats>] [-X <pats>] [--all] [<remote> [<ref>...]] */
static void parse_fetch_opts(int argc, char **argv, struct fetch_opts *o, int allow_all)
{
	int i;

	memset(o, 0, sizeof(*o));
	o->refs = xcalloc((size_t)argc + 1, sizeof(char *));
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v;

		if ((!strcmp(a, "-I") || !strcmp(a, "--include")) && i + 1 < argc)
			o->include = argv[++i];
		else if ((v = skip_prefix(a, "--include=")))
			o->include = v;
		else if ((!strcmp(a, "-X") || !strcmp(a, "--exclude")) && i + 1 < argc)
			o->exclude = argv[++i];
		else if ((v = skip_prefix(a, "--exclude=")))
			o->exclude = v;
		else if (allow_all && (!strcmp(a, "--all") || !strcmp(a, "-a")))
			o->all = 1;
		else if (a[0] == '-')
			die("unknown option '%s'", a);
		else if (!o->remote)
			o->remote = a;
		else
			o->refs[o->nrefs++] = a;
	}
}

static int do_fetch(const struct fetch_opts *o)
{
	struct ptrlist all = { NULL, 0, 0 }, need = { NULL, 0, 0 };
	struct pathfilter f;
	size_t i;
	int failed, r;

	if (o->all) {
		const char *revs[] = { "--all", NULL };

		scan_revs(revs, &all);
	} else if (!o->nrefs) {
		scan_tree("HEAD", &all);
	} else {
		for (r = 0; r < o->nrefs; r++)
			scan_tree(o->refs[r], &all);
	}
	pathfilter_init(&f, o->include, o->exclude);
	for (i = 0; i < all.n; i++)
		if (pathfilter_match(&f, all.v[i].path) && !lfs_object_ok(all.v[i].oid, all.v[i].size))
			ptrlist_add(&need, all.v[i].oid, all.v[i].size, all.v[i].path);
	ptrlist_dedup(&need);
	pathfilter_free(&f);

	if (need.n)
		fprintf(stderr, "Downloading LFS objects: %zu\n", need.n);
	failed = lfs_transfer(o->remote, "download", need.v, need.n);
	if (failed)
		warnf("failed to fetch %d object(s)", failed);
	ptrlist_free(&all);
	ptrlist_free(&need);
	return failed ? 2 : 0;
}

/* Файл в рабочем дереве — всё ещё pointer на этот объект */
static int worktree_is_pointer(const char *top, const struct lfs_obj *o)
{
	struct strbuf path = STRBUF_INIT, data = STRBUF_INIT;
	struct pointer p;
	long long sz;
	int ret = 0;

	sb_addf(&path, "%s/%s", top, o->path);
	sz = file_size(path.buf);
	if (sz > 0 && sz <= POINTER_MAX && !read_file(path.buf, &data) &&
	    !pointer_parse(data.buf, data.len, &p) && !strcmp(p.oid, o->oid))
		ret = 1;
	sb_free(&path);
	sb_free(&data);
	return ret;
}

static void fd_writer(void *ctx, const void *data, size_t n)
{
	int *fd = ctx;

	if (*fd >= 0 && write_all(*fd, data, n)) {
		warnf("write: %s", strerror(errno));
		close(*fd);
		*fd = -1;
	}
}

/*
 * Подменяет pointer в рабочем дереве содержимым объекта: пишем во временный
 * файл рядом и переименовываем, права берём у текущего файла.
 */
static int replace_worktree_file(const char *top, const struct lfs_obj *o)
{
	struct strbuf path = STRBUF_INIT, tmp = STRBUF_INIT;
	struct pointer p;
	struct stat st;
	int fd, ret = -1;

	sb_addf(&path, "%s/%s", top, o->path);
	sb_addf(&tmp, "%s.lfs-XXXXXX", path.buf);
	if (stat(path.buf, &st) || (fd = mkstemp(tmp.buf)) < 0) {
		warnf("cannot update %s: %s", o->path, strerror(errno));
		goto out;
	}
	fchmod(fd, st.st_mode & 07777);
	memcpy(p.oid, o->oid, 65);
	p.size = o->size;
	if (smudge_write(&p, fd_writer, &fd) || fd < 0 || close(fd)) {
		if (fd >= 0)
			close(fd);
		unlink(tmp.buf);
		warnf("cannot update %s", o->path);
		goto out;
	}
	if (rename(tmp.buf, path.buf)) {
		warnf("cannot update %s: %s", o->path, strerror(errno));
		unlink(tmp.buf);
		goto out;
	}
	ret = 0;
out:
	sb_free(&path);
	sb_free(&tmp);
	return ret;
}

static int do_checkout(void)
{
	struct ptrlist all = { NULL, 0, 0 };
	struct strbuf paths = STRBUF_INIT;
	const char *top = git_toplevel();
	size_t i;
	int ret = 0;

	if (!top)
		die("not in a git working tree");
	scan_tree("HEAD", &all);
	for (i = 0; i < all.n; i++) {
		if (!lfs_object_ok(all.v[i].oid, all.v[i].size) || !worktree_is_pointer(top, &all.v[i]))
			continue;
		if (replace_worktree_file(top, &all.v[i]))
			ret = 2;
		else
			sb_add(&paths, all.v[i].path, strlen(all.v[i].path) + 1);
	}
	/*
	 * В индексе записан размер pointer'а, и --refresh такие файлы содержимым
	 * не сверяет. Переиндексируем: clean даст тот же pointer, обновится только stat.
	 */
	if (paths.len && run_git(paths.buf, paths.len, NULL, 0, "-C", top, "update-index",
				 "-q", "-z", "--stdin", NULL)) {
		warnf("git update-index failed");
		ret = 2;
	}
	ptrlist_free(&all);
	sb_free(&paths);
	return ret;
}

int cmd_fetch(int argc, char **argv)
{
	struct fetch_opts o;
	int ret;

	parse_fetch_opts(argc, argv, &o, 1);
	ret = do_fetch(&o);
	free(o.refs);
	return ret;
}

int cmd_checkout(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return do_checkout();
}

int cmd_pull(int argc, char **argv)
{
	struct fetch_opts o;
	int ret, ret2;

	parse_fetch_opts(argc, argv, &o, 0);
	ret = do_fetch(&o);
	ret2 = do_checkout();
	free(o.refs);
	return ret ? ret : ret2;
}

int cmd_ls_files(int argc, char **argv)
{
	struct ptrlist all = { NULL, 0, 0 };
	const char *top = git_toplevel();
	const char *ref = "HEAD";
	int i, show_size = 0, long_oid = 0, name_only = 0;
	size_t k;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--size"))
			show_size = 1;
		else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--long"))
			long_oid = 1;
		else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--name-only"))
			name_only = 1;
		else if (argv[i][0] == '-')
			die("unknown option '%s'", argv[i]);
		else
			ref = argv[i];
	}
	scan_tree(ref, &all);
	for (k = 0; k < all.n; k++) {
		int full = top && !worktree_is_pointer(top, &all.v[k]);
		char sz[32];

		if (name_only) {
			puts(all.v[k].path);
			continue;
		}
		printf("%.*s %c %s", long_oid ? 64 : 10, all.v[k].oid, full ? '*' : '-',
		       all.v[k].path);
		if (show_size) {
			format_size(all.v[k].size, sz, sizeof(sz));
			printf(" (%s)", sz);
		}
		putchar('\n');
	}
	ptrlist_free(&all);
	return 0;
}

static int is_zero_oid(const char *s)
{
	return s[strspn(s, "0")] == '\0';
}

static int upload(const char *remote, struct ptrlist *objs, int dry_run)
{
	size_t i;
	int failed;

	ptrlist_dedup(objs);
	if (dry_run) {
		for (i = 0; i < objs->n; i++)
			printf("push %s => %s\n", objs->v[i].oid,
			       objs->v[i].path ? objs->v[i].path : "");
		return 0;
	}
	if (objs->n)
		fprintf(stderr, "Uploading LFS objects: %zu\n", objs->n);
	failed = lfs_transfer(remote, "upload", objs->v, objs->n);
	if (failed) {
		warnf("failed to push %d LFS object(s)", failed);
		return 2;
	}
	return 0;
}

/* stdin: "<local ref> <local sha> <remote ref> <remote sha>" (githooks(5)) */
int cmd_pre_push(int argc, char **argv)
{
	struct ptrlist objs = { NULL, 0, 0 };
	struct strbuf in = STRBUF_INIT, remotes = STRBUF_INIT;
	const char *remote;
	char *line, *save = NULL;
	int ret;

	if (argc < 2)
		die("usage: git lfs pre-push <remote> [<url>]");
	remote = argv[1];
	if (read_file("/dev/stdin", &in))
		die("cannot read stdin");
	sb_addf(&remotes, "--remotes=%s", remote);

	for (line = strtok_r((char *)sb_str(&in), "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char lref[1024], lsha[128], rref[1024], rsha[128];
		const char *revs[6];
		int n = 0;

		if (sscanf(line, "%1023s %127s %1023s %127s", lref, lsha, rref, rsha) != 4)
			continue;
		if (is_zero_oid(lsha))
			continue; /* удаление ветки */
		revs[n++] = lsha;
		revs[n++] = "--not";
		revs[n++] = remotes.buf;
		if (!is_zero_oid(rsha) &&
		    !run_git(NULL, 0, NULL, RUN_QUIET, "cat-file", "-e", rsha, NULL))
			revs[n++] = rsha;
		revs[n] = NULL;
		scan_revs(revs, &objs);
	}
	ret = upload(remote, &objs, 0);
	ptrlist_free(&objs);
	sb_free(&in);
	sb_free(&remotes);
	return ret;
}

/* push [--all] [--object-id] [--dry-run] <remote> [<ref>|<oid>...] */
int cmd_push(int argc, char **argv)
{
	struct ptrlist objs = { NULL, 0, 0 };
	struct strbuf remotes = STRBUF_INIT;
	const char **args = xcalloc((size_t)argc + 4, sizeof(char *));
	const char *remote = NULL;
	int i, n = 0, all = 0, by_oid = 0, dry_run = 0, ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--all"))
			all = 1;
		else if (!strcmp(argv[i], "--object-id"))
			by_oid = 1;
		else if (!strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "-d"))
			dry_run = 1;
		else if (argv[i][0] == '-')
			die("unknown option '%s'", argv[i]);
		else if (!remote)
			remote = argv[i];
		else
			args[n++] = argv[i];
	}
	if (!remote)
		die("usage: git lfs push [--all|--object-id] <remote> [<ref>|<oid>...]");

	if (by_oid) {
		for (i = 0; i < n; i++) {
			char *path;
			long long sz;

			if (!is_oid(args[i]))
				die("'%s' is not a sha256 object id", args[i]);
			path = lfs_object_path(args[i]);
			sz = file_size(path);
			free(path);
			if (sz < 0)
				die("object %s is missing locally", args[i]);
			ptrlist_add(&objs, args[i], sz, NULL);
		}
	} else {
		if (!n && !all)
			args[n++] = "HEAD";
		if (!n)
			args[n++] = "--all";
		if (!all) {
			/* как pre-push: только то, чего ещё нет в remote-tracking ветках */
			sb_addf(&remotes, "--remotes=%s", remote);
			args[n++] = "--not";
			args[n++] = remotes.buf;
		}
		args[n] = NULL;
		scan_revs(args, &objs);
	}
	ret = upload(remote, &objs, dry_run);
	ptrlist_free(&objs);
	sb_free(&remotes);
	free(args);
	return ret;
}

/* Все объекты в .git/lfs/objects/xx/yy/<oid> */
static void list_local_objects(struct ptrlist *out)
{
	struct strbuf dir = STRBUF_INIT;
	DIR *d1, *d2, *d3;
	struct dirent *a, *b, *c;

	sb_addf(&dir, "%s/objects", lfs_dir());
	if (!(d1 = opendir(dir.buf))) {
		sb_free(&dir);
		return;
	}
	while ((a = readdir(d1))) {
		size_t l1;

		if (strlen(a->d_name) != 2 || a->d_name[0] == '.')
			continue;
		l1 = dir.len;
		sb_addf(&dir, "/%s", a->d_name);
		if ((d2 = opendir(dir.buf))) {
			while ((b = readdir(d2))) {
				size_t l2;

				if (strlen(b->d_name) != 2 || b->d_name[0] == '.')
					continue;
				l2 = dir.len;
				sb_addf(&dir, "/%s", b->d_name);
				if ((d3 = opendir(dir.buf))) {
					while ((c = readdir(d3))) {
						struct strbuf f = STRBUF_INIT;
						long long sz;

						if (!is_oid(c->d_name))
							continue;
						sb_addf(&f, "%s/%s", dir.buf, c->d_name);
						sz = file_size(f.buf);
						if (sz >= 0)
							ptrlist_add(out, c->d_name, sz, NULL);
						sb_free(&f);
					}
					closedir(d3);
				}
				dir.len = l2;
				dir.buf[l2] = '\0';
			}
			closedir(d2);
		}
		dir.len = l1;
		dir.buf[l1] = '\0';
	}
	closedir(d1);
	sb_free(&dir);
}

/* Что нельзя удалять: HEAD и вершины веток, индекс, рабочие деревья, неотправленное */
static void collect_retained(struct ptrlist *keep)
{
	struct strbuf sb = STRBUF_INIT;
	const char *unpushed[] = { "--branches", "--not", "--remotes", NULL };
	char *line, *save = NULL;

	scan_tree("HEAD", keep);
	scan_index(keep);
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "for-each-ref", "--format=%(objectname)",
		     "refs/heads", NULL)) {
		for (line = strtok_r((char *)sb_str(&sb), "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save))
			scan_tree(line, keep);
	}
	sb_reset(&sb);
	save = NULL;
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "worktree", "list", "--porcelain", NULL)) {
		for (line = strtok_r((char *)sb_str(&sb), "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			const char *v = skip_prefix(line, "HEAD ");

			if (v)
				scan_tree(v, keep);
		}
	}
	sb_reset(&sb);
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "rev-parse", "-q", "--verify", "refs/stash", NULL)) {
		const char *stash[] = { "refs/stash", "--not", "--remotes", NULL };

		scan_revs(stash, keep);
	}
	scan_revs(unpushed, keep);
	sb_free(&sb);
	ptrlist_dedup(keep);
}

static void prune_tmp(void)
{
	struct strbuf path = STRBUF_INIT;
	struct dirent *e;
	time_t now = time(NULL);
	DIR *d;

	sb_addf(&path, "%s/tmp", lfs_dir());
	if (!(d = opendir(path.buf))) {
		sb_free(&path);
		return;
	}
	while ((e = readdir(d))) {
		struct strbuf f = STRBUF_INIT;
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		sb_addf(&f, "%s/%s", path.buf, e->d_name);
		/* свежие файлы может прямо сейчас писать другой процесс */
		if (!stat(f.buf, &st) && S_ISREG(st.st_mode) && now - st.st_mtime > 3600)
			unlink(f.buf);
		sb_free(&f);
	}
	closedir(d);
	sb_free(&path);
}

int cmd_prune(int argc, char **argv)
{
	struct ptrlist local = { NULL, 0, 0 }, keep = { NULL, 0, 0 };
	long long bytes = 0;
	size_t i, ndel = 0;
	int dry_run = 0, verbose = 0;
	char sz[32];

	for (i = 1; i < (size_t)argc; i++) {
		if (!strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "-d"))
			dry_run = 1;
		else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
			verbose = 1;
		else
			die("unknown option '%s'", argv[i]);
	}
	list_local_objects(&local);
	collect_retained(&keep);

	for (i = 0; i < local.n; i++) {
		char *path;

		if (ptrlist_has(&keep, local.v[i].oid))
			continue;
		ndel++;
		bytes += local.v[i].size;
		if (verbose)
			printf(" * %s (%lld)\n", local.v[i].oid, local.v[i].size);
		if (dry_run)
			continue;
		path = lfs_object_path(local.v[i].oid);
		if (unlink(path))
			warnf("cannot delete %s: %s", path, strerror(errno));
		free(path);
	}
	format_size(bytes, sz, sizeof(sz));
	printf("prune: %zu local object(s), %zu retained\n", local.n, local.n - ndel);
	printf("prune: %s %zu object(s), %s\n", dry_run ? "would delete" : "deleted", ndel, sz);
	if (!dry_run)
		prune_tmp();
	ptrlist_free(&local);
	ptrlist_free(&keep);
	return 0;
}
