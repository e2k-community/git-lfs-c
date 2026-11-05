#include "scan.h"
#include "pointer.h"
#include "run.h"

#include <stdlib.h>
#include <string.h>

#define MAX_REV_ARGS 64

struct cand {
	char oid[65];
	char *path;
};

struct candlist {
	struct cand *v;
	size_t n, cap;
};

void ptrlist_add(struct ptrlist *l, const char *oid, long long size, const char *path)
{
	struct lfs_obj *o;

	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 16;
		l->v = xrealloc(l->v, l->cap * sizeof(*l->v));
	}
	o = &l->v[l->n++];
	memcpy(o->oid, oid, 65);
	o->size = size;
	o->path = path ? xstrdup(path) : NULL;
}

static int cmp_obj(const void *a, const void *b)
{
	return strcmp(((const struct lfs_obj *)a)->oid, ((const struct lfs_obj *)b)->oid);
}

void ptrlist_dedup(struct ptrlist *l)
{
	size_t i, j = 0;

	if (!l->n)
		return;
	qsort(l->v, l->n, sizeof(*l->v), cmp_obj);
	for (i = 1; i < l->n; i++) {
		if (!strcmp(l->v[i].oid, l->v[j].oid)) {
			free(l->v[i].path);
			continue;
		}
		l->v[++j] = l->v[i];
	}
	l->n = j + 1;
}

int ptrlist_has(const struct ptrlist *l, const char *oid)
{
	struct lfs_obj key;

	memcpy(key.oid, oid, 65);
	return l->n && bsearch(&key, l->v, l->n, sizeof(*l->v), cmp_obj) != NULL;
}

void ptrlist_free(struct ptrlist *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free(l->v[i].path);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static void cand_add(struct candlist *c, const char *oid, size_t oidlen,
		     const char *path, size_t pathlen)
{
	struct cand *x;

	/* id git-объекта: SHA-1 (40) или SHA-256 (64) */
	if (oidlen != 40 && oidlen != 64)
		return;
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 64;
		c->v = xrealloc(c->v, c->cap * sizeof(*c->v));
	}
	x = &c->v[c->n++];
	memcpy(x->oid, oid, oidlen);
	x->oid[oidlen] = '\0';
	x->path = path ? xstrndup(path, pathlen) : NULL;
}

/* Читает мелкие блобы через cat-file --batch и оставляет только pointers */
static void read_pointers(struct candlist *c, struct ptrlist *out)
{
	struct strbuf in = STRBUF_INIT, res = STRBUF_INIT;
	const char *p, *end;
	size_t i;

	if (!c->n)
		return;
	for (i = 0; i < c->n; i++)
		sb_addf(&in, "%s\n", c->v[i].oid);
	if (run_git(in.buf, in.len, &res, 0, "cat-file", "--batch", NULL))
		die("git cat-file --batch failed");

	p = res.buf;
	end = res.buf + res.len;
	for (i = 0; i < c->n && p && p < end; i++) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *sp;
		struct pointer ptr;
		long long size;

		if (!nl)
			break;
		/* "<oid> <type> <size>" или "<oid> missing" */
		sp = memchr(p, ' ', (size_t)(nl - p));
		if (!sp || !strncmp(sp + 1, "missing", 7)) {
			p = nl + 1;
			continue;
		}
		sp = memchr(sp + 1, ' ', (size_t)(nl - sp - 1));
		if (!sp)
			break;
		size = strtoll(sp + 1, NULL, 10);
		if (size < 0 || nl + 1 + size + 1 > end)
			break;
		if (!pointer_parse(nl + 1, (size_t)size, &ptr))
			ptrlist_add(out, ptr.oid, ptr.size, c->v[i].path);
		p = nl + 1 + size + 1;
	}

	for (i = 0; i < c->n; i++)
		free(c->v[i].path);
	free(c->v);
	memset(c, 0, sizeof(*c));
	sb_free(&in);
	sb_free(&res);
}

void scan_tree(const char *treeish, struct ptrlist *out)
{
	struct strbuf res = STRBUF_INIT;
	struct candlist c = { NULL, 0, 0 };
	const char *p, *end;

	if (run_git(NULL, 0, &res, RUN_QUIET, "ls-tree", "-r", "-l", "-z", "--full-tree",
		    treeish, NULL)) {
		sb_free(&res);
		return; /* пустой репозиторий или нет такого дерева */
	}
	/* "<mode> <type> <oid> <size>\t<path>\0" */
	p = res.buf;
	end = res.buf + res.len;
	while (p && p < end) {
		const char *rec_end = memchr(p, '\0', (size_t)(end - p));
		const char *tab, *type, *oid, *size;
		long long sz;

		if (!rec_end)
			rec_end = end;
		tab = memchr(p, '\t', (size_t)(rec_end - p));
		type = memchr(p, ' ', (size_t)(rec_end - p));
		if (tab && type && type < tab) {
			type++;
			oid = memchr(type, ' ', (size_t)(tab - type));
			if (oid && !strncmp(type, "blob ", 5)) {
				oid++;
				size = memchr(oid, ' ', (size_t)(tab - oid));
				if (size) {
					sz = strtoll(size, NULL, 10);
					if (sz > 0 && sz <= POINTER_MAX)
						cand_add(&c, oid, (size_t)(size - oid), tab + 1,
							 (size_t)(rec_end - tab - 1));
				}
			}
		}
		p = rec_end + 1;
	}
	sb_free(&res);
	read_pointers(&c, out);
}

void scan_object_lines(const char *lines, size_t len, struct ptrlist *out)
{
	struct strbuf res = STRBUF_INIT;
	struct candlist c = { NULL, 0, 0 };
	char *line, *save = NULL;

	if (!len)
		return;
	if (run_git(lines, len, &res, 0, "cat-file",
		    "--batch-check=%(objectname) %(objecttype) %(objectsize) %(rest)", NULL))
		die("git cat-file --batch-check failed");

	for (line = strtok_r((char *)sb_str(&res), "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *type = strchr(line, ' '), *size, *path;
		long long sz;

		if (!type || strncmp(type + 1, "blob ", 5))
			continue;
		size = type + 6;
		sz = strtoll(size, &path, 10);
		if (sz <= 0 || sz > POINTER_MAX)
			continue;
		if (*path == ' ')
			path++;
		cand_add(&c, line, (size_t)(type - line), *path ? path : NULL, strlen(path));
	}
	sb_free(&res);
	read_pointers(&c, out);
}

void scan_revs(const char *const *revs, struct ptrlist *out)
{
	struct strbuf objs = STRBUF_INIT;
	const char *args[MAX_REV_ARGS + 8];
	int n = 0;

	args[n++] = "rev-list";
	args[n++] = "--objects";
	/* блобы больше pointer'а нам не нужны */
	args[n++] = "--filter=blob:limit=1025";
	while (*revs) {
		if (n >= MAX_REV_ARGS)
			die("too many revisions");
		args[n++] = *revs++;
	}
	args[n] = NULL;
	if (run_git_argv(NULL, 0, &objs, 0, args))
		die("git rev-list failed");
	scan_object_lines(objs.buf, objs.len, out);
	sb_free(&objs);
}

void scan_index(struct ptrlist *out)
{
	struct strbuf res = STRBUF_INIT, lines = STRBUF_INIT;
	const char *p, *end;

	if (run_git(NULL, 0, &res, RUN_QUIET, "ls-files", "-s", "-z", NULL)) {
		sb_free(&res);
		return;
	}
	/* "<mode> <oid> <stage>\t<path>\0" */
	p = res.buf;
	end = res.buf + res.len;
	while (p && p < end) {
		const char *rec_end = memchr(p, '\0', (size_t)(end - p));
		const char *sp = memchr(p, ' ', (size_t)((rec_end ? rec_end : end) - p));
		const char *tab = memchr(p, '\t', (size_t)((rec_end ? rec_end : end) - p));

		if (!rec_end)
			rec_end = end;
		if (sp && tab && sp < tab) {
			const char *oid = sp + 1, *oid_end = memchr(oid, ' ', (size_t)(tab - oid));

			if (oid_end) {
				sb_add(&lines, oid, (size_t)(oid_end - oid));
				sb_addch(&lines, ' ');
				sb_add(&lines, tab + 1, (size_t)(rec_end - tab - 1));
				sb_addch(&lines, '\n');
			}
		}
		p = rec_end + 1;
	}
	scan_object_lines(lines.buf, lines.len, out);
	sb_free(&res);
	sb_free(&lines);
}
