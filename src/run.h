#ifndef LFS_RUN_H
#define LFS_RUN_H

#include "util.h"

#include <sys/types.h>

/* stderr дочернего процесса в /dev/null */
#define RUN_QUIET 1

/*
 * Запускает argv[0] (через PATH). in/inlen — данные на stdin (NULL — пустой
 * stdin), stdout дочернего процесса собирается в out (NULL — выбрасывается;
 * наш stdout ребёнку никогда не отдаётся, в filter-process это канал git).
 * Возвращает код возврата или -1.
 */
int run_cmd(const char *const *argv, const char *in, size_t inlen,
	    struct strbuf *out, int flags);

/* То же для git, аргументы без "git", список заканчивается NULL */
int run_git(const char *in, size_t inlen, struct strbuf *out, int flags, ...);
int run_git_argv(const char *in, size_t inlen, struct strbuf *out, int flags,
		 const char *const *args);

/* Долгоживущий дочерний процесс с пайпами */
#define CHILD_PIPE (-1)   /* создать пайп */
#define CHILD_NULL (-2)   /* /dev/null */

struct child {
	pid_t pid;
	int in;   /* stdin ребёнка (пишем), -1 если не пайп */
	int out;  /* stdout ребёнка (читаем), -1 если не пайп */
};

/* in_fd/out_fd: CHILD_PIPE, CHILD_NULL или готовый дескриптор (будет закрыт) */
void start_cmd(struct child *c, const char *const *argv, int flags, int in_fd, int out_fd);
/* Закрывает пайпы и ждёт завершения, возвращает код возврата или -1 */
int finish_cmd(struct child *c);

/* Буферное чтение из дескриптора */
struct reader {
	int fd;
	size_t pos, len;
	char buf[65536];
};

void reader_init(struct reader *r, int fd);
/* Строка без '\n'; -1 на EOF */
int reader_line(struct reader *r, struct strbuf *line);
/* Ровно n байт (или меньше на EOF), возвращает прочитанное */
size_t reader_bytes(struct reader *r, void *dst, size_t n);

#endif
