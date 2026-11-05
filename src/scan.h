#ifndef LFS_SCAN_H
#define LFS_SCAN_H

#include "transfer.h"

#include <stddef.h>

struct ptrlist {
	struct lfs_obj *v;
	size_t n, cap;
};

void ptrlist_add(struct ptrlist *l, const char *oid, long long size, const char *path);
/* Сортирует по oid и убирает повторы */
void ptrlist_dedup(struct ptrlist *l);
void ptrlist_free(struct ptrlist *l);

/* Все LFS pointers в дереве (пути от корня репозитория) */
void scan_tree(const char *treeish, struct ptrlist *out);
/* LFS pointers в объектах, достижимых по аргументам rev-list */
void scan_revs(const char *const *revs, struct ptrlist *out);
/* LFS pointers в индексе */
void scan_index(struct ptrlist *out);
/* Строки "<git oid>[ <path>]\n" → pointers среди этих объектов */
void scan_object_lines(const char *lines, size_t len, struct ptrlist *out);
/* Объект с таким oid есть в отсортированном (ptrlist_dedup) списке */
int ptrlist_has(const struct ptrlist *l, const char *oid);

#endif
