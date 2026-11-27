/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#ifndef LFS_PATHFILTER_H
#define LFS_PATHFILTER_H

#include <stddef.h>

/*
 * Шаблоны путей как в lfs.fetchinclude/lfs.fetchexclude: список через запятую.
 * Шаблон без '/' сравнивается с каждым компонентом пути (*.iso, images),
 * шаблон с '/' — с путём от корня (assets/ *.psd, docs/).
 */
struct pathfilter {
	char **inc;
	size_t ninc;
	char **exc;
	size_t nexc;
};

/* NULL — взять из lfs.fetchinclude / lfs.fetchexclude */
void pathfilter_init(struct pathfilter *f, const char *include, const char *exclude);
void pathfilter_add(char ***list, size_t *n, const char *csv);
void pathfilter_free(struct pathfilter *f);
/* 1 — путь проходит фильтр */
int pathfilter_match(const struct pathfilter *f, const char *path);
int path_matches(const char *pattern, const char *path);

#endif
