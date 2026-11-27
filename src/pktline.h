/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#ifndef LFS_PKTLINE_H
#define LFS_PKTLINE_H

#include <stddef.h>

/* pkt-line из протокола long-running filter (gitattributes(5), filter.<driver>.process) */
#define PKT_MAX_DATA 65516

#define PKT_EOF   (-1)
#define PKT_FLUSH 0
#define PKT_DATA  1

/* buf — не меньше PKT_MAX_DATA + 1 байт */
int pkt_read(int fd, char *buf, size_t *len);
/* Текстовый пакет: убирает '\n' в конце и ставит '\0' */
int pkt_read_text(int fd, char *buf);

void pkt_write(int fd, const void *data, size_t n);
void pkt_write_text(int fd, const char *s);
void pkt_write_data(int fd, const void *data, size_t n);
void pkt_flush(int fd);

#endif
