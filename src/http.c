#include "http.h"
#include "git.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_RESP (64 << 20)

struct http_opts {
	int ssl_verify;
	char *cainfo;
	char *capath;
	char *proxy;
	int verbose;
};

static struct http_opts o;
static pthread_once_t opts_once = PTHREAD_ONCE_INIT;

static void load_opts(void)
{
	const char *v;

	o.ssl_verify = env_true("GIT_SSL_NO_VERIFY") ? 0 : git_config_bool("http.sslVerify", 1);
	v = getenv("GIT_SSL_CAINFO");
	o.cainfo = v && *v ? xstrdup(v) : git_config("http.sslCAInfo");
	v = getenv("GIT_SSL_CAPATH");
	o.capath = v && *v ? xstrdup(v) : git_config("http.sslCAPath");
	o.proxy = git_config("http.proxy");
	o.verbose = env_true("GIT_CURL_VERBOSE");
}

static const struct http_opts *http_opts(void)
{
	pthread_once(&opts_once, load_opts);
	return &o;
}

void http_init(void)
{
	http_opts();
}

static CURL *handle(void)
{
	/* свой handle на поток: curl_easy_reset сохраняет пул соединений */
	static __thread CURL *c;

	if (!c && !(c = curl_easy_init()))
		die("curl_easy_init failed");
	return c;
}

void http_req_init(struct http_req *r)
{
	memset(r, 0, sizeof(*r));
	r->upload_fd = -1;
	r->last_pct = -1;
}

static long response_code(void)
{
	long code = 0;

	curl_easy_getinfo(handle(), CURLINFO_RESPONSE_CODE, &code);
	return code;
}

static size_t on_data(char *ptr, size_t size, size_t nmemb, void *ud)
{
	struct http_req *r = ud;
	size_t n = size * nmemb;
	long code = response_code();

	if (r->sink && code >= 200 && code < 300)
		return r->sink(ptr, n, r->sink_ctx);
	if (r->resp) {
		if (r->resp->len + n > MAX_RESP)
			return 0;
		sb_add(r->resp, ptr, n);
	}
	return n;
}

static size_t on_read(char *ptr, size_t size, size_t nmemb, void *ud)
{
	struct http_req *r = ud;
	ssize_t n;

	do {
		n = read(r->upload_fd, ptr, size * nmemb);
	} while (n < 0 && errno == EINTR);
	return n < 0 ? CURL_READFUNC_ABORT : (size_t)n;
}

static int on_seek(void *ud, curl_off_t off, int origin)
{
	struct http_req *r = ud;

	return lseek(r->upload_fd, (off_t)off, origin) < 0 ? CURL_SEEKFUNC_FAIL : CURL_SEEKFUNC_OK;
}

static int on_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow,
		       curl_off_t ultotal, curl_off_t ulnow)
{
	struct http_req *r = ud;
	curl_off_t total = r->upload_fd >= 0 ? ultotal : dltotal;
	curl_off_t now = r->upload_fd >= 0 ? ulnow : dlnow;
	char a[32], b[32];
	int pct;

	if (total <= 0)
		return 0;
	pct = (int)(now * 100 / total);
	if (pct == r->last_pct)
		return 0;
	r->last_pct = pct;
	r->shown = 1;
	format_size((long long)now, a, sizeof(a));
	format_size((long long)total, b, sizeof(b));
	fprintf(stderr, "\r%s %s: %3d%% (%s / %s)   ",
		r->upload_fd >= 0 ? "Uploading" : "Downloading", r->progress, pct, a, b);
	return 0;
}

long http_do(struct http_req *r)
{
	const struct http_opts *o = http_opts();
	CURL *c = handle();
	CURLcode res;
	long code;

	curl_easy_reset(c);
	r->err[0] = '\0';
	r->last_pct = -1;
	r->shown = 0;

	curl_easy_setopt(c, CURLOPT_URL, r->url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "git-lfs/" LFS_VERSION " (lfs-c)");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, r->headers);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
	/* соединение считаем зависшим, если минуту не идут данные */
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, r->err);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
	if (o->verbose)
		curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
	if (!o->ssl_verify) {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	if (o->cainfo)
		curl_easy_setopt(c, CURLOPT_CAINFO, o->cainfo);
	if (o->capath)
		curl_easy_setopt(c, CURLOPT_CAPATH, o->capath);
	if (o->proxy)
		curl_easy_setopt(c, CURLOPT_PROXY, o->proxy);

	if (r->body) {
		curl_easy_setopt(c, CURLOPT_POST, 1L);
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)r->bodylen);
	} else if (r->upload_fd >= 0) {
		curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(c, CURLOPT_READFUNCTION, on_read);
		curl_easy_setopt(c, CURLOPT_READDATA, r);
		curl_easy_setopt(c, CURLOPT_SEEKFUNCTION, on_seek);
		curl_easy_setopt(c, CURLOPT_SEEKDATA, r);
		curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)r->upload_size);
	}
	if (r->user && r->pass) {
		curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
		curl_easy_setopt(c, CURLOPT_USERNAME, r->user);
		curl_easy_setopt(c, CURLOPT_PASSWORD, r->pass);
	}
	if (r->progress && isatty(2)) {
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, r);
	}

	trace("http: %s %s", r->body ? "POST" : r->upload_fd >= 0 ? "PUT" : "GET", r->url);
	res = curl_easy_perform(c);
	if (r->shown)
		fputc('\n', stderr);
	if (res != CURLE_OK) {
		if (!r->err[0])
			snprintf(r->err, sizeof(r->err), "%s", curl_easy_strerror(res));
		return -1;
	}
	code = response_code();
	trace("http: status %ld", code);
	return code;
}
