/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "commands.h"
#include "endpoint.h"
#include "git.h"
#include "json.h"
#include "run.h"

#include <stdlib.h>
#include <string.h>

/* LFS File Locking API: https://github.com/git-lfs/git-lfs/blob/main/docs/api/locking.md */

struct lock_opts {
	const char *remote;
	const char *id;
	const char *path;
	int force, json, verify;
	long limit;
};

static void parse_lock_opts(int argc, char **argv, struct lock_opts *o)
{
	int i;

	memset(o, 0, sizeof(*o));
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v;

		if ((v = skip_prefix(a, "--remote=")))
			o->remote = v;
		else if ((!strcmp(a, "-r") || !strcmp(a, "--remote")) && i + 1 < argc)
			o->remote = argv[++i];
		else if ((v = skip_prefix(a, "--id=")))
			o->id = v;
		else if ((!strcmp(a, "-i") || !strcmp(a, "--id")) && i + 1 < argc)
			o->id = argv[++i];
		else if ((v = skip_prefix(a, "--path=")))
			o->path = v;
		else if ((!strcmp(a, "-p") || !strcmp(a, "--path")) && i + 1 < argc)
			o->path = argv[++i];
		else if ((v = skip_prefix(a, "--limit=")))
			o->limit = atol(v);
		else if ((!strcmp(a, "-l") || !strcmp(a, "--limit")) && i + 1 < argc)
			o->limit = atol(argv[++i]);
		else if (!strcmp(a, "--force") || !strcmp(a, "-f"))
			o->force = 1;
		else if (!strcmp(a, "--json"))
			o->json = 1;
		else if (!strcmp(a, "--verify"))
			o->verify = 1;
		else if (a[0] == '-')
			die("unknown option '%s'", a);
		else if (!o->path)
			o->path = a;
		else
			die("unexpected argument '%s'", a);
	}
}

/* Путь относительно корня репозитория, без "." и ".." */
static char *repo_path(const char *arg)
{
	struct strbuf joined = STRBUF_INIT, out = STRBUF_INIT, prefix = STRBUF_INIT;
	const char *top = git_toplevel();
	char **parts = NULL, *tok, *save = NULL;
	size_t n = 0, i;

	if (!top)
		die("not in a git working tree");
	if (arg[0] == '/') {
		size_t tl = strlen(top);

		if (strncmp(arg, top, tl) || arg[tl] != '/')
			die("'%s' is outside the repository", arg);
		sb_adds(&joined, arg + tl + 1);
	} else {
		run_git(NULL, 0, &prefix, RUN_QUIET, "rev-parse", "--show-prefix", NULL);
		sb_rtrim(&prefix);
		sb_addf(&joined, "%s%s", sb_str(&prefix), arg);
	}
	for (tok = strtok_r(joined.buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (!strcmp(tok, "."))
			continue;
		if (!strcmp(tok, "..")) {
			if (!n)
				die("'%s' is outside the repository", arg);
			n--;
			continue;
		}
		parts = xrealloc(parts, (n + 1) * sizeof(char *));
		parts[n++] = tok;
	}
	if (!n)
		die("invalid path '%s'", arg);
	for (i = 0; i < n; i++)
		sb_addf(&out, "%s%s", i ? "/" : "", parts[i]);
	free(parts);
	sb_free(&joined);
	sb_free(&prefix);
	return sb_detach(&out);
}

static void url_escape(struct strbuf *sb, const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
			sb_addch(sb, c);
		else
			sb_addf(sb, "%%%02X", c);
	}
}

static void add_ref(struct strbuf *body)
{
	char *ref = git_current_ref();

	if (!ref)
		return;
	sb_adds(body, "\"ref\":{\"name\":");
	json_add_str(body, ref);
	sb_adds(body, "}");
	free(ref);
}

static struct endpoint *lock_endpoint(const struct lock_opts *o)
{
	return endpoint_get(o->remote ? o->remote : lfs_default_remote(), "upload");
}

/* Запрос к /locks…; на ошибку печатает сообщение и возвращает NULL */
static struct json *lock_api(struct endpoint *e, const char *path, const char *body,
			     long *code_out, int quiet_codes)
{
	struct strbuf url = STRBUF_INIT, resp = STRBUF_INIT;
	char err[CURL_ERROR_SIZE];
	struct json *j;
	long code;

	sb_addf(&url, "%s/locks%s", e->href, path);
	code = endpoint_request(e, url.buf, body, &resp, err, sizeof(err));
	if (code_out)
		*code_out = code;
	j = resp.len ? json_parse(resp.buf, resp.len) : NULL;
	if (code < 0) {
		warnf("locking API: %s", err);
	} else if ((code < 200 || code >= 300) && code != quiet_codes) {
		const char *msg = json_get_str(j, "message");

		warnf("locking API: HTTP %ld%s%s", code, msg ? ": " : "", msg ? msg : "");
	}
	sb_free(&url);
	sb_free(&resp);
	if (code < 200 || code >= 300) {
		if (code != quiet_codes) {
			json_free(j);
			return NULL;
		}
	}
	return j;
}

static const char *lock_owner(const struct json *lock)
{
	const char *name = json_get_str(json_get(lock, "owner"), "name");

	return name ? name : "";
}

static void print_json(const struct json *j)
{
	struct strbuf sb = STRBUF_INIT;

	json_dump(&sb, j);
	puts(sb_str(&sb));
	sb_free(&sb);
}

int cmd_lock(int argc, char **argv)
{
	struct strbuf body = STRBUF_INIT;
	struct lock_opts o;
	struct endpoint *e;
	struct json *j;
	char *path;
	long code;
	int ret = 0;

	parse_lock_opts(argc, argv, &o);
	if (!o.path)
		die("usage: git lfs lock [--remote=<remote>] [--json] <path>");
	path = repo_path(o.path);
	e = lock_endpoint(&o);

	sb_adds(&body, "{\"path\":");
	json_add_str(&body, path);
	sb_addch(&body, ',');
	add_ref(&body);
	if (body.buf[body.len - 1] == ',')
		body.buf[--body.len] = '\0';
	sb_addch(&body, '}');

	j = lock_api(e, "", body.buf, &code, 409);
	if (!j) {
		ret = 2;
	} else if (code == 409) {
		const struct json *lock = json_get(j, "lock");

		warnf("lock exists: %s is locked by %s (ID: %s)", path, lock_owner(lock),
		      json_get_str(lock, "id") ? json_get_str(lock, "id") : "?");
		ret = 2;
	} else if (o.json) {
		print_json(json_get(j, "lock"));
	} else {
		printf("Locked %s\n", path);
	}
	json_free(j);
	sb_free(&body);
	free(path);
	return ret;
}

/* id блокировки для пути или NULL */
static char *find_lock_id(struct endpoint *e, const char *path)
{
	struct strbuf q = STRBUF_INIT;
	const struct json *arr;
	struct json *j;
	char *id = NULL;

	sb_adds(&q, "?path=");
	url_escape(&q, path);
	j = lock_api(e, q.buf, NULL, NULL, 0);
	arr = json_get(j, "locks");
	if (arr && arr->type == J_ARR && arr->child && json_get_str(arr->child, "id"))
		id = xstrdup(json_get_str(arr->child, "id"));
	json_free(j);
	sb_free(&q);
	return id;
}

int cmd_unlock(int argc, char **argv)
{
	struct strbuf body = STRBUF_INIT, url = STRBUF_INIT;
	struct lock_opts o;
	struct endpoint *e;
	struct json *j;
	char *path = NULL, *id;
	int ret = 0;

	parse_lock_opts(argc, argv, &o);
	if (!o.path && !o.id)
		die("usage: git lfs unlock [--force] [--remote=<remote>] (<path> | --id=<id>)");
	e = lock_endpoint(&o);
	if (o.id) {
		id = xstrdup(o.id);
	} else {
		path = repo_path(o.path);
		id = find_lock_id(e, path);
		if (!id)
			die("%s is not locked", path);
	}

	sb_addf(&body, "{\"force\":%s,", o.force ? "true" : "false");
	add_ref(&body);
	if (body.buf[body.len - 1] == ',')
		body.buf[--body.len] = '\0';
	sb_addch(&body, '}');
	sb_adds(&url, "/");
	url_escape(&url, id);
	sb_adds(&url, "/unlock");

	j = lock_api(e, url.buf, body.buf, NULL, 0);
	if (!j) {
		ret = 2;
	} else if (o.json) {
		print_json(json_get(j, "lock"));
	} else {
		const char *p = json_get_str(json_get(j, "lock"), "path");

		printf("Unlocked %s\n", p ? p : path ? path : id);
	}
	json_free(j);
	free(id);
	free(path);
	sb_free(&body);
	sb_free(&url);
	return ret;
}

static void print_locks(const struct json *arr, const char *mark)
{
	const struct json *l;

	for (l = arr && arr->type == J_ARR ? arr->child : NULL; l; l = l->next) {
		const char *path = json_get_str(l, "path"), *id = json_get_str(l, "id");

		printf("%s%s\t%s\tID:%s\n", mark, path ? path : "", lock_owner(l), id ? id : "");
	}
}

int cmd_locks(int argc, char **argv)
{
	struct strbuf q = STRBUF_INIT, all = STRBUF_INIT;
	struct lock_opts o;
	struct endpoint *e;
	char *cursor = NULL;
	char *ref = git_current_ref();
	int ret = 0, first = 1;

	parse_lock_opts(argc, argv, &o);
	e = lock_endpoint(&o);
	if (o.json)
		sb_addch(&all, '[');

	do {
		struct json *j;
		const char *next;

		sb_reset(&q);
		if (o.verify) {
			struct strbuf body = STRBUF_INIT;

			sb_addch(&body, '{');
			add_ref(&body);
			if (cursor) {
				sb_adds(&body, body.len > 1 ? ",\"cursor\":" : "\"cursor\":");
				json_add_str(&body, cursor);
			}
			sb_addch(&body, '}');
			j = lock_api(e, "/verify", body.buf, NULL, 0);
			sb_free(&body);
		} else {
			sb_addch(&q, '?');
			if (o.path) {
				char *p = repo_path(o.path);

				sb_adds(&q, "path=");
				url_escape(&q, p);
				sb_addch(&q, '&');
				free(p);
			}
			if (o.id) {
				sb_adds(&q, "id=");
				url_escape(&q, o.id);
				sb_addch(&q, '&');
			}
			if (o.limit > 0)
				sb_addf(&q, "limit=%ld&", o.limit);
			if (ref) {
				sb_adds(&q, "refspec=");
				url_escape(&q, ref);
				sb_addch(&q, '&');
			}
			if (cursor) {
				sb_adds(&q, "cursor=");
				url_escape(&q, cursor);
				sb_addch(&q, '&');
			}
			q.buf[--q.len] = '\0';
			j = lock_api(e, q.buf, NULL, NULL, 0);
		}
		if (!j) {
			ret = 2;
			break;
		}
		if (o.json) {
			const struct json *l;
			const char *lists[] = { "locks", "ours", "theirs" };
			size_t k;

			for (k = 0; k < 3; k++) {
				const struct json *arr = json_get(j, lists[k]);

				for (l = arr && arr->type == J_ARR ? arr->child : NULL; l; l = l->next) {
					if (!first)
						sb_addch(&all, ',');
					json_dump(&all, l);
					first = 0;
				}
			}
		} else if (o.verify) {
			print_locks(json_get(j, "ours"), "O ");
			print_locks(json_get(j, "theirs"), "  ");
		} else {
			print_locks(json_get(j, "locks"), "");
		}
		next = json_get_str(j, "next_cursor");
		free(cursor);
		cursor = next && *next && o.limit <= 0 ? xstrdup(next) : NULL;
		json_free(j);
	} while (cursor);

	if (o.json && !ret) {
		sb_addch(&all, ']');
		puts(all.buf);
	}
	free(cursor);
	free(ref);
	sb_free(&q);
	sb_free(&all);
	return ret;
}
