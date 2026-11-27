/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#ifndef LFS_UTIL_H
#define LFS_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define NORETURN __attribute__((noreturn))
#define PRINTF(a, b) __attribute__((format(printf, a, b)))

void die(const char *fmt, ...) NORETURN PRINTF(1, 2);
void warnf(const char *fmt, ...) PRINTF(1, 2);
/* Отладочный вывод, включается через GIT_TRACE */
void trace(const char *fmt, ...) PRINTF(1, 2);
int env_true(const char *name);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Растущая строка, buf всегда заканчивается '\0' */
struct strbuf {
	char *buf;
	size_t len, cap;
};
#define STRBUF_INIT { NULL, 0, 0 }

void sb_grow(struct strbuf *sb, size_t extra);
void sb_add(struct strbuf *sb, const void *data, size_t n);
void sb_adds(struct strbuf *sb, const char *s);
void sb_addch(struct strbuf *sb, int c);
void sb_addf(struct strbuf *sb, const char *fmt, ...) PRINTF(2, 3);
void sb_reset(struct strbuf *sb);
void sb_rtrim(struct strbuf *sb);
void sb_free(struct strbuf *sb);
char *sb_detach(struct strbuf *sb);
const char *sb_str(struct strbuf *sb);

int write_all(int fd, const void *buf, size_t n);
ssize_t read_full(int fd, void *buf, size_t n);
int read_file(const char *path, struct strbuf *out);
int mkdir_p(const char *path);
long long file_size(const char *path);

int hexval(int c);
void hex_encode(const unsigned char *in, size_t n, char *out);
/* sha256 в hex: 64 символа [0-9a-f] */
int is_oid(const char *s);
const char *skip_prefix(const char *s, const char *prefix);
void format_size(long long n, char *buf, size_t len);
/* "10", "1.5mb", "200k" → байты; -1 при ошибке */
long long parse_size(const char *s);

#endif
