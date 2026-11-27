/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#ifndef LFS_GIT_H
#define LFS_GIT_H

#include "util.h"

/* Абсолютный путь к общему .git (для worktree — основной), NULL вне репозитория */
const char *git_dir(void);
/* Корень рабочего дерева, NULL для bare или вне репозитория */
const char *git_toplevel(void);

/* Значение из git config или NULL (строку освобождает вызывающий) */
char *git_config(const char *key);
int git_config_bool(const char *key, int def);
/* git config, а если там нет — .lfsconfig из корня репозитория или HEAD */
char *lfs_config(const char *key);

/* Remote по умолчанию: remote текущей ветки, единственный remote или origin */
const char *lfs_default_remote(void);
/* lfs.concurrenttransfers, по умолчанию 8 */
int lfs_concurrency(void);
/* Полное имя текущей ветки (refs/heads/…) или NULL */
char *git_current_ref(void);

/* <git dir>/lfs, умирает вне репозитория */
const char *lfs_dir(void);
char *lfs_object_path(const char *oid);
/* Объект есть локально и нужного размера */
int lfs_object_ok(const char *oid, long long size);
/* Временный файл в <git dir>/lfs/tmp (та же ФС, что и objects) */
int lfs_tmpfile(char **path);
/* Переносит временный файл на место объекта */
int lfs_install_object(const char *tmp, const char *oid);

#endif
