/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "pktline.h"
#include "util.h"

#include <errno.h>
#include <string.h>

int pkt_read(int fd, char *buf, size_t *len)
{
	char hdr[4];
	ssize_t r = read_full(fd, hdr, 4);
	unsigned n = 0;
	int i;

	if (r == 0)
		return PKT_EOF;
	if (r != 4)
		die("pkt-line: unexpected end of input");
	for (i = 0; i < 4; i++) {
		int v = hexval((unsigned char)hdr[i]);

		if (v < 0)
			die("pkt-line: bad length header");
		n = n * 16 + (unsigned)v;
	}
	if (n == 0)
		return PKT_FLUSH;
	if (n < 4 || n > PKT_MAX_DATA + 4)
		die("pkt-line: bad length %u", n);
	n -= 4;
	if (read_full(fd, buf, n) != (ssize_t)n)
		die("pkt-line: short read");
	buf[n] = '\0';
	*len = n;
	return PKT_DATA;
}

int pkt_read_text(int fd, char *buf)
{
	size_t len = 0;
	int t = pkt_read(fd, buf, &len);

	if (t == PKT_DATA && len && buf[len - 1] == '\n')
		buf[len - 1] = '\0';
	return t;
}

void pkt_write(int fd, const void *data, size_t n)
{
	char hdr[5];

	if (n > PKT_MAX_DATA)
		die("pkt-line: packet too long");
	snprintf(hdr, sizeof(hdr), "%04zx", n + 4);
	if (write_all(fd, hdr, 4) || write_all(fd, data, n))
		die("pkt-line: write failed: %s", strerror(errno));
}

void pkt_write_text(int fd, const char *s)
{
	struct strbuf sb = STRBUF_INIT;

	sb_addf(&sb, "%s\n", s);
	pkt_write(fd, sb.buf, sb.len);
	sb_free(&sb);
}

void pkt_write_data(int fd, const void *data, size_t n)
{
	const char *p = data;

	while (n) {
		size_t chunk = n < PKT_MAX_DATA ? n : PKT_MAX_DATA;

		pkt_write(fd, p, chunk);
		p += chunk;
		n -= chunk;
	}
}

void pkt_flush(int fd)
{
	if (write_all(fd, "0000", 4))
		die("pkt-line: write failed: %s", strerror(errno));
}
