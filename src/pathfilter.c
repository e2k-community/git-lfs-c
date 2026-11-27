/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "pathfilter.h"
#include "git.h"
#include "util.h"

#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

void pathfilter_add(char ***list, size_t *n, const char *csv)
{
	const char *p = csv;

	while (p && *p) {
		const char *end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		while (len && (*p == ' ' || *p == '\t')) {
			p++;
			len--;
		}
		while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
			len--;
		if (len) {
			*list = xrealloc(*list, (*n + 1) * sizeof(char *));
			(*list)[(*n)++] = xstrndup(p, len);
		}
		p = end ? end + 1 : NULL;
	}
}

void pathfilter_init(struct pathfilter *f, const char *include, const char *exclude)
{
	char *ci = include ? NULL : lfs_config("lfs.fetchinclude");
	char *ce = exclude ? NULL : lfs_config("lfs.fetchexclude");

	memset(f, 0, sizeof(*f));
	pathfilter_add(&f->inc, &f->ninc, include ? include : ci);
	pathfilter_add(&f->exc, &f->nexc, exclude ? exclude : ce);
	free(ci);
	free(ce);
}

void pathfilter_free(struct pathfilter *f)
{
	size_t i;

	for (i = 0; i < f->ninc; i++)
		free(f->inc[i]);
	for (i = 0; i < f->nexc; i++)
		free(f->exc[i]);
	free(f->inc);
	free(f->exc);
	memset(f, 0, sizeof(*f));
}

int path_matches(const char *pattern, const char *path)
{
	struct strbuf pat = STRBUF_INIT, sub = STRBUF_INIT;
	const char *p;
	int ret = 0;

	while (pattern[0] == '/' || (pattern[0] == '.' && pattern[1] == '/'))
		pattern += pattern[0] == '/' ? 1 : 2;
	sb_adds(&pat, pattern);
	while (pat.len && pat.buf[pat.len - 1] == '/')
		pat.buf[--pat.len] = '\0';
	if (!pat.len)
		goto out;

	if (strchr(pat.buf, '/')) {
		/* от корня: сам путь или всё внутри каталога */
		sb_addf(&sub, "%s/*", pat.buf);
		ret = !fnmatch(pat.buf, path, 0) || !fnmatch(sub.buf, path, 0);
		goto out;
	}
	/* без '/': любой компонент пути (имя файла или каталога) */
	for (p = path; *p; ) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);

		sb_reset(&sub);
		sb_add(&sub, p, len);
		if (!fnmatch(pat.buf, sub.buf, 0)) {
			ret = 1;
			break;
		}
		p += len + (slash ? 1 : 0);
	}
out:
	sb_free(&pat);
	sb_free(&sub);
	return ret;
}

int pathfilter_match(const struct pathfilter *f, const char *path)
{
	size_t i;
	int inc = !f->ninc;

	if (!path)
		return 1;
	for (i = 0; i < f->ninc && !inc; i++)
		inc = path_matches(f->inc[i], path);
	if (!inc)
		return 0;
	for (i = 0; i < f->nexc; i++)
		if (path_matches(f->exc[i], path))
			return 0;
	return 1;
}
