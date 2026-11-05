#ifndef LFS_COMMANDS_H
#define LFS_COMMANDS_H

#include "util.h"

/* argv[0] — имя подкоманды */
int cmd_clean(int argc, char **argv);
int cmd_smudge(int argc, char **argv);
int cmd_filter_process(int argc, char **argv);

int cmd_install(int argc, char **argv);
int cmd_uninstall(int argc, char **argv);
int cmd_track(int argc, char **argv);
int cmd_untrack(int argc, char **argv);
int cmd_version(int argc, char **argv);

int cmd_fetch(int argc, char **argv);
int cmd_checkout(int argc, char **argv);
int cmd_pull(int argc, char **argv);
int cmd_push(int argc, char **argv);
int cmd_ls_files(int argc, char **argv);
int cmd_pre_push(int argc, char **argv);
int cmd_prune(int argc, char **argv);

int cmd_status(int argc, char **argv);
int cmd_env(int argc, char **argv);

int cmd_lock(int argc, char **argv);
int cmd_unlock(int argc, char **argv);
int cmd_locks(int argc, char **argv);

int cmd_migrate(int argc, char **argv);

/* Ставит pre-push хук в текущий репозиторий. verbose — сообщать о конфликте */
int hooks_install(int verbose, int force);
/* Добавляет в содержимое .gitattributes строку для шаблона; 1 — добавлена */
int gitattributes_track(struct strbuf *attrs, const char *pattern);

#endif
