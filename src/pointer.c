#include "pointer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int pointer_parse(const char *buf, size_t len, struct pointer *p)
{
	char tmp[POINTER_MAX + 1];
	char *line, *save = NULL;
	const char *v;
	int have_oid = 0, have_size = 0, first = 1;

	if (len == 0 || len > POINTER_MAX || memchr(buf, '\0', len))
		return -1;
	memcpy(tmp, buf, len);
	tmp[len] = '\0';

	for (line = strtok_r(tmp, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (first) {
			if (strcmp(line, LFS_SPEC))
				return -1;
			first = 0;
		} else if ((v = skip_prefix(line, "oid sha256:"))) {
			if (!is_oid(v))
				return -1;
			memcpy(p->oid, v, 65);
			have_oid = 1;
		} else if ((v = skip_prefix(line, "size "))) {
			char *e;
			long long n;

			if (*v < '0' || *v > '9')
				return -1;
			errno = 0;
			n = strtoll(v, &e, 10);
			if (*e || errno || n < 0)
				return -1;
			p->size = n;
			have_size = 1;
		} else if (!strchr(line, ' ')) {
			return -1;
		}
		/* прочие ключи "key value" (ext-* и т.п.) пропускаем */
	}
	return have_oid && have_size ? 0 : -1;
}

void pointer_format(const struct pointer *p, struct strbuf *out)
{
	sb_addf(out, LFS_SPEC "\noid sha256:%s\nsize %lld\n", p->oid, p->size);
}
