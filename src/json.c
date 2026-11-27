/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Valentine Vasilenko */
#include "json.h"

#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

struct parser {
	const char *s, *end;
	int depth;
};

static struct json *parse_value(struct parser *p);

static void skip_ws(struct parser *p)
{
	while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' ||
				 *p->s == '\n' || *p->s == '\r'))
		p->s++;
}

static struct json *new_node(enum json_type t)
{
	struct json *j = xcalloc(1, sizeof(*j));

	j->type = t;
	return j;
}

static int parse_hex4(struct parser *p, unsigned *out)
{
	unsigned v = 0;
	int i;

	if (p->end - p->s < 4)
		return -1;
	for (i = 0; i < 4; i++) {
		int h = hexval((unsigned char)p->s[i]);

		if (h < 0)
			return -1;
		v = v * 16 + (unsigned)h;
	}
	p->s += 4;
	*out = v;
	return 0;
}

static void utf8_put(struct strbuf *sb, unsigned cp)
{
	if (cp < 0x80) {
		sb_addch(sb, (int)cp);
	} else if (cp < 0x800) {
		sb_addch(sb, (int)(0xC0 | (cp >> 6)));
		sb_addch(sb, (int)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		sb_addch(sb, (int)(0xE0 | (cp >> 12)));
		sb_addch(sb, (int)(0x80 | ((cp >> 6) & 0x3F)));
		sb_addch(sb, (int)(0x80 | (cp & 0x3F)));
	} else {
		sb_addch(sb, (int)(0xF0 | (cp >> 18)));
		sb_addch(sb, (int)(0x80 | ((cp >> 12) & 0x3F)));
		sb_addch(sb, (int)(0x80 | ((cp >> 6) & 0x3F)));
		sb_addch(sb, (int)(0x80 | (cp & 0x3F)));
	}
}

/* p->s указывает на открывающую кавычку */
static char *parse_string(struct parser *p)
{
	struct strbuf sb = STRBUF_INIT;

	p->s++;
	while (p->s < p->end) {
		unsigned char c = (unsigned char)*p->s++;
		unsigned cp, lo;

		if (c == '"')
			return sb_detach(&sb);
		if (c < 0x20)
			break;
		if (c != '\\') {
			sb_addch(&sb, c);
			continue;
		}
		if (p->s >= p->end)
			break;
		c = (unsigned char)*p->s++;
		switch (c) {
		case '"': case '\\': case '/':
			sb_addch(&sb, c);
			break;
		case 'b': sb_addch(&sb, '\b'); break;
		case 'f': sb_addch(&sb, '\f'); break;
		case 'n': sb_addch(&sb, '\n'); break;
		case 'r': sb_addch(&sb, '\r'); break;
		case 't': sb_addch(&sb, '\t'); break;
		case 'u':
			if (parse_hex4(p, &cp))
				goto fail;
			if (cp >= 0xD800 && cp < 0xDC00) {
				if (p->end - p->s < 6 || p->s[0] != '\\' || p->s[1] != 'u')
					goto fail;
				p->s += 2;
				if (parse_hex4(p, &lo) || lo < 0xDC00 || lo > 0xDFFF)
					goto fail;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			}
			utf8_put(&sb, cp);
			break;
		default:
			goto fail;
		}
	}
fail:
	sb_free(&sb);
	return NULL;
}

static struct json *parse_literal(struct parser *p, const char *lit,
				  enum json_type t, long long val)
{
	size_t n = strlen(lit);
	struct json *j;

	if ((size_t)(p->end - p->s) < n || memcmp(p->s, lit, n))
		return NULL;
	p->s += n;
	j = new_node(t);
	j->num = val;
	return j;
}

static struct json *parse_number(struct parser *p)
{
	const char *start = p->s;
	char tmp[64], *e;
	size_t n;
	struct json *j;
	double d;

	while (p->s < p->end && *p->s && strchr("+-0123456789.eE", *p->s))
		p->s++;
	n = (size_t)(p->s - start);
	if (!n || n >= sizeof(tmp))
		return NULL;
	memcpy(tmp, start, n);
	tmp[n] = '\0';
	d = strtod(tmp, &e);
	if (*e)
		return NULL;
	j = new_node(J_NUM);
	j->num = strpbrk(tmp, ".eE") ? (long long)d : strtoll(tmp, NULL, 10);
	return j;
}

static struct json *parse_container(struct parser *p, enum json_type t)
{
	char close = t == J_OBJ ? '}' : ']';
	struct json *j, **tail;

	if (++p->depth > MAX_DEPTH)
		return NULL;
	j = new_node(t);
	tail = &j->child;
	p->s++;
	skip_ws(p);
	if (p->s < p->end && *p->s == close) {
		p->s++;
		p->depth--;
		return j;
	}
	for (;;) {
		char *key = NULL;
		struct json *v;

		skip_ws(p);
		if (t == J_OBJ) {
			if (p->s >= p->end || *p->s != '"' || !(key = parse_string(p)))
				goto fail;
			skip_ws(p);
			if (p->s >= p->end || *p->s != ':') {
				free(key);
				goto fail;
			}
			p->s++;
		}
		v = parse_value(p);
		if (!v) {
			free(key);
			goto fail;
		}
		v->key = key;
		*tail = v;
		tail = &v->next;
		skip_ws(p);
		if (p->s >= p->end)
			goto fail;
		if (*p->s == ',') {
			p->s++;
			continue;
		}
		if (*p->s == close) {
			p->s++;
			break;
		}
		goto fail;
	}
	p->depth--;
	return j;
fail:
	json_free(j);
	return NULL;
}

static struct json *parse_value(struct parser *p)
{
	struct json *j;
	char *s;

	skip_ws(p);
	if (p->s >= p->end)
		return NULL;
	switch (*p->s) {
	case '{':
		return parse_container(p, J_OBJ);
	case '[':
		return parse_container(p, J_ARR);
	case '"':
		if (!(s = parse_string(p)))
			return NULL;
		j = new_node(J_STR);
		j->str = s;
		return j;
	case 't':
		return parse_literal(p, "true", J_BOOL, 1);
	case 'f':
		return parse_literal(p, "false", J_BOOL, 0);
	case 'n':
		return parse_literal(p, "null", J_NULL, 0);
	default:
		return parse_number(p);
	}
}

struct json *json_parse(const char *s, size_t len)
{
	struct parser p = { s, s + len, 0 };
	struct json *j = parse_value(&p);

	if (!j)
		return NULL;
	skip_ws(&p);
	if (p.s != p.end) {
		json_free(j);
		return NULL;
	}
	return j;
}

void json_free(struct json *j)
{
	while (j) {
		struct json *next = j->next;

		json_free(j->child);
		free(j->key);
		free(j->str);
		free(j);
		j = next;
	}
}

const struct json *json_get(const struct json *obj, const char *key)
{
	const struct json *c;

	if (!obj || obj->type != J_OBJ)
		return NULL;
	for (c = obj->child; c; c = c->next)
		if (!strcmp(c->key, key))
			return c;
	return NULL;
}

const char *json_get_str(const struct json *obj, const char *key)
{
	const struct json *j = json_get(obj, key);

	return j && j->type == J_STR ? j->str : NULL;
}

long long json_get_int(const struct json *obj, const char *key, long long def)
{
	const struct json *j = json_get(obj, key);

	return j && j->type == J_NUM ? j->num : def;
}

void json_dump(struct strbuf *sb, const struct json *j)
{
	const struct json *c;

	if (!j) {
		sb_adds(sb, "null");
		return;
	}
	switch (j->type) {
	case J_NULL:
		sb_adds(sb, "null");
		break;
	case J_BOOL:
		sb_adds(sb, j->num ? "true" : "false");
		break;
	case J_NUM:
		sb_addf(sb, "%lld", j->num);
		break;
	case J_STR:
		json_add_str(sb, j->str);
		break;
	case J_ARR:
	case J_OBJ:
		sb_addch(sb, j->type == J_ARR ? '[' : '{');
		for (c = j->child; c; c = c->next) {
			if (c != j->child)
				sb_addch(sb, ',');
			if (j->type == J_OBJ) {
				json_add_str(sb, c->key);
				sb_addch(sb, ':');
			}
			json_dump(sb, c);
		}
		sb_addch(sb, j->type == J_ARR ? ']' : '}');
		break;
	}
}

void json_add_str(struct strbuf *sb, const char *s)
{
	sb_addch(sb, '"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			sb_addch(sb, '\\');
			sb_addch(sb, c);
		} else if (c == '\n') {
			sb_adds(sb, "\\n");
		} else if (c == '\t') {
			sb_adds(sb, "\\t");
		} else if (c == '\r') {
			sb_adds(sb, "\\r");
		} else if (c < 0x20) {
			sb_addf(sb, "\\u%04x", c);
		} else {
			sb_addch(sb, c);
		}
	}
	sb_addch(sb, '"');
}
