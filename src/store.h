#ifndef LFS_STORE_H
#define LFS_STORE_H

#include "pointer.h"
#include "sha256.h"

/*
 * clean: потоково считает sha256 и складывает содержимое в .git/lfs/objects.
 * Первые POINTER_MAX байт держим в памяти: если файл уже pointer (или пустой),
 * отдаём его git'у как есть и объект не создаём.
 */
struct cleaner {
	struct sha256 md;
	char head[POINTER_MAX];
	size_t headlen;
	int fd;
	char *tmp;
	long long size;
};

void cleaner_init(struct cleaner *c);
void cleaner_write(struct cleaner *c, const void *data, size_t n);
/* В out — то, что нужно отдать git'у */
void cleaner_finish(struct cleaner *c, struct strbuf *out);

typedef void (*write_fn)(void *ctx, const void *data, size_t n);

/* Скачивает объект, если его нет локально. 0 — объект на месте */
int smudge_fetch(const struct pointer *p, const char *path);
/* Пишет содержимое локального объекта */
int smudge_write(const struct pointer *p, write_fn wr, void *ctx);

/* GIT_LFS_SKIP_SMUDGE или --skip: отдавать pointer без скачивания */
int skip_smudge(void);
void set_skip_smudge(void);

#endif
