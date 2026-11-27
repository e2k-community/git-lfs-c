/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "commands.h"
#include "git.h"
#include "pathfilter.h"
#include "pktline.h"
#include "scan.h"
#include "store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void parse_skip(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc && strcmp(argv[i], "--"); i++)
		if (!strcmp(argv[i], "--skip") || !strcmp(argv[i], "-s"))
			set_skip_smudge();
}

static const char *file_arg(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc - 1; i++)
		if (!strcmp(argv[i], "--"))
			return argv[i + 1];
	return NULL;
}

static void fd_writer(void *ctx, const void *data, size_t n)
{
	if (write_all(*(int *)ctx, data, n))
		die("write: %s", strerror(errno));
}

static void pkt_writer(void *ctx, const void *data, size_t n)
{
	pkt_write_data(*(int *)ctx, data, n);
}

/* lfs.fetchexclude/fetchinclude: такие файлы оставляем pointer'ами */
static int want_content(const char *path)
{
	static struct pathfilter f;
	static int init;

	if (!init) {
		pathfilter_init(&f, NULL, NULL);
		init = 1;
	}
	return pathfilter_match(&f, path);
}

int cmd_clean(int argc, char **argv)
{
	struct strbuf out = STRBUF_INIT;
	struct cleaner c;
	char buf[65536];
	ssize_t r;

	(void)argc;
	(void)argv;
	cleaner_init(&c);
	while ((r = read(0, buf, sizeof(buf))) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			die("read: %s", strerror(errno));
		}
		cleaner_write(&c, buf, (size_t)r);
	}
	cleaner_finish(&c, &out);
	if (write_all(1, out.buf, out.len))
		die("write: %s", strerror(errno));
	sb_free(&out);
	return 0;
}

int cmd_smudge(int argc, char **argv)
{
	const char *path = file_arg(argc, argv);
	char buf[65536];
	struct pointer p;
	ssize_t n;
	int fd = 1;

	parse_skip(argc, argv);
	n = read_full(0, buf, POINTER_MAX + 1);
	if (n < 0)
		die("read: %s", strerror(errno));
	if (skip_smudge() || pointer_parse(buf, (size_t)n, &p) || !want_content(path)) {
		/* не pointer или файл исключён — отдаём как есть */
		fd_writer(&fd, buf, (size_t)n);
		while ((n = read(0, buf, sizeof(buf))) != 0) {
			if (n < 0) {
				if (errno == EINTR)
					continue;
				die("read: %s", strerror(errno));
			}
			fd_writer(&fd, buf, (size_t)n);
		}
		return 0;
	}
	if (smudge_fetch(&p, path) || smudge_write(&p, fd_writer, &fd))
		return 2;
	return 0;
}

static void send_status(const char *status)
{
	struct strbuf sb = STRBUF_INIT;

	sb_addf(&sb, "status=%s", status);
	pkt_write_text(1, sb.buf);
	pkt_flush(1);
	sb_free(&sb);
}

static void filter_clean(char *buf)
{
	struct strbuf out = STRBUF_INIT;
	struct cleaner c;
	size_t len;
	int t;

	cleaner_init(&c);
	while ((t = pkt_read(0, buf, &len)) == PKT_DATA)
		cleaner_write(&c, buf, len);
	if (t != PKT_FLUSH)
		die("filter-process: unexpected end of content");
	cleaner_finish(&c, &out);
	send_status("success");
	pkt_write_data(1, out.buf, out.len);
	pkt_flush(1);
	pkt_flush(1); /* пустой список: статус не меняется */
	sb_free(&out);
}

/*
 * Отложенный smudge (capability=delay): при clone git отдаёт все файлы, мы
 * отвечаем status=delayed и копим их, а на list_available_blobs скачиваем
 * всё одним batch-запросом в несколько потоков.
 */
struct delay_state {
	struct ptrlist pending;   /* path — pathname от git */
	struct ptrlist ready;     /* отдали в list_available_blobs, отсортирован по path */
	struct ptrlist failed;    /* не скачались — второй раз не откладываем */
};

static struct delay_state ds;

static int cmp_path(const void *a, const void *b)
{
	return strcmp(((const struct lfs_obj *)a)->path, ((const struct lfs_obj *)b)->path);
}

static const struct lfs_obj *find_path(const struct ptrlist *l, const char *path, int sorted)
{
	struct lfs_obj key;
	size_t i;

	if (!path || !l->n)
		return NULL;
	if (sorted) {
		key.path = (char *)path;
		return bsearch(&key, l->v, l->n, sizeof(*l->v), cmp_path);
	}
	for (i = 0; i < l->n; i++)
		if (!strcmp(l->v[i].path, path))
			return &l->v[i];
	return NULL;
}

static void list_available_blobs(void)
{
	struct ptrlist uniq = { NULL, 0, 0 };
	size_t i;

	if (ds.pending.n) {
		for (i = 0; i < ds.pending.n; i++)
			ptrlist_add(&uniq, ds.pending.v[i].oid, ds.pending.v[i].size,
				    ds.pending.v[i].path);
		ptrlist_dedup(&uniq);
		lfs_transfer(NULL, "download", uniq.v, uniq.n);
		ptrlist_free(&uniq);

		for (i = 0; i < ds.pending.n; i++) {
			const struct lfs_obj *o = &ds.pending.v[i];
			struct strbuf sb = STRBUF_INIT;

			if (!lfs_object_ok(o->oid, o->size))
				ptrlist_add(&ds.failed, o->oid, o->size, o->path);
			ptrlist_add(&ds.ready, o->oid, o->size, o->path);
			sb_addf(&sb, "pathname=%s", o->path);
			pkt_write_text(1, sb.buf);
			sb_free(&sb);
		}
		ptrlist_free(&ds.pending);
		qsort(ds.ready.v, ds.ready.n, sizeof(*ds.ready.v), cmp_path);
	}
	pkt_flush(1);
	send_status("success");
}

static void filter_smudge(char *buf, const char *pathname, int can_delay)
{
	/* git сначала отдаёт весь вход и только потом читает ответ */
	struct strbuf in = STRBUF_INIT;
	const struct lfs_obj *ready;
	struct pointer p;
	size_t len;
	int t, fd = 1, is_ptr;

	while ((t = pkt_read(0, buf, &len)) == PKT_DATA)
		sb_add(&in, buf, len);
	if (t != PKT_FLUSH)
		die("filter-process: unexpected end of content");

	is_ptr = !pointer_parse(in.buf, in.len, &p);
	/* повторный запрос отложенного файла приходит с пустым содержимым */
	if (!is_ptr && !in.len && (ready = find_path(&ds.ready, pathname, 1))) {
		memcpy(p.oid, ready->oid, 65);
		p.size = ready->size;
		is_ptr = 1;
	}

	if (skip_smudge() || !is_ptr || !want_content(pathname)) {
		send_status("success");
		pkt_write_data(1, in.buf, in.len);
		goto done;
	}
	if (!lfs_object_ok(p.oid, p.size)) {
		if (can_delay && pathname && !find_path(&ds.failed, pathname, 0)) {
			ptrlist_add(&ds.pending, p.oid, p.size, pathname);
			send_status("delayed");
			sb_free(&in);
			return;
		}
		if (smudge_fetch(&p, pathname)) {
			send_status("error");
			sb_free(&in);
			return;
		}
	}
	send_status("success");
	if (smudge_write(&p, pkt_writer, &fd)) {
		pkt_flush(1);
		send_status("error");
		sb_free(&in);
		return;
	}
done:
	pkt_flush(1);
	pkt_flush(1);
	sb_free(&in);
}

static void expect_text(char *buf, const char *want)
{
	if (pkt_read_text(0, buf) != PKT_DATA || strcmp(buf, want))
		die("filter-process: expected '%s'", want);
}

int cmd_filter_process(int argc, char **argv)
{
	char *buf = xmalloc(PKT_MAX_DATA + 1);
	int t, can_clean = 0, can_smudge = 0, can_delay = 0, have_version = 0;

	parse_skip(argc, argv);
	/* как git-lfs: после clone в репозитории должен появиться pre-push */
	if (git_dir())
		hooks_install(0, 0);

	expect_text(buf, "git-filter-client");
	while ((t = pkt_read_text(0, buf)) == PKT_DATA)
		if (!strcmp(buf, "version=2"))
			have_version = 1;
	if (t != PKT_FLUSH || !have_version)
		die("filter-process: unsupported protocol version");
	pkt_write_text(1, "git-filter-server");
	pkt_write_text(1, "version=2");
	pkt_flush(1);

	while ((t = pkt_read_text(0, buf)) == PKT_DATA) {
		if (!strcmp(buf, "capability=clean"))
			can_clean = 1;
		else if (!strcmp(buf, "capability=smudge"))
			can_smudge = 1;
		else if (!strcmp(buf, "capability=delay"))
			can_delay = 1;
	}
	if (t != PKT_FLUSH)
		die("filter-process: bad capabilities");
	if (env_true("GIT_LFS_NO_DELAY"))
		can_delay = 0;
	if (can_clean)
		pkt_write_text(1, "capability=clean");
	if (can_smudge)
		pkt_write_text(1, "capability=smudge");
	if (can_delay)
		pkt_write_text(1, "capability=delay");
	pkt_flush(1);

	for (;;) {
		char *command = NULL, *pathname = NULL;
		int delay_ok = 0;
		const char *v;

		while ((t = pkt_read_text(0, buf)) == PKT_DATA) {
			if ((v = skip_prefix(buf, "command="))) {
				free(command);
				command = xstrdup(v);
			} else if ((v = skip_prefix(buf, "pathname="))) {
				free(pathname);
				pathname = xstrdup(v);
			} else if (!strcmp(buf, "can-delay=1")) {
				delay_ok = can_delay;
			}
		}
		if (t == PKT_EOF && !command)
			break;
		if (t != PKT_FLUSH || !command)
			die("filter-process: bad request");
		trace("filter-process: %s %s", command, pathname ? pathname : "");
		if (!strcmp(command, "clean")) {
			filter_clean(buf);
		} else if (!strcmp(command, "smudge")) {
			filter_smudge(buf, pathname, delay_ok);
		} else if (!strcmp(command, "list_available_blobs")) {
			list_available_blobs();
		} else {
			size_t len;

			while (pkt_read(0, buf, &len) == PKT_DATA)
				;
			send_status("error");
		}
		free(command);
		free(pathname);
	}
	free(buf);
	return 0;
}
