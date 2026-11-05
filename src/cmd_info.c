#include "commands.h"
#include "endpoint.h"
#include "git.h"
#include "run.h"
#include "scan.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

static void print_ptrs(const char *title, struct ptrlist *l)
{
	size_t i;

	printf("%s\n", title);
	for (i = 0; i < l->n; i++)
		printf("\t%s (LFS: %.7s)\n", l->v[i].path ? l->v[i].path : "?", l->v[i].oid);
	putchar('\n');
}

/* git diff-index --cached -z: ":<mode> <mode> <old> <new> <st>\0<path>\0" */
static void staged_pointers(struct ptrlist *out)
{
	struct strbuf res = STRBUF_INIT, lines = STRBUF_INIT;
	const char *p, *end;

	if (run_git(NULL, 0, NULL, RUN_QUIET, "rev-parse", "-q", "--verify", "HEAD", NULL)) {
		scan_index(out); /* ещё нет коммитов: всё в индексе — к коммиту */
		return;
	}
	if (run_git(NULL, 0, &res, 0, "diff-index", "--cached", "-z", "--no-renames", "HEAD", NULL))
		die("git diff-index failed");
	p = res.buf;
	end = res.buf + res.len;
	while (p && p < end) {
		const char *meta_end = memchr(p, '\0', (size_t)(end - p));
		const char *path, *path_end;
		char oldm[16], newm[16], olds[80], news[80], st[8];

		if (!meta_end)
			break;
		path = meta_end + 1;
		path_end = memchr(path, '\0', (size_t)(end - path));
		if (!path_end)
			break;
		if (sscanf(p, ":%15s %15s %79s %79s %7s", oldm, newm, olds, news, st) == 5 &&
		    (st[0] == 'A' || st[0] == 'M' || st[0] == 'T') && news[strspn(news, "0")]) {
			sb_addf(&lines, "%s ", news);
			sb_add(&lines, path, (size_t)(path_end - path));
			sb_addch(&lines, '\n');
		}
		p = path_end + 1;
	}
	scan_object_lines(lines.buf, lines.len, out);
	sb_free(&res);
	sb_free(&lines);
}

/* Изменённые, но не добавленные файлы с filter=lfs */
static void print_unstaged(void)
{
	struct strbuf names = STRBUF_INIT, attrs = STRBUF_INIT;
	const char *p, *end;
	int any = 0;

	if (run_git(NULL, 0, &names, 0, "diff-files", "-z", "--name-only", NULL) || !names.len)
		goto out;
	if (run_git(names.buf, names.len, &attrs, 0, "check-attr", "-z", "--stdin", "filter", NULL))
		goto out;
	/* "<path>\0filter\0<value>\0" */
	p = attrs.buf;
	end = attrs.buf + attrs.len;
	while (p < end) {
		const char *path = p, *attr, *val;

		attr = memchr(path, '\0', (size_t)(end - path));
		if (!attr)
			break;
		attr++;
		val = memchr(attr, '\0', (size_t)(end - attr));
		if (!val)
			break;
		val++;
		if (!strcmp(val, "lfs")) {
			if (!any)
				puts("Objects not staged for commit:");
			printf("\t%s (File)\n", path);
			any = 1;
		}
		p = val + strlen(val) + 1;
	}
	if (any)
		putchar('\n');
out:
	sb_free(&names);
	sb_free(&attrs);
}

int cmd_status(int argc, char **argv)
{
	struct ptrlist push = { NULL, 0, 0 }, commit = { NULL, 0, 0 };
	struct strbuf up = STRBUF_INIT, title = STRBUF_INIT;

	(void)argc;
	(void)argv;
	if (!git_toplevel())
		die("not in a git working tree");

	if (!run_git(NULL, 0, &up, RUN_QUIET, "rev-parse", "--abbrev-ref",
		     "--symbolic-full-name", "@{u}", NULL)) {
		const char *revs[4];

		sb_rtrim(&up);
		revs[0] = "HEAD";
		revs[1] = "--not";
		revs[2] = up.buf;
		revs[3] = NULL;
		scan_revs(revs, &push);
		sb_addf(&title, "Objects to be pushed to %s:", up.buf);
	} else if (!run_git(NULL, 0, NULL, RUN_QUIET, "rev-parse", "-q", "--verify", "HEAD", NULL)) {
		const char *revs[] = { "HEAD", "--not", "--remotes", NULL };

		scan_revs(revs, &push);
		sb_adds(&title, "Objects to be pushed:");
	}
	if (title.len) {
		ptrlist_dedup(&push);
		print_ptrs(title.buf, &push);
	}
	staged_pointers(&commit);
	print_ptrs("Objects to be committed:", &commit);
	print_unstaged();

	ptrlist_free(&push);
	ptrlist_free(&commit);
	sb_free(&up);
	sb_free(&title);
	return 0;
}

static void print_config(const char *key)
{
	char *v = git_config(key);

	printf("git config %s = \"%s\"\n", key, v ? v : "");
	free(v);
}

int cmd_env(int argc, char **argv)
{
	struct strbuf sb = STRBUF_INIT;
	char *inc, *exc;

	(void)argc;
	(void)argv;
	cmd_version(0, NULL);
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "version", NULL))
		fputs(sb_str(&sb), stdout);
	putchar('\n');

	if (git_dir()) {
		const char *remote = lfs_default_remote();
		char *ep = endpoint_describe(remote);

		printf("Endpoint=%s\n", ep);
		free(ep);
		sb_reset(&sb);
		if (!run_git(NULL, 0, &sb, RUN_QUIET, "remote", NULL)) {
			char *line, *save = NULL;

			for (line = strtok_r((char *)sb_str(&sb), "\n", &save); line;
			     line = strtok_r(NULL, "\n", &save)) {
				if (!strcmp(line, remote))
					continue;
				ep = endpoint_describe(line);
				printf("  %s Endpoint=%s\n", line, ep);
				free(ep);
			}
		}
		printf("LocalWorkingDir=%s\n", git_toplevel() ? git_toplevel() : "");
		printf("LocalGitDir=%s\n", git_dir());
		printf("LocalGitStorageDir=%s\n", git_dir());
		printf("LocalMediaDir=%s/objects\n", lfs_dir());
		printf("TempDir=%s/tmp\n", lfs_dir());
	}
	printf("ConcurrentTransfers=%d\n", lfs_concurrency());
	printf("SHA256=%s\n", sha256_impl());
	printf("BasicTransfersOnly=true\n");
	inc = lfs_config("lfs.fetchinclude");
	exc = lfs_config("lfs.fetchexclude");
	printf("FetchInclude=%s\n", inc ? inc : "");
	printf("FetchExclude=%s\n", exc ? exc : "");
	free(inc);
	free(exc);
	print_config("filter.lfs.process");
	print_config("filter.lfs.smudge");
	print_config("filter.lfs.clean");
	sb_free(&sb);
	return 0;
}
