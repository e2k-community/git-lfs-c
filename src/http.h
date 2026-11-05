#ifndef LFS_HTTP_H
#define LFS_HTTP_H

#include "util.h"

#include <curl/curl.h>

struct http_req {
	const char *url;
	struct curl_slist *headers;
	const char *body;            /* POST: тело запроса */
	size_t bodylen;
	int upload_fd;               /* PUT: файл, -1 если нет */
	long long upload_size;
	/* Тело ответа 2xx уходит в sink; если sink нет или ответ не 2xx — в resp */
	size_t (*sink)(const void *data, size_t n, void *ctx);
	void *sink_ctx;
	struct strbuf *resp;
	const char *user, *pass;     /* basic auth */
	const char *progress;        /* подпись для прогресса в терминале */
	char err[CURL_ERROR_SIZE];

	int last_pct;
	int shown;
};

void http_req_init(struct http_req *r);
/* HTTP-код ответа или -1 при ошибке транспорта (текст в r->err) */
long http_do(struct http_req *r);
/* Читает настройки http.* из git config; вызывать до запуска потоков */
void http_init(void);

#endif
