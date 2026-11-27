/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "run.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 128

static void trace_argv(const char *const *argv)
{
	struct strbuf sb = STRBUF_INIT;
	int i;

	if (!env_true("GIT_TRACE"))
		return;
	for (i = 0; argv[i]; i++)
		sb_addf(&sb, "%s%s", i ? " " : "", argv[i]);
	trace("run: %s", sb_str(&sb));
	sb_free(&sb);
}

/* Пайпы с O_CLOEXEC: иначе они утекают в другие дочерние процессы и EOF не наступает */
static void xpipe(int fds[2])
{
	if (pipe2(fds, O_CLOEXEC) < 0)
		die("pipe: %s", strerror(errno));
}

static void exec_child(const char *const *argv, int flags, int in, int out)
{
	dup2(in, 0);
	dup2(out, 1);
	if (flags & RUN_QUIET) {
		int fd = open("/dev/null", O_WRONLY);

		if (fd >= 0)
			dup2(fd, 2);
	}
	execvp(argv[0], (char *const *)argv);
	fprintf(stderr, "git-lfs: cannot run %s: %s\n", argv[0], strerror(errno));
	_exit(127);
}

static int wait_child(pid_t pid)
{
	int status;

	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_cmd(const char *const *argv, const char *in, size_t inlen,
	    struct strbuf *out, int flags)
{
	int pin[2], pout[2];
	size_t off = 0;
	char buf[65536];
	pid_t pid;

	trace_argv(argv);
	xpipe(pin);
	xpipe(pout);
	pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid == 0)
		exec_child(argv, flags, pin[0], pout[1]);
	close(pin[0]);
	close(pout[1]);
	if (!in || !inlen) {
		close(pin[1]);
		pin[1] = -1;
	}

	/* Пишем stdin и читаем stdout одновременно, чтобы не словить взаимную блокировку */
	while (pin[1] >= 0 || pout[0] >= 0) {
		struct pollfd pfd[2];
		int nfd = 0, iw = -1, ir = -1;

		if (pin[1] >= 0) {
			pfd[nfd].fd = pin[1];
			pfd[nfd].events = POLLOUT;
			iw = nfd++;
		}
		if (pout[0] >= 0) {
			pfd[nfd].fd = pout[0];
			pfd[nfd].events = POLLIN;
			ir = nfd++;
		}
		if (poll(pfd, (nfds_t)nfd, -1) < 0) {
			if (errno == EINTR)
				continue;
			die("poll: %s", strerror(errno));
		}
		if (iw >= 0 && pfd[iw].revents) {
			/* не больше PIPE_BUF: такая запись после POLLOUT не блокируется */
			size_t chunk = inlen - off < PIPE_BUF ? inlen - off : PIPE_BUF;
			ssize_t w = write(pin[1], in + off, chunk);

			if (w < 0 && errno != EINTR && errno != EAGAIN) {
				close(pin[1]);
				pin[1] = -1;
			} else if (w > 0) {
				off += (size_t)w;
				if (off == inlen) {
					close(pin[1]);
					pin[1] = -1;
				}
			}
		}
		if (ir >= 0 && pfd[ir].revents) {
			ssize_t r = read(pout[0], buf, sizeof(buf));

			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0) {
				close(pout[0]);
				pout[0] = -1;
			} else if (out) {
				sb_add(out, buf, (size_t)r);
			}
		}
	}
	return wait_child(pid);
}

int run_git_argv(const char *in, size_t inlen, struct strbuf *out, int flags,
		 const char *const *args)
{
	const char *argv[MAX_ARGS + 2];
	int n = 0;

	argv[n++] = "git";
	while (*args) {
		if (n > MAX_ARGS)
			die("too many arguments for git");
		argv[n++] = *args++;
	}
	argv[n] = NULL;
	return run_cmd(argv, in, inlen, out, flags);
}

int run_git(const char *in, size_t inlen, struct strbuf *out, int flags, ...)
{
	const char *args[MAX_ARGS + 1];
	const char *a;
	va_list ap;
	int n = 0;

	va_start(ap, flags);
	while ((a = va_arg(ap, const char *))) {
		if (n >= MAX_ARGS)
			die("too many arguments for git");
		args[n++] = a;
	}
	va_end(ap);
	args[n] = NULL;
	return run_git_argv(in, inlen, out, flags, args);
}

static int child_end(int fd, int *mine, int write_end)
{
	int p[2];

	if (fd == CHILD_PIPE) {
		xpipe(p);
		*mine = write_end ? p[1] : p[0];
		return write_end ? p[0] : p[1];
	}
	*mine = -1;
	if (fd == CHILD_NULL) {
		fd = open("/dev/null", write_end ? O_RDONLY : O_WRONLY);
		if (fd < 0)
			die("/dev/null: %s", strerror(errno));
	}
	return fd;
}

void start_cmd(struct child *c, const char *const *argv, int flags, int in_fd, int out_fd)
{
	int cin, cout;

	trace_argv(argv);
	cin = child_end(in_fd, &c->in, 1);
	cout = child_end(out_fd, &c->out, 0);
	c->pid = fork();
	if (c->pid < 0)
		die("fork: %s", strerror(errno));
	if (c->pid == 0)
		exec_child(argv, flags, cin, cout);
	close(cin);
	close(cout);
}

int finish_cmd(struct child *c)
{
	if (c->in >= 0)
		close(c->in);
	if (c->out >= 0)
		close(c->out);
	c->in = c->out = -1;
	return wait_child(c->pid);
}

void reader_init(struct reader *r, int fd)
{
	r->fd = fd;
	r->pos = r->len = 0;
}

static int reader_fill(struct reader *r)
{
	ssize_t n;

	do {
		n = read(r->fd, r->buf, sizeof(r->buf));
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		die("read: %s", strerror(errno));
	r->pos = 0;
	r->len = (size_t)n;
	return n > 0;
}

int reader_line(struct reader *r, struct strbuf *line)
{
	sb_reset(line);
	for (;;) {
		char *nl;

		if (r->pos == r->len && !reader_fill(r))
			return line->len ? 0 : -1;
		nl = memchr(r->buf + r->pos, '\n', r->len - r->pos);
		if (nl) {
			sb_add(line, r->buf + r->pos, (size_t)(nl - (r->buf + r->pos)));
			r->pos = (size_t)(nl - r->buf) + 1;
			return 0;
		}
		sb_add(line, r->buf + r->pos, r->len - r->pos);
		r->pos = r->len;
	}
}

size_t reader_bytes(struct reader *r, void *dst, size_t n)
{
	char *d = dst;
	size_t got = 0;

	while (got < n) {
		size_t chunk;

		if (r->pos == r->len && !reader_fill(r))
			break;
		chunk = r->len - r->pos < n - got ? r->len - r->pos : n - got;
		memcpy(d + got, r->buf + r->pos, chunk);
		r->pos += chunk;
		got += chunk;
	}
	return got;
}
