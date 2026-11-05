#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Сообщение одной строкой: передача идёт в несколько потоков */
static void vreport(const char *prefix, const char *fmt, va_list ap)
{
	flockfile(stderr);
	fputs(prefix, stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	funlockfile(stderr);
}

void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vreport("git-lfs: ", fmt, ap);
	va_end(ap);
	exit(2);
}

void warnf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vreport("git-lfs: ", fmt, ap);
	va_end(ap);
}

int env_true(const char *name)
{
	const char *v = getenv(name);

	if (!v || !*v)
		return 0;
	return strcmp(v, "0") && strcasecmp(v, "false") && strcasecmp(v, "no");
}

void trace(const char *fmt, ...)
{
	va_list ap;

	if (!env_true("GIT_TRACE"))
		return;
	va_start(ap, fmt);
	vreport("trace git-lfs: ", fmt, ap);
	va_end(ap);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void sb_grow(struct strbuf *sb, size_t extra)
{
	size_t cap;

	if (sb->buf && sb->len + extra + 1 <= sb->cap)
		return;
	cap = sb->cap ? sb->cap : 64;
	while (cap < sb->len + extra + 1)
		cap *= 2;
	sb->buf = xrealloc(sb->buf, cap);
	sb->cap = cap;
	sb->buf[sb->len] = '\0';
}

void sb_add(struct strbuf *sb, const void *data, size_t n)
{
	sb_grow(sb, n);
	memcpy(sb->buf + sb->len, data, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
}

void sb_adds(struct strbuf *sb, const char *s)
{
	sb_add(sb, s, strlen(s));
}

void sb_addch(struct strbuf *sb, int c)
{
	char ch = (char)c;

	sb_add(sb, &ch, 1);
}

void sb_addf(struct strbuf *sb, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		die("vsnprintf failed");
	sb_grow(sb, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	sb->len += (size_t)n;
}

void sb_reset(struct strbuf *sb)
{
	sb->len = 0;
	if (sb->buf)
		sb->buf[0] = '\0';
}

void sb_rtrim(struct strbuf *sb)
{
	while (sb->len && isspace((unsigned char)sb->buf[sb->len - 1]))
		sb->len--;
	if (sb->buf)
		sb->buf[sb->len] = '\0';
}

void sb_free(struct strbuf *sb)
{
	free(sb->buf);
	sb->buf = NULL;
	sb->len = sb->cap = 0;
}

char *sb_detach(struct strbuf *sb)
{
	char *p;

	sb_grow(sb, 0);
	p = sb->buf;
	sb->buf = NULL;
	sb->len = sb->cap = 0;
	return p;
}

const char *sb_str(struct strbuf *sb)
{
	sb_grow(sb, 0);
	return sb->buf;
}

int write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

ssize_t read_full(int fd, void *buf, size_t n)
{
	char *p = buf;
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	return (ssize_t)got;
}

int read_file(const char *path, struct strbuf *out)
{
	char buf[65536];
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	while ((r = read(fd, buf, sizeof(buf))) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		sb_add(out, buf, (size_t)r);
	}
	close(fd);
	return 0;
}

int mkdir_p(const char *path)
{
	char *p = xstrdup(path);
	char *s;
	int r;

	for (s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = '\0';
		if (mkdir(p, 0777) && errno != EEXIST) {
			free(p);
			return -1;
		}
		*s = '/';
	}
	r = mkdir(p, 0777);
	free(p);
	return (r && errno != EEXIST) ? -1 : 0;
}

long long file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) || !S_ISREG(st.st_mode))
		return -1;
	return (long long)st.st_size;
}

int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

void hex_encode(const unsigned char *in, size_t n, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * n] = '\0';
}

int is_oid(const char *s)
{
	int i;

	for (i = 0; i < 64; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return 0;
	return s[64] == '\0';
}

const char *skip_prefix(const char *s, const char *prefix)
{
	size_t n = strlen(prefix);

	return strncmp(s, prefix, n) ? NULL : s + n;
}

long long parse_size(const char *s)
{
	char *e;
	double v;

	if (!s || !*s)
		return -1;
	v = strtod(s, &e);
	if (e == s || v < 0)
		return -1;
	while (*e == ' ')
		e++;
	if (!strcasecmp(e, "") || !strcasecmp(e, "b"))
		return (long long)v;
	if (!strcasecmp(e, "k") || !strcasecmp(e, "kb") || !strcasecmp(e, "kib"))
		return (long long)(v * 1024);
	if (!strcasecmp(e, "m") || !strcasecmp(e, "mb") || !strcasecmp(e, "mib"))
		return (long long)(v * 1024 * 1024);
	if (!strcasecmp(e, "g") || !strcasecmp(e, "gb") || !strcasecmp(e, "gib"))
		return (long long)(v * 1024 * 1024 * 1024);
	return -1;
}

void format_size(long long n, char *buf, size_t len)
{
	static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)n;
	int u = 0;

	while (v >= 1024 && u < 4) {
		v /= 1024;
		u++;
	}
	if (u == 0)
		snprintf(buf, len, "%lld B", n);
	else
		snprintf(buf, len, "%.1f %s", v, units[u]);
}
