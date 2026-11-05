#include "transfer.h"
#include "endpoint.h"
#include "git.h"
#include "json.h"
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define BATCH_SIZE 100

static const char *obj_name(const struct lfs_obj *o)
{
	return o->path ? o->path : o->oid;
}

struct hdr_flags {
	int auth;
	int ctype;
};

static struct curl_slist *headers_from_json(struct curl_slist *h, const struct json *hdr,
					    struct hdr_flags *f)
{
	const struct json *c;

	for (c = hdr ? hdr->child : NULL; c; c = c->next) {
		struct strbuf sb = STRBUF_INIT;

		if (c->type != J_STR)
			continue;
		if (!strcasecmp(c->key, "Authorization"))
			f->auth = 1;
		if (!strcasecmp(c->key, "Content-Type"))
			f->ctype = 1;
		sb_addf(&sb, "%s: %s", c->key, c->str);
		h = curl_slist_append(h, sb.buf);
		sb_free(&sb);
	}
	return h;
}

/* Basic-учётку отдаём только на хост LFS API и только если action не принёс свою */
static void action_auth(struct endpoint *e, struct http_req *r, const char *href,
			const struct hdr_flags *f)
{
	if (!f->auth && !e->has_auth_header && endpoint_same_host(e, href)) {
		r->user = e->user;
		r->pass = e->pass;
	}
}

static void warn_http(const char *what, const char *err, long code, struct strbuf *resp)
{
	struct json *j;
	const char *msg;

	if (code < 0) {
		warnf("%s: %s", what, err);
		return;
	}
	j = resp->len ? json_parse(resp->buf, resp->len) : NULL;
	msg = json_get_str(j, "message");
	warnf("%s: HTTP %ld%s%s", what, code, msg ? ": " : "", msg ? msg : "");
	json_free(j);
}

static struct json *batch_request(struct endpoint *e, const char *op,
				  const struct lfs_obj *objs, size_t n)
{
	struct strbuf body = STRBUF_INIT, url = STRBUF_INIT, resp = STRBUF_INIT;
	char err[CURL_ERROR_SIZE];
	struct json *j = NULL;
	char *ref = git_current_ref();
	long code;
	size_t i;

	sb_addf(&body, "{\"operation\":\"%s\",\"transfers\":[\"basic\"],"
		"\"hash_algo\":\"sha256\",", op);
	if (ref) {
		sb_adds(&body, "\"ref\":{\"name\":");
		json_add_str(&body, ref);
		sb_adds(&body, "},");
	}
	sb_adds(&body, "\"objects\":[");
	for (i = 0; i < n; i++)
		sb_addf(&body, "%s{\"oid\":\"%s\",\"size\":%lld}", i ? "," : "",
			objs[i].oid, objs[i].size);
	sb_adds(&body, "]}");
	sb_addf(&url, "%s/objects/batch", e->href);

	code = endpoint_request(e, url.buf, body.buf, &resp, err, sizeof(err));
	if (code != 200) {
		warn_http("batch request failed", err, code, &resp);
	} else {
		j = json_parse(resp.buf, resp.len);
		if (!j)
			warnf("batch request: invalid JSON in response");
	}
	free(ref);
	sb_free(&body);
	sb_free(&url);
	sb_free(&resp);
	return j;
}

struct dl_ctx {
	int fd;
	EVP_MD_CTX *md;
	long long got;
	int err;
};

static size_t dl_sink(const void *data, size_t n, void *ctx)
{
	struct dl_ctx *d = ctx;

	if (write_all(d->fd, data, n)) {
		d->err = errno;
		return 0;
	}
	EVP_DigestUpdate(d->md, data, n);
	d->got += (long long)n;
	return n;
}

static int download_one(struct endpoint *e, const struct lfs_obj *o,
			const struct json *action, int progress)
{
	const char *href = json_get_str(action, "href");
	struct strbuf resp = STRBUF_INIT;
	struct hdr_flags f = { 0, 0 };
	struct curl_slist *h;
	struct http_req r;
	struct dl_ctx d;
	unsigned char dig[EVP_MAX_MD_SIZE];
	char hex[65], *tmp = NULL;
	long code;
	int ret = -1, cerr;

	if (!href) {
		warnf("%s: server returned no download URL", obj_name(o));
		return -1;
	}
	d.fd = lfs_tmpfile(&tmp);
	d.md = EVP_MD_CTX_new();
	d.got = 0;
	d.err = 0;
	if (!d.md || !EVP_DigestInit_ex(d.md, lfs_sha256(), NULL))
		die("sha256 init failed");

	h = headers_from_json(NULL, json_get(action, "header"), &f);
	http_req_init(&r);
	r.url = href;
	r.headers = h;
	r.sink = dl_sink;
	r.sink_ctx = &d;
	r.resp = &resp;
	r.progress = progress ? obj_name(o) : NULL;
	action_auth(e, &r, href, &f);
	code = http_do(&r);
	curl_slist_free_all(h);
	EVP_DigestFinal_ex(d.md, dig, NULL);
	EVP_MD_CTX_free(d.md);
	hex_encode(dig, 32, hex);
	cerr = close(d.fd);

	if (d.err)
		warnf("%s: write failed: %s", obj_name(o), strerror(d.err));
	else if (code != 200)
		warn_http(obj_name(o), r.err, code, &resp);
	else if (cerr)
		warnf("%s: write failed: %s", obj_name(o), strerror(errno));
	else if (d.got != o->size || strcmp(hex, o->oid))
		warnf("%s: downloaded data does not match (got %lld bytes, sha256 %s)",
		      obj_name(o), d.got, hex);
	else
		ret = lfs_install_object(tmp, o->oid);
	if (ret)
		unlink(tmp);
	free(tmp);
	sb_free(&resp);
	return ret;
}

static int verify_one(struct endpoint *e, const struct lfs_obj *o, const struct json *action)
{
	const char *href = json_get_str(action, "href");
	struct strbuf body = STRBUF_INIT, resp = STRBUF_INIT;
	struct hdr_flags f = { 0, 1 };
	struct curl_slist *h;
	struct http_req r;
	long code;

	if (!href)
		return 0;
	sb_addf(&body, "{\"oid\":\"%s\",\"size\":%lld}", o->oid, o->size);
	h = curl_slist_append(NULL, "Accept: " LFS_MEDIA);
	h = curl_slist_append(h, "Content-Type: " LFS_MEDIA);
	h = headers_from_json(h, json_get(action, "header"), &f);
	http_req_init(&r);
	r.url = href;
	r.headers = h;
	r.body = body.buf;
	r.bodylen = body.len;
	r.resp = &resp;
	action_auth(e, &r, href, &f);
	code = http_do(&r);
	curl_slist_free_all(h);
	if (code != 200)
		warn_http("verify failed", r.err, code, &resp);
	sb_free(&body);
	sb_free(&resp);
	return code == 200 ? 0 : -1;
}

static int upload_one(struct endpoint *e, const struct lfs_obj *o,
		      const struct json *actions, int progress)
{
	const struct json *up = json_get(actions, "upload");
	const char *href = json_get_str(up, "href");
	struct strbuf resp = STRBUF_INIT;
	struct hdr_flags f = { 0, 0 };
	struct curl_slist *h;
	struct http_req r;
	char *path;
	long code;
	int fd;

	if (!up)
		return 0; /* объект уже есть на сервере */
	if (!href) {
		warnf("%s: server returned no upload URL", obj_name(o));
		return -1;
	}
	if (!lfs_object_ok(o->oid, o->size)) {
		warnf("%s: object %s is missing locally, cannot upload", obj_name(o), o->oid);
		return -1;
	}
	path = lfs_object_path(o->oid);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		warnf("cannot open %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	free(path);

	h = headers_from_json(NULL, json_get(up, "header"), &f);
	if (!f.ctype)
		h = curl_slist_append(h, "Content-Type: application/octet-stream");
	h = curl_slist_append(h, "Expect:");
	http_req_init(&r);
	r.url = href;
	r.headers = h;
	r.upload_fd = fd;
	r.upload_size = o->size;
	r.resp = &resp;
	r.progress = progress ? obj_name(o) : NULL;
	action_auth(e, &r, href, &f);
	code = http_do(&r);
	curl_slist_free_all(h);
	close(fd);
	if (code < 200 || code >= 300) {
		warn_http(obj_name(o), r.err, code, &resp);
		sb_free(&resp);
		return -1;
	}
	sb_free(&resp);
	return verify_one(e, o, json_get(actions, "verify"));
}

/* Общий прогресс по всем объектам одного lfs_transfer */
struct progress {
	const char *verb;
	size_t total, done;
	long long bytes;
	int tty;
	int shown;
};

struct task {
	struct lfs_obj *o;
	const struct json *actions; /* для download — сам action "download" */
	int failed;
};

struct pool {
	struct endpoint *e;
	int download;
	int per_object_progress;
	struct task *tasks;
	size_t n, next;
	struct progress *pr;
	pthread_mutex_t mu;
};

static void progress_step(struct progress *pr, const struct lfs_obj *o)
{
	char sz[32];

	pr->done++;
	pr->bytes += o->size;
	if (!pr->tty)
		return;
	format_size(pr->bytes, sz, sizeof(sz));
	flockfile(stderr);
	fprintf(stderr, "\r%s LFS objects: %3d%% (%zu/%zu), %s   ", pr->verb,
		(int)(pr->done * 100 / pr->total), pr->done, pr->total, sz);
	funlockfile(stderr);
	pr->shown = 1;
}

static void run_task(struct pool *p, struct task *t)
{
	int rc;

	if (p->download)
		rc = lfs_object_ok(t->o->oid, t->o->size) ? 0
			: download_one(p->e, t->o, t->actions, p->per_object_progress);
	else
		rc = upload_one(p->e, t->o, t->actions, p->per_object_progress);
	t->failed = rc != 0;
}

static void *worker(void *arg)
{
	struct pool *p = arg;

	for (;;) {
		struct task *t;

		pthread_mutex_lock(&p->mu);
		if (p->next >= p->n) {
			pthread_mutex_unlock(&p->mu);
			break;
		}
		t = &p->tasks[p->next++];
		pthread_mutex_unlock(&p->mu);

		run_task(p, t);

		pthread_mutex_lock(&p->mu);
		if (!p->per_object_progress)
			progress_step(p->pr, t->o);
		pthread_mutex_unlock(&p->mu);
	}
	return NULL;
}

static void run_pool(struct pool *p)
{
	size_t nthreads = (size_t)lfs_concurrency(), i;
	pthread_t *th;

	if (nthreads > p->n)
		nthreads = p->n;
	if (nthreads <= 1) {
		worker(p);
		return;
	}
	th = xcalloc(nthreads, sizeof(*th));
	for (i = 0; i < nthreads; i++)
		if (pthread_create(&th[i], NULL, worker, p))
			die("pthread_create failed");
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	free(th);
}

int lfs_transfer(const char *remote, const char *operation,
		 struct lfs_obj *objs, size_t n)
{
	int download = !strcmp(operation, "download");
	struct progress pr;
	struct endpoint *e;
	int failed = 0;
	size_t i, k;

	if (!n)
		return 0;
	/* всё ленивое кешируем до потоков */
	lfs_dir();
	http_init();
	lfs_sha256();
	lfs_concurrency();
	e = endpoint_get(remote ? remote : lfs_default_remote(), operation);

	memset(&pr, 0, sizeof(pr));
	pr.verb = download ? "Downloading" : "Uploading";
	pr.total = n;
	pr.tty = isatty(2);

	for (i = 0; i < n; i += BATCH_SIZE) {
		size_t m = n - i < BATCH_SIZE ? n - i : BATCH_SIZE;
		struct lfs_obj *chunk = objs + i;
		struct task tasks[BATCH_SIZE];
		char done[BATCH_SIZE] = { 0 };
		const struct json *arr, *it;
		const char *tr;
		struct json *resp;
		struct pool pool;
		size_t nt = 0;

		resp = batch_request(e, operation, chunk, m);
		if (!resp) {
			failed += (int)m;
			continue;
		}
		tr = json_get_str(resp, "transfer");
		if (tr && strcmp(tr, "basic"))
			warnf("server selected unsupported transfer '%s'", tr);
		arr = json_get(resp, "objects");
		for (it = arr && arr->type == J_ARR ? arr->child : NULL; it; it = it->next) {
			const char *oid = json_get_str(it, "oid");
			const struct json *err = json_get(it, "error");
			const struct json *actions = json_get(it, "actions");

			for (k = 0; k < m; k++)
				if (!done[k] && oid && !strcmp(chunk[k].oid, oid))
					break;
			if (k == m)
				continue;
			done[k] = 1;
			if (err) {
				const char *msg = json_get_str(err, "message");

				warnf("%s: %s (code %lld)", obj_name(&chunk[k]), msg ? msg : "error",
				      json_get_int(err, "code", 0));
				failed++;
				continue;
			}
			if (download && !json_get(actions, "download") &&
			    !lfs_object_ok(chunk[k].oid, chunk[k].size)) {
				warnf("%s: server returned no download action", obj_name(&chunk[k]));
				failed++;
				continue;
			}
			tasks[nt].o = &chunk[k];
			tasks[nt].actions = download ? json_get(actions, "download") : actions;
			tasks[nt].failed = 0;
			nt++;
		}
		for (k = 0; k < m; k++) {
			if (!done[k]) {
				warnf("%s: missing in server response", obj_name(&chunk[k]));
				failed++;
			}
		}

		memset(&pool, 0, sizeof(pool));
		pool.e = e;
		pool.download = download;
		/* при одном потоке показываем прогресс каждого файла, иначе общий */
		pool.per_object_progress = lfs_concurrency() == 1 || n == 1;
		pool.tasks = tasks;
		pool.n = nt;
		pool.pr = &pr;
		pthread_mutex_init(&pool.mu, NULL);
		run_pool(&pool);
		pthread_mutex_destroy(&pool.mu);
		for (k = 0; k < nt; k++)
			failed += tasks[k].failed;
		json_free(resp);
	}
	if (pr.shown)
		fputc('\n', stderr);
	return failed;
}
