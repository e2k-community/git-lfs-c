/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#ifndef LFS_SHA256_H
#define LFS_SHA256_H

#include <stddef.h>
#include <stdint.h>

#include <openssl/evp.h>

/*
 * Потоковый SHA-256. Реализация выбирается при сборке:
 *  - LFS_BUILTIN_SHA256 — своя (sha256.c). На e2k она примерно вдвое быстрее
 *    OpenSSL, у которого для этой архитектуры только generic C;
 *  - иначе OpenSSL: на x86/ARM он использует аппаратные инструкции.
 */

/* Своя реализация: собирается всегда, чтобы её можно было тестировать */
struct sha256_builtin {
	uint32_t st[8];
	unsigned char buf[64];
	size_t buflen;
	uint64_t total;
};

void sha256_builtin_init(struct sha256_builtin *c);
void sha256_builtin_update(struct sha256_builtin *c, const void *data, size_t n);
void sha256_builtin_final(struct sha256_builtin *c, unsigned char out[32]);

struct sha256 {
#ifdef LFS_BUILTIN_SHA256
	struct sha256_builtin b;
#else
	EVP_MD_CTX *md;
#endif
};

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t n);
/* Освобождает контекст; после final нужен новый init */
void sha256_final(struct sha256 *c, unsigned char out[32]);

/* "builtin" или "openssl" — для git lfs env */
const char *sha256_impl(void);
/* Ленивую инициализацию (поиск алгоритма в OpenSSL 3) — до запуска потоков */
void sha256_prepare(void);

#endif
