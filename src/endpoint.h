/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#ifndef LFS_ENDPOINT_H
#define LFS_ENDPOINT_H

#include "http.h"

#include <time.h>

#define LFS_MEDIA "application/vnd.git-lfs+json"

/*
 * Куда и с какой авторизацией ходить в LFS API.
 * SSH-remote: href и заголовки берём у git-lfs-authenticate на сервере,
 * если не вышло — откатываемся на HTTPS с git credential.
 */
struct endpoint {
	char *operation;   /* download | upload */
	char *href;        /* база LFS API, …/info/lfs */
	char **headers;    /* "Key: Value" от git-lfs-authenticate, NULL-терминирован */
	int has_auth_header;
	time_t expires;    /* 0 — бессрочно */

	int ssh;
	char *ssh_host;    /* [user@]host */
	char *ssh_port;
	char *ssh_path;
	int ssh_retried;

	char *user, *pass;
	int cred_filled;   /* user/pass получены от git credential fill */
	int cred_approved;
};

struct endpoint *endpoint_get(const char *remote, const char *operation);
/* Адрес LFS API без обращения к серверу (для git lfs env) */
char *endpoint_describe(const char *remote);

/*
 * Запрос к LFS API (JSON). body == NULL — GET. На 401 переспрашивает
 * учётные данные. Возвращает HTTP-код или -1 (текст ошибки в err).
 */
long endpoint_request(struct endpoint *e, const char *url, const char *body,
		      struct strbuf *resp, char *err, size_t errlen);

/* Добавляет авторизацию endpoint'а в запрос к Batch API */
struct curl_slist *endpoint_apply(struct endpoint *e, struct http_req *r,
				  struct curl_slist *h);
/* На 401: 1 — получили новые учётные данные, можно повторить */
int endpoint_handle_401(struct endpoint *e);
void endpoint_success(struct endpoint *e);
/* url на том же хосте, что и LFS API (можно отдавать basic-учётку) */
int endpoint_same_host(const struct endpoint *e, const char *url);

/* Разбор URL remote'а: scheme://[user[:pass]@]host[:port]/path или [user@]host:path */
struct lfs_url {
	char *scheme, *user, *pass, *host, *path;
};

int lfs_url_parse(const char *url, struct lfs_url *u);
void lfs_url_free(struct lfs_url *u);
/* https://host/path → https://host/path.git/info/lfs */
char *lfs_derive_href(const char *scheme, const char *host, const char *path);

#endif
