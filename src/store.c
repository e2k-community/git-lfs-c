#include "store.h"
#include "git.h"
#include "transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int skip_flag;


void cleaner_init(struct cleaner *c)
{
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	sha256_init(&c->md);
}

static void cleaner_spill(struct cleaner *c)
{
	c->fd = lfs_tmpfile(&c->tmp);
	if (write_all(c->fd, c->head, c->headlen))
		die("write %s: %s", c->tmp, strerror(errno));
}

void cleaner_write(struct cleaner *c, const void *data, size_t n)
{
	sha256_update(&c->md, data, n);
	c->size += (long long)n;
	if (c->fd < 0 && c->headlen + n <= POINTER_MAX) {
		memcpy(c->head + c->headlen, data, n);
		c->headlen += n;
		return;
	}
	if (c->fd < 0)
		cleaner_spill(c);
	if (write_all(c->fd, data, n))
		die("write %s: %s", c->tmp, strerror(errno));
}

void cleaner_finish(struct cleaner *c, struct strbuf *out)
{
	unsigned char dig[32];
	struct pointer p;

	sha256_final(&c->md, dig);

	if (c->fd < 0) {
		if (!c->headlen || !pointer_parse(c->head, c->headlen, &p)) {
			sb_add(out, c->head, c->headlen);
			return;
		}
		cleaner_spill(c);
	}
	if (close(c->fd))
		die("write %s: %s", c->tmp, strerror(errno));
	hex_encode(dig, 32, p.oid);
	p.size = c->size;
	if (lfs_object_ok(p.oid, p.size))
		unlink(c->tmp);
	else if (lfs_install_object(c->tmp, p.oid))
		die("cannot store object %s", p.oid);
	free(c->tmp);
	c->tmp = NULL;
	pointer_format(&p, out);
}

int skip_smudge(void)
{
	return skip_flag || env_true("GIT_LFS_SKIP_SMUDGE");
}

void set_skip_smudge(void)
{
	skip_flag = 1;
}

int smudge_fetch(const struct pointer *p, const char *path)
{
	struct lfs_obj o;

	if (lfs_object_ok(p->oid, p->size))
		return 0;
	memcpy(o.oid, p->oid, 65);
	o.size = p->size;
	o.path = (char *)path;
	return lfs_transfer(NULL, "download", &o, 1) ? -1 : 0;
}

int smudge_write(const struct pointer *p, write_fn wr, void *ctx)
{
	char *path = lfs_object_path(p->oid);
	char buf[65536];
	ssize_t r;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		warnf("cannot open %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	while ((r = read(fd, buf, sizeof(buf))) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			warnf("read %s: %s", path, strerror(errno));
			close(fd);
			free(path);
			return -1;
		}
		wr(ctx, buf, (size_t)r);
	}
	close(fd);
	free(path);
	return 0;
}
