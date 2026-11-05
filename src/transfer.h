#ifndef LFS_TRANSFER_H
#define LFS_TRANSFER_H

#include <stddef.h>

struct lfs_obj {
	char oid[65];
	long long size;
	char *path;   /* для сообщений, может быть NULL */
};

/*
 * Скачивает (operation "download") или выгружает ("upload") объекты через
 * Batch API с basic-транспортом. Возвращает число неудачных объектов.
 */
int lfs_transfer(const char *remote, const char *operation,
		 struct lfs_obj *objs, size_t n);

#endif
