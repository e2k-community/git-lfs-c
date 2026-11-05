#include "endpoint.h"
#include "git.h"
#include "json.h"
#include "run.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

void lfs_url_free(struct lfs_url *u)
{
	free(u->scheme);
	free(u->user);
	free(u->pass);
	free(u->host);
	free(u->path);
	memset(u, 0, sizeof(*u));
}

static char *pct_decode(const char *s, size_t n)
{
	struct strbuf sb = STRBUF_INIT;
	size_t i;

	for (i = 0; i < n; i++) {
		if (s[i] == '%' && i + 2 < n &&
		    hexval((unsigned char)s[i + 1]) >= 0 && hexval((unsigned char)s[i + 2]) >= 0) {
			sb_addch(&sb, hexval((unsigned char)s[i + 1]) * 16 + hexval((unsigned char)s[i + 2]));
			i += 2;
		} else {
			sb_addch(&sb, s[i]);
		}
	}
	return sb_detach(&sb);
}

static const char *find_last(const char *s, const char *end, char c)
{
	const char *r = NULL;

	for (; s < end; s++)
		if (*s == c)
			r = s;
	return r;
}

int lfs_url_parse(const char *url, struct lfs_url *u)
{
	const char *p, *rest, *slash, *at, *colon;

	memset(u, 0, sizeof(*u));
	p = strstr(url, "://");
	if (p) {
		u->scheme = xstrndup(url, (size_t)(p - url));
		rest = p + 3;
		slash = strchr(rest, '/');
		if (!slash)
			slash = rest + strlen(rest);
		at = find_last(rest, slash, '@');
		if (at) {
			colon = memchr(rest, ':', (size_t)(at - rest));
			if (colon) {
				u->user = pct_decode(rest, (size_t)(colon - rest));
				u->pass = pct_decode(colon + 1, (size_t)(at - colon - 1));
			} else {
				u->user = pct_decode(rest, (size_t)(at - rest));
			}
			rest = at + 1;
		}
		u->host = xstrndup(rest, (size_t)(slash - rest));
		u->path = xstrdup(*slash ? slash : "/");
	} else {
		colon = strchr(url, ':');
		slash = strchr(url, '/');
		if (!colon || (slash && slash < colon))
			return -1; /* локальный путь */
		u->scheme = xstrdup("ssh");
		at = find_last(url, colon, '@');
		if (at) {
			u->user = xstrndup(url, (size_t)(at - url));
			rest = at + 1;
		} else {
			rest = url;
		}
		u->host = xstrndup(rest, (size_t)(colon - rest));
		u->path = xstrdup(colon + 1);
	}
	if (!*u->host) {
		lfs_url_free(u);
		return -1;
	}
	return 0;
}

static int is_ssh_scheme(const char *s)
{
	return !strcmp(s, "ssh") || !strcmp(s, "git+ssh") || !strcmp(s, "ssh+git");
}

char *lfs_derive_href(const char *scheme, const char *host, const char *path)
{
	struct strbuf sb = STRBUF_INIT;
	char *p = xstrdup(path);
	size_t n = strlen(p);

	while (n > 1 && p[n - 1] == '/')
		p[--n] = '\0';
	sb_addf(&sb, "%s://%s%s%s", scheme, host, p[0] == '/' ? "" : "/", p);
	if (n < 4 || strcmp(p + n - 4, ".git"))
		sb_adds(&sb, ".git");
	sb_adds(&sb, "/info/lfs");
	free(p);
	return sb_detach(&sb);
}

static char *strip_slashes(const char *scheme, const char *host, const char *path)
{
	struct strbuf sb = STRBUF_INIT;

	sb_addf(&sb, "%s://%s%s", scheme, host, path);
	while (sb.len > 1 && sb.buf[sb.len - 1] == '/')
		sb.buf[--sb.len] = '\0';
	return sb_detach(&sb);
}

static void sq_quote(struct strbuf *sb, const char *s)
{
	static const char safe[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./~@+";

	if (*s && !s[strspn(s, safe)]) {
		sb_adds(sb, s);
		return;
	}
	sb_addch(sb, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			sb_adds(sb, "'\\''");
		else
			sb_addch(sb, *s);
	}
	sb_addch(sb, '\'');
}

static void free_headers(struct endpoint *e)
{
	char **h;

	for (h = e->headers; h && *h; h++)
		free(*h);
	free(e->headers);
	e->headers = NULL;
	e->has_auth_header = 0;
}

static int ssh_authenticate(struct endpoint *e)
{
	struct strbuf remote_cmd = STRBUF_INIT, shcmd = STRBUF_INIT, out = STRBUF_INIT;
	const char *argv[12];
	const char *sshcmd = getenv("GIT_SSH_COMMAND");
	char *cfg = NULL;
	struct json *j = NULL;
	const struct json *hdr, *c;
	const char *href;
	long long expires_in;
	int n = 0, rc, ret = -1;
	size_t nh = 0;

	sb_adds(&remote_cmd, "git-lfs-authenticate ");
	sq_quote(&remote_cmd, e->ssh_path);
	sb_addf(&remote_cmd, " %s", e->operation);

	if (!sshcmd || !*sshcmd)
		sshcmd = cfg = git_config("core.sshCommand");
	if (sshcmd && *sshcmd) {
		/* как в git: команда через shell, аргументы дописываются в конец */
		sb_addf(&shcmd, "%s \"$@\"", sshcmd);
		argv[n++] = "sh";
		argv[n++] = "-c";
		argv[n++] = shcmd.buf;
		argv[n++] = "ssh";
	} else {
		const char *prog = getenv("GIT_SSH");

		argv[n++] = prog && *prog ? prog : "ssh";
	}
	if (e->ssh_port) {
		argv[n++] = "-p";
		argv[n++] = e->ssh_port;
	}
	argv[n++] = e->ssh_host;
	argv[n++] = remote_cmd.buf;
	argv[n] = NULL;

	rc = run_cmd(argv, NULL, 0, &out, 0);
	if (rc) {
		warnf("ssh %s %s: exit status %d", e->ssh_host, remote_cmd.buf, rc);
		goto out;
	}
	j = json_parse(out.buf, out.len);
	href = json_get_str(j, "href");
	if (!href) {
		warnf("git-lfs-authenticate: unexpected response");
		goto out;
	}

	free(e->href);
	e->href = xstrdup(href);
	while (e->href[0] && e->href[strlen(e->href) - 1] == '/')
		e->href[strlen(e->href) - 1] = '\0';
	free_headers(e);
	hdr = json_get(j, "header");
	for (c = hdr ? hdr->child : NULL; c; c = c->next)
		nh++;
	e->headers = xcalloc(nh + 1, sizeof(char *));
	nh = 0;
	for (c = hdr ? hdr->child : NULL; c; c = c->next) {
		struct strbuf h = STRBUF_INIT;

		if (c->type != J_STR)
			continue;
		if (!strcasecmp(c->key, "Authorization"))
			e->has_auth_header = 1;
		sb_addf(&h, "%s: %s", c->key, c->str);
		e->headers[nh++] = sb_detach(&h);
	}
	expires_in = json_get_int(j, "expires_in", 0);
	e->expires = expires_in > 0 ? time(NULL) + (time_t)expires_in - 5 : 0;
	trace("ssh: authenticated %s, href %s", e->operation, e->href);
	ret = 0;
out:
	json_free(j);
	free(cfg);
	sb_free(&remote_cmd);
	sb_free(&shcmd);
	sb_free(&out);
	return ret;
}

static int credential(struct endpoint *e, const char *action)
{
	struct strbuf in = STRBUF_INIT, out = STRBUF_INIT;
	struct lfs_url u;
	char *line, *save = NULL;
	int rc;

	if (lfs_url_parse(e->href, &u))
		return -1;
	sb_addf(&in, "protocol=%s\nhost=%s\n", u.scheme, u.host);
	if (u.path[0] && strcmp(u.path, "/"))
		sb_addf(&in, "path=%s\n", u.path + (u.path[0] == '/'));
	if (e->user)
		sb_addf(&in, "username=%s\n", e->user);
	if (e->pass && strcmp(action, "fill"))
		sb_addf(&in, "password=%s\n", e->pass);
	sb_addch(&in, '\n');
	lfs_url_free(&u);

	rc = run_git(in.buf, in.len, &out, 0, "credential", action, NULL);
	if (!rc && !strcmp(action, "fill")) {
		for (line = strtok_r((char *)sb_str(&out), "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			const char *v;

			if ((v = skip_prefix(line, "username="))) {
				free(e->user);
				e->user = xstrdup(v);
			} else if ((v = skip_prefix(line, "password="))) {
				free(e->pass);
				e->pass = xstrdup(v);
			}
		}
	}
	sb_free(&in);
	sb_free(&out);
	return rc;
}

static char *find_lfs_url(const char *remote, int *explicit)
{
	struct strbuf sb = STRBUF_INIT;
	char *v;

	*explicit = 1;
	if ((v = lfs_config("lfs.url")))
		return v;
	sb_addf(&sb, "remote.%s.lfsurl", remote);
	v = lfs_config(sb.buf);
	sb_reset(&sb);
	if (v)
		return v;
	*explicit = 0;
	/* ls-remote --get-url учитывает url.<base>.insteadOf и принимает сам URL */
	if (!run_git(NULL, 0, &sb, RUN_QUIET, "ls-remote", "--get-url", remote, NULL)) {
		sb_rtrim(&sb);
		if (sb.len)
			return sb_detach(&sb);
	}
	sb_free(&sb);
	return xstrdup(remote);
}

static int endpoint_init(struct endpoint *e, const char *url, int explicit)
{
	struct lfs_url u;

	if (lfs_url_parse(url, &u))
		return -1;
	if (is_ssh_scheme(u.scheme)) {
		char *colon = strchr(u.host, ':');
		struct strbuf h = STRBUF_INIT;

		e->ssh = 1;
		if (colon && !strchr(colon + 1, ':')) {
			e->ssh_port = xstrdup(colon + 1);
			*colon = '\0';
		}
		if (u.user)
			sb_addf(&h, "%s@", u.user);
		sb_adds(&h, u.host);
		e->ssh_host = sb_detach(&h);
		e->ssh_path = xstrdup(u.path);
		/* запасной вариант, если ssh не отдал href */
		e->href = lfs_derive_href("https", u.host, u.path);
	} else if (!strcmp(u.scheme, "https") || !strcmp(u.scheme, "http")) {
		e->href = explicit ? strip_slashes(u.scheme, u.host, u.path)
				   : lfs_derive_href(u.scheme, u.host, u.path);
		e->user = u.user;
		e->pass = u.pass;
		u.user = u.pass = NULL;
	} else {
		lfs_url_free(&u);
		return -1;
	}
	lfs_url_free(&u);
	return 0;
}

struct endpoint *endpoint_get(const char *remote, const char *operation)
{
	static struct endpoint *cache[2];
	int idx = !strcmp(operation, "upload");
	struct endpoint *e = cache[idx];
	int explicit;
	char *url;

	if (e) {
		if (e->ssh && e->expires && time(NULL) >= e->expires && ssh_authenticate(e))
			die("SSH re-authentication failed");
		return e;
	}
	e = xcalloc(1, sizeof(*e));
	e->operation = xstrdup(operation);
	url = find_lfs_url(remote, &explicit);
	if (endpoint_init(e, url, explicit))
		die("cannot use '%s' as LFS endpoint: set lfs.url (local paths are not supported)", url);
	free(url);
	if (e->ssh && ssh_authenticate(e)) {
		warnf("SSH authentication failed, falling back to %s", e->href);
		e->ssh = 0;
	}
	trace("endpoint %s: %s", operation, e->href);
	cache[idx] = e;
	return e;
}

char *endpoint_describe(const char *remote)
{
	struct endpoint e;
	struct strbuf sb = STRBUF_INIT;
	int explicit;
	char *url = find_lfs_url(remote, &explicit);

	memset(&e, 0, sizeof(e));
	if (endpoint_init(&e, url, explicit)) {
		sb_addf(&sb, "<unsupported: %s>", url);
	} else if (e.ssh) {
		sb_addf(&sb, "%s (auth=ssh %s%s%s %s)", e.href, e.ssh_host,
			e.ssh_port ? " -p " : "", e.ssh_port ? e.ssh_port : "", e.ssh_path);
	} else {
		sb_addf(&sb, "%s (auth=%s)", e.href, e.user && e.pass ? "basic" : "none");
	}
	free(url);
	free(e.href);
	free(e.ssh_host);
	free(e.ssh_port);
	free(e.ssh_path);
	free(e.user);
	free(e.pass);
	return sb_detach(&sb);
}

struct curl_slist *endpoint_apply(struct endpoint *e, struct http_req *r,
				  struct curl_slist *h)
{
	char **p;

	for (p = e->headers; p && *p; p++)
		h = curl_slist_append(h, *p);
	if (!e->has_auth_header) {
		r->user = e->user;
		r->pass = e->pass;
	}
	return h;
}

int endpoint_handle_401(struct endpoint *e)
{
	if (e->ssh) {
		/* токен мог протухнуть — один раз переспрашиваем сервер */
		if (e->ssh_retried++)
			return 0;
		return !ssh_authenticate(e);
	}
	if (e->cred_filled) {
		credential(e, "reject");
		return 0;
	}
	free(e->pass);
	e->pass = NULL;
	if (credential(e, "fill") || !e->pass)
		return 0;
	e->cred_filled = 1;
	return 1;
}

void endpoint_success(struct endpoint *e)
{
	if (e->cred_filled && !e->cred_approved) {
		credential(e, "approve");
		e->cred_approved = 1;
	}
}

long endpoint_request(struct endpoint *e, const char *url, const char *body,
		      struct strbuf *resp, char *err, size_t errlen)
{
	struct http_req r;
	long code = -1;
	int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		struct curl_slist *h = NULL;

		http_req_init(&r);
		h = curl_slist_append(h, "Accept: " LFS_MEDIA);
		if (body)
			h = curl_slist_append(h, "Content-Type: " LFS_MEDIA);
		h = endpoint_apply(e, &r, h);
		r.url = url;
		r.headers = h;
		r.body = body;
		r.bodylen = body ? strlen(body) : 0;
		r.resp = resp;
		sb_reset(resp);
		code = http_do(&r);
		curl_slist_free_all(h);
		if (code == 401 && endpoint_handle_401(e))
			continue;
		break;
	}
	if (code >= 200 && code < 300)
		endpoint_success(e);
	if (err && errlen)
		snprintf(err, errlen, "%s", code < 0 ? r.err : "");
	return code;
}

int endpoint_same_host(const struct endpoint *e, const char *url)
{
	struct lfs_url a, b;
	int same;

	if (lfs_url_parse(e->href, &a))
		return 0;
	if (lfs_url_parse(url, &b)) {
		lfs_url_free(&a);
		return 0;
	}
	same = !strcasecmp(a.scheme, b.scheme) && !strcasecmp(a.host, b.host);
	lfs_url_free(&a);
	lfs_url_free(&b);
	return same;
}
