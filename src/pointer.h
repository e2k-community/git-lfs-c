#ifndef LFS_POINTER_H
#define LFS_POINTER_H

#include "util.h"

/* Файлы больше этого размера pointer'ом не считаются (так же делает git-lfs) */
#define POINTER_MAX 1024

#define LFS_SPEC "version https://git-lfs.github.com/spec/v1"

struct pointer {
	char oid[65];
	long long size;
};

/* 0 — это pointer, -1 — обычное содержимое */
int pointer_parse(const char *buf, size_t len, struct pointer *p);
void pointer_format(const struct pointer *p, struct strbuf *out);

#endif
