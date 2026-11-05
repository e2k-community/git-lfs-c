#include "commands.h"
#include "git.h"
#include "run.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char pre_push_hook[] =
	"#!/bin/sh\n"
	"command -v git-lfs >/dev/null 2>&1 || { printf >&2 \"\\n%s\\n\\n\" "
	"\"This repository is configured for Git LFS but 'git-lfs' was not found on your path. "
	"If you no longer wish to use Git LFS, remove this hook by deleting the 'pre-push' file "
	"in the hooks directory (set by 'core.hookspath'; usually '.git/hooks').\"; exit 2; }\n"
	"git lfs pre-push \"$@\"\n";

/* Хук считаем своим, если он вызывает git lfs pre-push (в т.ч. хук от гошного git-lfs) */
static int is_lfs_hook(const char *path)
{
	struct strbuf sb = STRBUF_INIT;
	int ours = !read_file(path, &sb) && strstr(sb_str(&sb), "git lfs pre-push");

	sb_free(&sb);
	return ours;
}

static char *hook_path(void)
{
	struct strbuf sb = STRBUF_INIT;

	if (run_git(NULL, 0, &sb, RUN_QUIET, "rev-parse", "--git-path", "hooks/pre-push", NULL)) {
		sb_free(&sb);
		return NULL;
	}
	sb_rtrim(&sb);
	return sb_detach(&sb);
}

int hooks_install(int verbose, int force)
{
	char *path = hook_path();
	char *slash;
	FILE *f;

	if (!path)
		return -1;
	if (file_size(path) >= 0 && !force) {
		if (!is_lfs_hook(path) && verbose)
			warnf("hook %s already exists and does not call git lfs pre-push; "
			      "add 'git lfs pre-push \"$@\"' to it or run 'git lfs install --force'", path);
		free(path);
		return 0;
	}
	slash = strrchr(path, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(path);
		*slash = '/';
	}
	f = fopen(path, "w");
	if (!f) {
		if (verbose)
			warnf("cannot write %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	fputs(pre_push_hook, f);
	fclose(f);
	chmod(path, 0755);
	free(path);
	return 0;
}

static const char *scope_arg(int argc, char **argv, int *skip_repo, int *force, int *skip_smudge)
{
	const char *scope = "--global";
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--local") || !strcmp(argv[i], "-l"))
			scope = "--local";
		else if (!strcmp(argv[i], "--system"))
			scope = "--system";
		else if (!strcmp(argv[i], "--worktree") || !strcmp(argv[i], "-w"))
			scope = "--worktree";
		else if (!strcmp(argv[i], "--global"))
			scope = "--global";
		else if (!strcmp(argv[i], "--skip-repo") && skip_repo)
			*skip_repo = 1;
		else if ((!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f")) && force)
			*force = 1;
		else if ((!strcmp(argv[i], "--skip-smudge") || !strcmp(argv[i], "-s")) && skip_smudge)
			*skip_smudge = 1;
		else
			die("unknown option '%s'", argv[i]);
	}
	if ((!strcmp(scope, "--local") || !strcmp(scope, "--worktree")) && !git_dir())
		die("%s requires a git repository", scope);
	return scope;
}

static void set_config(const char *scope, const char *key, const char *value)
{
	if (run_git(NULL, 0, NULL, 0, "config", scope, "--replace-all", key, value, NULL))
		die("cannot set %s", key);
}

int cmd_install(int argc, char **argv)
{
	int skip_repo = 0, force = 0, skip = 0;
	const char *scope = scope_arg(argc, argv, &skip_repo, &force, &skip);

	set_config(scope, "filter.lfs.clean", "git-lfs clean -- %f");
	set_config(scope, "filter.lfs.smudge",
		   skip ? "git-lfs smudge --skip -- %f" : "git-lfs smudge -- %f");
	set_config(scope, "filter.lfs.process",
		   skip ? "git-lfs filter-process --skip" : "git-lfs filter-process");
	set_config(scope, "filter.lfs.required", "true");
	if (!skip_repo && git_dir())
		hooks_install(1, force);
	puts("Git LFS initialized.");
	return 0;
}

int cmd_uninstall(int argc, char **argv)
{
	int skip_repo = 0;
	const char *scope = scope_arg(argc, argv, &skip_repo, NULL, NULL);
	char *path;

	run_git(NULL, 0, NULL, RUN_QUIET, "config", scope, "--remove-section", "filter.lfs", NULL);
	if (!skip_repo && git_dir() && (path = hook_path())) {
		if (is_lfs_hook(path))
			unlink(path);
		free(path);
	}
	puts("Git LFS configuration has been removed.");
	return 0;
}

/* .gitattributes в корне рабочего дерева */
static char *attributes_path(void)
{
	struct strbuf sb = STRBUF_INIT;
	const char *top = git_toplevel();

	if (!top)
		die("not in a git working tree");
	sb_addf(&sb, "%s/.gitattributes", top);
	return sb_detach(&sb);
}

static char *escape_pattern(const char *s)
{
	struct strbuf sb = STRBUF_INIT;

	for (; *s; s++) {
		if (*s == ' ')
			sb_adds(&sb, "[[:space:]]");
		else
			sb_addch(&sb, *s);
	}
	return sb_detach(&sb);
}

/* Строка вида "<pattern> ... filter=lfs ..." */
static int line_is_lfs(const char *line, size_t len, const char *pattern)
{
	char *l = xstrndup(line, len);
	char *sp = strpbrk(l, " \t");
	int ret;

	ret = sp && strstr(sp, "filter=lfs") &&
	      (!pattern || ((size_t)(sp - l) == strlen(pattern) && !strncmp(l, pattern, (size_t)(sp - l))));
	free(l);
	return ret;
}

int gitattributes_track(struct strbuf *attrs, const char *pattern)
{
	char *pat = escape_pattern(pattern);
	const char *p = sb_str(attrs);
	int found = 0;

	while (*p && !found) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);

		found = line_is_lfs(p, len, pat);
		p += len + (nl ? 1 : 0);
	}
	if (!found) {
		if (attrs->len && attrs->buf[attrs->len - 1] != '\n')
			sb_addch(attrs, '\n');
		sb_addf(attrs, "%s filter=lfs diff=lfs merge=lfs -text\n", pat);
	}
	free(pat);
	return !found;
}

int cmd_track(int argc, char **argv)
{
	struct strbuf attrs = STRBUF_INIT;
	char *path = attributes_path();
	int i, changed = 0;

	read_file(path, &attrs);
	if (argc < 2) {
		const char *p = sb_str(&attrs);

		puts("Listing tracked patterns");
		while (*p) {
			const char *nl = strchr(p, '\n');
			size_t len = nl ? (size_t)(nl - p) : strlen(p);

			if (line_is_lfs(p, len, NULL)) {
				const char *sp = strpbrk(p, " \t");

				printf("    %.*s (.gitattributes)\n", (int)(sp - p), p);
			}
			p += len + (nl ? 1 : 0);
		}
		goto out;
	}

	for (i = 1; i < argc; i++) {
		if (gitattributes_track(&attrs, argv[i])) {
			printf("Tracking \"%s\"\n", argv[i]);
			changed = 1;
		} else {
			printf("\"%s\" already supported\n", argv[i]);
		}
	}
	if (changed) {
		FILE *f = fopen(path, "w");

		if (!f || fwrite(attrs.buf, 1, attrs.len, f) != attrs.len || fclose(f))
			die("cannot write %s: %s", path, strerror(errno));
	}
out:
	sb_free(&attrs);
	free(path);
	return 0;
}

int cmd_untrack(int argc, char **argv)
{
	struct strbuf attrs = STRBUF_INIT, out = STRBUF_INIT;
	char *path = attributes_path();
	const char *p;
	int i;
	FILE *f;

	if (argc < 2)
		die("usage: git lfs untrack <pattern>...");
	if (read_file(path, &attrs))
		goto done;
	p = sb_str(&attrs);
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		int drop = 0;

		for (i = 1; i < argc && !drop; i++) {
			char *pat = escape_pattern(argv[i]);

			if (line_is_lfs(p, len, pat)) {
				printf("Untracking \"%s\"\n", argv[i]);
				drop = 1;
			}
			free(pat);
		}
		if (!drop) {
			sb_add(&out, p, len);
			sb_addch(&out, '\n');
		}
		p += len + (nl ? 1 : 0);
	}
	f = fopen(path, "w");
	if (!f || fwrite(sb_str(&out), 1, out.len, f) != out.len || fclose(f))
		die("cannot write %s: %s", path, strerror(errno));
done:
	sb_free(&attrs);
	sb_free(&out);
	free(path);
	return 0;
}

int cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	puts("git-lfs/" LFS_VERSION " (lfs-c; linux; C)");
	return 0;
}
