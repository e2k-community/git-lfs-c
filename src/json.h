#ifndef LFS_JSON_H
#define LFS_JSON_H

#include "util.h"

/* Минимальный JSON: ровно то, что нужно для Batch API и git-lfs-authenticate */
enum json_type { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct json {
	enum json_type type;
	char *key;          /* имя поля, если узел — элемент объекта */
	char *str;          /* J_STR */
	long long num;      /* J_NUM, J_BOOL */
	struct json *child; /* J_ARR, J_OBJ */
	struct json *next;
};

struct json *json_parse(const char *s, size_t len);
void json_free(struct json *j);

const struct json *json_get(const struct json *obj, const char *key);
const char *json_get_str(const struct json *obj, const char *key);
long long json_get_int(const struct json *obj, const char *key, long long def);

/* Дописывает строку в кавычках с экранированием */
void json_add_str(struct strbuf *sb, const char *s);
/* Сериализует узел (без имени поля) */
void json_dump(struct strbuf *sb, const struct json *j);

#endif
