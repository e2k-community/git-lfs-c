#include "commands.h"
#include "git.h"
#include "pathfilter.h"
#include "run.h"
#include "store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * migrate import: git fast-export --no-data | (мы) | git fast-import --force.
 * В потоке файлы ссылаются на блобы по sha; подходящие под --include мы
 * прогоняем через clean (объект ложится в .git/lfs/objects) и подставляем
 * pointer как inline-данные. В корневые коммиты добавляется .gitattributes,
 * а существующий .gitattributes дополняется шаблонами.
 */

struct conv {
	char sha[65];
	char *ptr;      /* pointer или NULL — оставить блоб как есть */
	size_t len;
	int used;
};

struct migrator {
	struct pathfilter f;
	struct child catfile;
	struct reader catrd;
	FILE *imp;
	struct conv *map;
	size_t cap, n;
	size_t converted;
};

static size_t sha_hash(const char *s)
{
	size_t h = 0;
	int i;

	for (i = 0; i < 16 && s[i]; i++)
		h = h * 16 + (size_t)hexval((unsigned char)s[i]);
	return h;
}

static struct conv *map_slot(struct migrator *m, const char *sha)
{
	size_t i = sha_hash(sha) & (m->cap - 1);

	while (m->map[i].used && strcmp(m->map[i].sha, sha))
		i = (i + 1) & (m->cap - 1);
	return &m->map[i];
}

static void map_grow(struct migrator *m)
{
	struct conv *old = m->map;
	size_t oldcap = m->cap, i;

	m->cap = m->cap ? m->cap * 2 : 1024;
	m->map = xcalloc(m->cap, sizeof(*m->map));
	for (i = 0; i < oldcap; i++)
		if (old[i].used)
			*map_slot(m, old[i].sha) = old[i];
	free(old);
}

static const struct conv *convert_blob(struct migrator *m, const char *sha)
{
	struct strbuf line = STRBUF_INIT, out = STRBUF_INIT;
	struct cleaner c;
	struct conv *e;
	char buf[65536], nl;
	long long size, left;
	const char *sp;

	if (m->cap && (e = map_slot(m, sha))->used)
		return e;
	if ((m->n + 1) * 2 > m->cap)
		map_grow(m);

	sb_addf(&line, "%s\n", sha);
	if (write_all(m->catfile.in, line.buf, line.len))
		die("git cat-file: %s", strerror(errno));
	if (reader_line(&m->catrd, &line) || !(sp = strrchr(line.buf, ' ')) ||
	    strstr(line.buf, " missing"))
		die("git cat-file: cannot read blob %s", sha);
	size = strtoll(sp + 1, NULL, 10);

	cleaner_init(&c);
	for (left = size; left > 0; ) {
		size_t chunk = left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf);

		if (reader_bytes(&m->catrd, buf, chunk) != chunk)
			die("git cat-file: short read for %s", sha);
		cleaner_write(&c, buf, chunk);
		left -= (long long)chunk;
	}
	if (reader_bytes(&m->catrd, &nl, 1) != 1)
		die("git cat-file: short read for %s", sha);
	cleaner_finish(&c, &out);

	e = map_slot(m, sha);
	e->used = 1;
	memcpy(e->sha, sha, strlen(sha) + 1);
	if (out.len) {
		e->len = out.len;
		e->ptr = sb_detach(&out);
		m->converted++;
	}
	m->n++;
	sb_free(&line);
	sb_free(&out);
	return e;
}

/* Путь из fast-export может быть в C-кавычках */
static char *unquote_path(const char *s)
{
	struct strbuf sb = STRBUF_INIT;

	if (*s != '"')
		return xstrdup(s);
	for (s++; *s && *s != '"'; s++) {
		if (*s != '\\') {
			sb_addch(&sb, *s);
			continue;
		}
		s++;
		switch (*s) {
		case 'a': sb_addch(&sb, '\a'); break;
		case 'b': sb_addch(&sb, '\b'); break;
		case 'f': sb_addch(&sb, '\f'); break;
		case 'n': sb_addch(&sb, '\n'); break;
		case 'r': sb_addch(&sb, '\r'); break;
		case 't': sb_addch(&sb, '\t'); break;
		case 'v': sb_addch(&sb, '\v'); break;
		case '\0': s--; break;
		default:
			if (*s >= '0' && *s <= '7') {
				int v = 0, k;

				for (k = 0; k < 3 && *s >= '0' && *s <= '7'; k++, s++)
					v = v * 8 + (*s - '0');
				s--;
				sb_addch(&sb, v);
			} else {
				sb_addch(&sb, *s);
			}
		}
	}
	return sb_detach(&sb);
}

static void emit_inline(struct migrator *m, const char *mode, const char *pathtok,
			const char *data, size_t len)
{
	fprintf(m->imp, "M %s inline %s\ndata %zu\n", mode, pathtok, len);
	fwrite(data, 1, len, m->imp);
	fputc('\n', m->imp);
}

static void add_patterns(struct migrator *m, struct strbuf *attrs)
{
	size_t i;

	for (i = 0; i < m->f.ninc; i++)
		gitattributes_track(attrs, m->f.inc[i]);
}

static void inject_attributes(struct migrator *m)
{
	struct strbuf attrs = STRBUF_INIT;

	add_patterns(m, &attrs);
	emit_inline(m, "100644", ".gitattributes", attrs.buf, attrs.len);
	sb_free(&attrs);
}

/* "M <mode> <sha> <path>" */
static void handle_modify(struct migrator *m, const char *line)
{
	char mode[16], ref[80];
	const char *pathtok;
	char *path;
	int n = 0;

	if (sscanf(line, "M %15s %79s %n", mode, ref, &n) != 2 || !n) {
		fprintf(m->imp, "%s\n", line);
		return;
	}
	pathtok = line + n;
	path = unquote_path(pathtok);

	if (!strcmp(path, ".gitattributes")) {
		struct strbuf attrs = STRBUF_INIT;

		if (run_git(NULL, 0, &attrs, 0, "cat-file", "blob", ref, NULL))
			die("cannot read .gitattributes blob %s", ref);
		add_patterns(m, &attrs);
		emit_inline(m, mode, pathtok, attrs.buf, attrs.len);
		sb_free(&attrs);
	} else if ((!strcmp(mode, "100644") || !strcmp(mode, "100755")) && ref[0] != ':' &&
		   strcmp(ref, "inline") && pathfilter_match(&m->f, path)) {
		const struct conv *e = convert_blob(m, ref);

		if (e->ptr)
			emit_inline(m, mode, pathtok, e->ptr, e->len);
		else
			fprintf(m->imp, "%s\n", line);
	} else {
		fprintf(m->imp, "%s\n", line);
	}
	free(path);
}

static int is_commit_header(const char *l)
{
	static const char *const h[] = { "mark ", "author ", "committer ", "encoding ",
					 "original-oid ", "from ", "merge ", "gpgsig ", NULL };
	int i;

	for (i = 0; h[i]; i++)
		if (!strncmp(l, h[i], strlen(h[i])))
			return 1;
	return 0;
}

static int is_command(const char *l)
{
	static const char *const c[] = { "commit ", "tag ", "reset ", "blob", "checkpoint",
					 "progress ", "feature ", "option ", "done", "alias",
					 "ls ", "cat-blob ", "get-mark ", NULL };
	int i;

	for (i = 0; c[i]; i++)
		if (!strncmp(l, c[i], strlen(c[i])))
			return 1;
	return 0;
}

static void filter_stream(struct migrator *m, struct reader *ex)
{
	struct strbuf line = STRBUF_INIT;
	int in_commit = 0, has_from = 0, injected = 0;
	char buf[65536];

	while (!reader_line(ex, &line)) {
		const char *l = line.buf, *v;

		if ((v = skip_prefix(l, "data "))) {
			unsigned long long left = strtoull(v, NULL, 10);

			fprintf(m->imp, "%s\n", l);
			while (left) {
				size_t chunk = left < sizeof(buf) ? (size_t)left : sizeof(buf);

				if (reader_bytes(ex, buf, chunk) != chunk)
					die("fast-export: unexpected end of stream");
				fwrite(buf, 1, chunk, m->imp);
				left -= chunk;
			}
			continue;
		}
		if (in_commit && is_commit_header(l)) {
			if (!strncmp(l, "from ", 5))
				has_from = 1;
			fprintf(m->imp, "%s\n", l);
			continue;
		}
		if (in_commit) {
			/* первая строка изменений файлов: корневому коммиту нужен .gitattributes */
			if (!injected) {
				injected = 1;
				if (!has_from)
					inject_attributes(m);
			}
			if (l[0] == 'M' && l[1] == ' ') {
				handle_modify(m, l);
				continue;
			}
			if (!*l) {
				in_commit = 0;
				fputc('\n', m->imp);
				continue;
			}
			if (!is_command(l)) {
				fprintf(m->imp, "%s\n", l);
				continue;
			}
			in_commit = 0;
		}
		if (!strncmp(l, "commit ", 7)) {
			in_commit = 1;
			has_from = 0;
			injected = 0;
		}
		fprintf(m->imp, "%s\n", l);
	}
	if (in_commit && !injected && !has_from)
		inject_attributes(m);
	sb_free(&line);
}

static void require_clean_worktree(void)
{
	struct strbuf sb = STRBUF_INIT;

	if (!git_toplevel())
		return;
	if (run_git(NULL, 0, &sb, 0, "status", "--porcelain", "--untracked-files=no", NULL))
		die("git status failed");
	if (sb.len)
		die("working tree has uncommitted changes; commit or stash them first");
	sb_free(&sb);
}

static int migrate_import(int argc, char **argv)
{
	const char **exp = xcalloc((size_t)argc + 8, sizeof(char *));
	const char *imp_argv[] = { "git", "fast-import", "--quiet", "--force", NULL };
	const char *cat_argv[] = { "git", "cat-file", "--batch", NULL };
	struct child export_c, import_c;
	struct reader *ex = xmalloc(sizeof(*ex));
	struct migrator m;
	char *ref = NULL;
	int i, n = 0, everything = 0, nrefs = 0, rc1, rc2, rc3;

	memset(&m, 0, sizeof(m));
	exp[n++] = "git";
	exp[n++] = "fast-export";
	exp[n++] = "--no-data";
	exp[n++] = "--signed-tags=strip";
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v;

		if ((v = skip_prefix(a, "--include=")))
			pathfilter_add(&m.f.inc, &m.f.ninc, v);
		else if ((!strcmp(a, "-I") || !strcmp(a, "--include")) && i + 1 < argc)
			pathfilter_add(&m.f.inc, &m.f.ninc, argv[++i]);
		else if ((v = skip_prefix(a, "--exclude=")))
			pathfilter_add(&m.f.exc, &m.f.nexc, v);
		else if ((!strcmp(a, "-X") || !strcmp(a, "--exclude")) && i + 1 < argc)
			pathfilter_add(&m.f.exc, &m.f.nexc, argv[++i]);
		else if (!strcmp(a, "--everything"))
			everything = 1;
		else if (!strcmp(a, "--yes") || !strcmp(a, "--verbose"))
			;
		else if (a[0] == '-')
			die("unknown option '%s'", a);
		else {
			exp[n++] = a;
			nrefs++;
		}
	}
	if (!m.f.ninc)
		die("usage: git lfs migrate import --include=<patterns> [--exclude=<patterns>] "
		    "[--everything | <ref>...]");
	if (everything && nrefs)
		die("--everything cannot be combined with refs");
	if (everything) {
		exp[n++] = "--branches";
		exp[n++] = "--tags";
	} else if (!nrefs) {
		ref = git_current_ref();
		if (!ref)
			die("HEAD is detached; specify refs or --everything");
		exp[n++] = ref;
	}
	exp[n] = NULL;
	require_clean_worktree();
	lfs_dir();

	start_cmd(&import_c, imp_argv, 0, CHILD_PIPE, CHILD_NULL);
	start_cmd(&export_c, exp, 0, CHILD_NULL, CHILD_PIPE);
	start_cmd(&m.catfile, cat_argv, 0, CHILD_PIPE, CHILD_PIPE);
	reader_init(&m.catrd, m.catfile.out);
	reader_init(ex, export_c.out);
	m.imp = fdopen(import_c.in, "w");
	if (!m.imp)
		die("fdopen: %s", strerror(errno));
	import_c.in = -1;

	filter_stream(&m, ex);

	if (fclose(m.imp))
		die("git fast-import: write failed: %s", strerror(errno));
	rc1 = finish_cmd(&export_c);
	rc2 = finish_cmd(&m.catfile);
	rc3 = finish_cmd(&import_c);
	if (rc1 || rc2 || rc3)
		die("migrate failed (fast-export %d, cat-file %d, fast-import %d)", rc1, rc2, rc3);

	/* рабочее дерево было чистым: приводим его и индекс к новой истории */
	if (git_toplevel() &&
	    run_git(NULL, 0, NULL, 0, "-C", git_toplevel(), "reset", "-q", "--hard", NULL))
		warnf("git reset --hard failed; run it manually");
	fprintf(stderr, "migrate: converted %zu blob(s) to LFS objects\n", m.converted);
	fprintf(stderr, "migrate: history was rewritten, push with --force\n");

	for (i = 0; (size_t)i < m.cap; i++)
		free(m.map[i].ptr);
	free(m.map);
	pathfilter_free(&m.f);
	free(ex);
	free(exp);
	free(ref);
	return 0;
}

struct ext_stat {
	char *key;
	long long bytes, above_bytes;
	size_t files, above_files;
};

static int cmp_ext(const void *a, const void *b)
{
	const struct ext_stat *x = a, *y = b;

	return x->above_bytes < y->above_bytes ? 1 : x->above_bytes > y->above_bytes ? -1 : 0;
}

/* migrate info: какие типы файлов в истории занимают больше всего места */
static int migrate_info(int argc, char **argv)
{
	const char **args = xcalloc((size_t)argc + 6, sizeof(char *));
	struct strbuf objs = STRBUF_INIT, res = STRBUF_INIT;
	struct ext_stat *st = NULL;
	char *line, *save = NULL, *ref = NULL;
	long long above = 0, total = 0;
	size_t nst = 0, i, top = 5;
	int n = 0, k, nrefs = 0;

	args[n++] = "rev-list";
	args[n++] = "--objects";
	for (k = 1; k < argc; k++) {
		const char *v;

		if ((v = skip_prefix(argv[k], "--above="))) {
			if ((above = parse_size(v)) < 0)
				die("bad size '%s'", v);
		} else if ((v = skip_prefix(argv[k], "--top="))) {
			top = (size_t)atol(v);
		} else if (!strcmp(argv[k], "--everything")) {
			args[n++] = "--branches";
			args[n++] = "--tags";
			nrefs++;
		} else if (argv[k][0] == '-') {
			die("unknown option '%s'", argv[k]);
		} else {
			args[n++] = argv[k];
			nrefs++;
		}
	}
	if (!nrefs) {
		ref = git_current_ref();
		args[n++] = ref ? ref : "HEAD";
	}
	args[n] = NULL;
	if (run_git_argv(NULL, 0, &objs, 0, args))
		die("git rev-list failed");
	if (run_git(objs.buf, objs.len, &res, 0, "cat-file",
		    "--batch-check=%(objecttype) %(objectsize) %(rest)", NULL))
		die("git cat-file failed");

	for (line = strtok_r((char *)sb_str(&res), "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *sp, *path, *base, *dot, *key;
		long long size;

		if (strncmp(line, "blob ", 5))
			continue;
		size = strtoll(line + 5, &sp, 10);
		path = *sp == ' ' ? sp + 1 : sp;
		if (!*path)
			continue;
		base = strrchr(path, '/');
		base = base ? base + 1 : path;
		dot = strrchr(base, '.');
		key = dot && dot != base ? dot : base;
		for (i = 0; i < nst; i++)
			if (!strcmp(st[i].key, key))
				break;
		if (i == nst) {
			st = xrealloc(st, (nst + 1) * sizeof(*st));
			memset(&st[nst], 0, sizeof(*st));
			st[nst].key = xstrdup(key);
			nst++;
		}
		st[i].bytes += size;
		st[i].files++;
		if (size >= above) {
			st[i].above_bytes += size;
			st[i].above_files++;
			total += size;
		}
	}
	qsort(st, nst, sizeof(*st), cmp_ext);
	for (i = 0; i < nst && i < top; i++) {
		char sz[32];

		if (!st[i].above_files)
			break;
		format_size(st[i].above_bytes, sz, sizeof(sz));
		printf("%s%-20s\t%10s\t%zu/%zu files\t%5.1f%%\n", st[i].key[0] == '.' ? "*" : "",
		       st[i].key, sz, st[i].above_files, st[i].files,
		       total ? 100.0 * (double)st[i].above_bytes / (double)total : 0.0);
	}
	for (i = 0; i < nst; i++)
		free(st[i].key);
	free(st);
	free(args);
	free(ref);
	sb_free(&objs);
	sb_free(&res);
	return 0;
}

int cmd_migrate(int argc, char **argv)
{
	if (argc < 2)
		die("usage: git lfs migrate (import | info) [<options>]");
	if (!strcmp(argv[1], "import"))
		return migrate_import(argc - 1, argv + 1);
	if (!strcmp(argv[1], "info"))
		return migrate_info(argc - 1, argv + 1);
	die("unsupported migrate mode '%s' (supported: import, info)", argv[1]);
}
