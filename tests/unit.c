/* Unit-тесты для разборщиков и вспомогательных модулей */
#include "endpoint.h"
#include "git.h"
#include "json.h"
#include "pathfilter.h"
#include "pktline.h"
#include "pointer.h"
#include "run.h"
#include "store.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_STR(a, b) do { \
	const char *a_ = (a), *b_ = (b); \
	checks++; \
	if (!a_ || !b_ || strcmp(a_, b_)) { \
		failures++; \
		fprintf(stderr, "%s:%d: '%s' != '%s'\n", __FILE__, __LINE__, \
			a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
	} \
} while (0)

static struct json *parse(const char *s)
{
	return json_parse(s, strlen(s));
}

static void test_json(void)
{
	struct json *j;
	const struct json *arr;
	struct strbuf sb = STRBUF_INIT;

	j = parse("{\"a\": 1, \"b\": \"x\\ny\", \"c\": [true, false, null], \"d\": {\"e\": -12}}");
	CHECK(j != NULL);
	CHECK(json_get_int(j, "a", 0) == 1);
	CHECK_STR(json_get_str(j, "b"), "x\ny");
	arr = json_get(j, "c");
	CHECK(arr && arr->type == J_ARR && arr->child->type == J_BOOL && arr->child->num == 1);
	CHECK(json_get_int(json_get(j, "d"), "e", 0) == -12);
	CHECK(json_get_str(j, "missing") == NULL);
	CHECK(json_get_int(j, "b", 42) == 42);
	json_dump(&sb, j);
	CHECK_STR(sb_str(&sb), "{\"a\":1,\"b\":\"x\\ny\",\"c\":[true,false,null],\"d\":{\"e\":-12}}");
	json_free(j);

	j = parse("\"\\u043f\\u0440\\ud83d\\ude00\"");
	CHECK(j && j->type == J_STR && !strcmp(j->str, "пр\xF0\x9F\x98\x80"));
	json_free(j);

	j = parse("{\"size\": 10737418240, \"f\": 1.5e3}");
	CHECK(json_get_int(j, "size", 0) == 10737418240LL);
	CHECK(json_get_int(j, "f", 0) == 1500);
	json_free(j);

	CHECK(parse("") == NULL);
	CHECK(parse("{") == NULL);
	CHECK(parse("{\"a\":}") == NULL);
	CHECK(parse("[1,]") == NULL);
	CHECK(parse("{\"a\":1} x") == NULL);
	CHECK(parse("\"bad\\q\"") == NULL);
	CHECK(parse("\"ctrl\x01\"") == NULL);
	{
		/* глубокая вложенность не должна валить парсер */
		char deep[300];

		memset(deep, '[', sizeof(deep) - 1);
		deep[sizeof(deep) - 1] = '\0';
		CHECK(parse(deep) == NULL);
	}

	sb_reset(&sb);
	json_add_str(&sb, "a\"b\\c\n\x01");
	CHECK_STR(sb_str(&sb), "\"a\\\"b\\\\c\\n\\u0001\"");
	sb_free(&sb);
}

#define OID "4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393"

static void test_pointer(void)
{
	struct strbuf sb = STRBUF_INIT;
	struct pointer p, q;
	const char *good = LFS_SPEC "\noid sha256:" OID "\nsize 12345\n";

	CHECK(!pointer_parse(good, strlen(good), &p));
	CHECK_STR(p.oid, OID);
	CHECK(p.size == 12345);
	pointer_format(&p, &sb);
	CHECK_STR(sb_str(&sb), good);
	CHECK(!pointer_parse(sb.buf, sb.len, &q) && q.size == p.size);

	{
		const char *ext = LFS_SPEC "\next-0-foo sha256:abc\noid sha256:" OID "\nsize 1\n";
		const char *noeol = LFS_SPEC "\noid sha256:" OID "\nsize 7";

		CHECK(!pointer_parse(ext, strlen(ext), &p));
		CHECK(!pointer_parse(noeol, strlen(noeol), &p) && p.size == 7);
	}
	{
		const char *bad[] = {
			"",
			"hello world\n",
			"version https://example.com/spec\noid sha256:" OID "\nsize 1\n",
			LFS_SPEC "\noid sha256:" OID "\n",
			LFS_SPEC "\nsize 1\n",
			LFS_SPEC "\noid sha256:XYZ\nsize 1\n",
			LFS_SPEC "\noid sha256:" OID "\nsize -1\n",
			LFS_SPEC "\noid sha256:" OID "\nsize 1x\n",
			LFS_SPEC "\noid sha256:" OID "\ngarbage\n",
			NULL,
		};
		int i;

		for (i = 0; bad[i]; i++)
			CHECK(pointer_parse(bad[i], strlen(bad[i]), &p) != 0);
	}
	{
		char big[POINTER_MAX + 10];

		memset(big, 'a', sizeof(big));
		memcpy(big, good, strlen(good));
		CHECK(pointer_parse(big, sizeof(big), &p) != 0);
	}
	sb_free(&sb);
}

static void test_pktline(void)
{
	int fds[2];
	char *buf = xmalloc(PKT_MAX_DATA + 1);
	char *big = xmalloc(PKT_MAX_DATA * 2 + 100);
	size_t len, total = 0;
	int t;

	CHECK(pipe(fds) == 0);
	if (fork() == 0) {
		close(fds[0]);
		pkt_write_text(fds[1], "git-filter-client");
		pkt_flush(fds[1]);
		memset(big, 'x', PKT_MAX_DATA * 2 + 100);
		pkt_write_data(fds[1], big, PKT_MAX_DATA * 2 + 100);
		pkt_flush(fds[1]);
		_exit(0);
	}
	close(fds[1]);
	CHECK(pkt_read_text(fds[0], buf) == PKT_DATA);
	CHECK_STR(buf, "git-filter-client");
	CHECK(pkt_read(fds[0], buf, &len) == PKT_FLUSH);
	while ((t = pkt_read(fds[0], buf, &len)) == PKT_DATA) {
		CHECK(len <= PKT_MAX_DATA);
		total += len;
	}
	CHECK(t == PKT_FLUSH);
	CHECK(total == PKT_MAX_DATA * 2 + 100);
	CHECK(pkt_read(fds[0], buf, &len) == PKT_EOF);
	close(fds[0]);
	free(buf);
	free(big);
}

static void test_pathfilter(void)
{
	struct pathfilter f;

	CHECK(path_matches("*.bin", "a.bin"));
	CHECK(path_matches("*.bin", "dir/sub/a.bin"));
	CHECK(!path_matches("*.bin", "a.bin.txt"));
	CHECK(path_matches("images", "images/a.png"));
	CHECK(path_matches("images", "x/images/a.png"));
	CHECK(path_matches("images/", "images/a.png"));
	CHECK(path_matches("/assets/*.psd", "assets/a.psd"));
	CHECK(path_matches("assets/*.psd", "assets/a.psd"));
	CHECK(!path_matches("assets/*.psd", "other/assets/a.psd"));
	CHECK(path_matches("docs", "docs"));
	CHECK(path_matches("data/big", "data/big/x/y.dat"));
	CHECK(!path_matches("data/big", "data/bigger/x"));
	CHECK(!path_matches("", "anything"));

	memset(&f, 0, sizeof(f));
	pathfilter_add(&f.inc, &f.ninc, " *.iso , media ");
	pathfilter_add(&f.exc, &f.nexc, "media/tmp");
	CHECK(f.ninc == 2);
	CHECK_STR(f.inc[0], "*.iso");
	CHECK_STR(f.inc[1], "media");
	CHECK(pathfilter_match(&f, "a.iso"));
	CHECK(pathfilter_match(&f, "media/a.png"));
	CHECK(!pathfilter_match(&f, "media/tmp/a.png"));
	CHECK(!pathfilter_match(&f, "src/a.c"));
	pathfilter_free(&f);

	memset(&f, 0, sizeof(f));
	CHECK(pathfilter_match(&f, "anything"));
	CHECK(pathfilter_match(&f, NULL));
}

static void test_url(void)
{
	struct lfs_url u;
	char *h;

	CHECK(!lfs_url_parse("https://user:p%40ss@git.example.com:8443/group/repo.git", &u));
	CHECK_STR(u.scheme, "https");
	CHECK_STR(u.user, "user");
	CHECK_STR(u.pass, "p@ss");
	CHECK_STR(u.host, "git.example.com:8443");
	CHECK_STR(u.path, "/group/repo.git");
	lfs_url_free(&u);

	CHECK(!lfs_url_parse("git@gitlab.example.com:group/repo.git", &u));
	CHECK_STR(u.scheme, "ssh");
	CHECK_STR(u.user, "git");
	CHECK_STR(u.host, "gitlab.example.com");
	CHECK_STR(u.path, "group/repo.git");
	lfs_url_free(&u);

	CHECK(!lfs_url_parse("ssh://git@host:2222/g/r", &u));
	CHECK_STR(u.host, "host:2222");
	CHECK_STR(u.path, "/g/r");
	CHECK(u.pass == NULL);
	lfs_url_free(&u);

	CHECK(!lfs_url_parse("https://host", &u));
	CHECK_STR(u.path, "/");
	lfs_url_free(&u);

	CHECK(lfs_url_parse("/srv/git/repo.git", &u) != 0);
	CHECK(lfs_url_parse("./relative/path:with-colon", &u) != 0);
	CHECK(lfs_url_parse("https:///nohost", &u) != 0);

	h = lfs_derive_href("https", "host", "/g/r.git");
	CHECK_STR(h, "https://host/g/r.git/info/lfs");
	free(h);
	h = lfs_derive_href("https", "host", "g/r/");
	CHECK_STR(h, "https://host/g/r.git/info/lfs");
	free(h);
}

static void test_util(void)
{
	struct strbuf sb = STRBUF_INIT;
	char buf[32];

	CHECK(is_oid(OID));
	CHECK(!is_oid("4d7a"));
	CHECK(!is_oid("4D7A214614AB2935C943F9E0FF69D22EADBB8F32B1258DAAA5E2CA24D17E2393"));
	CHECK_STR(skip_prefix("size 10", "size "), "10");
	CHECK(skip_prefix("oid", "size ") == NULL);
	CHECK(parse_size("10") == 10);
	CHECK(parse_size("1k") == 1024);
	CHECK(parse_size("1.5mb") == 1572864);
	CHECK(parse_size("2 GB") == 2147483648LL);
	CHECK(parse_size("abc") == -1);
	CHECK(parse_size("5 parsecs") == -1);
	format_size(512, buf, sizeof(buf));
	CHECK_STR(buf, "512 B");
	format_size(1572864, buf, sizeof(buf));
	CHECK_STR(buf, "1.5 MB");

	sb_addf(&sb, "%d-%s", 42, "x");
	sb_adds(&sb, "  \n");
	sb_rtrim(&sb);
	CHECK_STR(sb_str(&sb), "42-x");
	sb_free(&sb);
	CHECK_STR(sb_str(&sb), "");
}

static void test_run(void)
{
	struct strbuf out = STRBUF_INIT;
	const char *cat[] = { "cat", NULL };
	const char *fail[] = { "sh", "-c", "exit 3", NULL };
	const char *nope[] = { "/nonexistent/binary", NULL };
	struct child c;
	struct reader *r = xmalloc(sizeof(*r));
	size_t big = 1 << 20, i;
	char *data = xmalloc(big);

	/* stdin и stdout больше буфера пайпа: проверка на взаимную блокировку */
	for (i = 0; i < big; i++)
		data[i] = (char)('a' + i % 26);
	CHECK(run_cmd(cat, data, big, &out, 0) == 0);
	CHECK(out.len == big && !memcmp(out.buf, data, big));
	CHECK(run_cmd(fail, NULL, 0, NULL, 0) == 3);
	CHECK(run_cmd(nope, NULL, 0, NULL, RUN_QUIET) == 127);

	start_cmd(&c, cat, 0, CHILD_PIPE, CHILD_PIPE);
	reader_init(r, c.out);
	CHECK(write_all(c.in, "line1\nline2\nxyz", 15) == 0);
	close(c.in);
	c.in = -1;
	CHECK(reader_line(r, &out) == 0 && !strcmp(out.buf, "line1"));
	CHECK(reader_line(r, &out) == 0 && !strcmp(out.buf, "line2"));
	CHECK(reader_bytes(r, data, 3) == 3 && !memcmp(data, "xyz", 3));
	CHECK(reader_line(r, &out) == -1);
	CHECK(finish_cmd(&c) == 0);

	sb_free(&out);
	free(data);
	free(r);
}

/* clean во временном репозитории: объект ложится в .git/lfs/objects */
static void test_cleaner(void)
{
	char tmpl[] = "/tmp/lfs-unit-XXXXXX";
	char *dir = mkdtemp(tmpl);
	struct strbuf out = STRBUF_INIT;
	struct cleaner c;
	struct pointer p;
	char *path;

	CHECK(dir != NULL);
	if (!dir || chdir(dir) || run_git(NULL, 0, NULL, RUN_QUIET, "init", "-q", NULL)) {
		failures++;
		return;
	}
	cleaner_init(&c);
	cleaner_write(&c, "hello ", 6);
	cleaner_write(&c, "world\n", 6);
	cleaner_finish(&c, &out);
	CHECK(!pointer_parse(out.buf, out.len, &p));
	/* sha256("hello world\n") */
	CHECK_STR(p.oid, "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447");
	CHECK(p.size == 12);
	CHECK(lfs_object_ok(p.oid, 12));
	path = lfs_object_path(p.oid);
	CHECK(strstr(path, "/lfs/objects/a9/48/") != NULL);
	free(path);

	/* уже pointer — возвращается как есть, объект не создаётся */
	{
		struct strbuf again = STRBUF_INIT;

		cleaner_init(&c);
		cleaner_write(&c, out.buf, out.len);
		cleaner_finish(&c, &again);
		CHECK(again.len == out.len && !memcmp(again.buf, out.buf, out.len));
		sb_free(&again);
	}
	/* пустой файл остаётся пустым */
	sb_reset(&out);
	cleaner_init(&c);
	cleaner_finish(&c, &out);
	CHECK(out.len == 0);

	sb_free(&out);
	if (chdir("/") == 0) {
		const char *rm[] = { "rm", "-rf", dir, NULL };

		run_cmd(rm, NULL, 0, NULL, 0);
	}
}

int main(void)
{
	test_json();
	test_pointer();
	test_pktline();
	test_pathfilter();
	test_url();
	test_util();
	test_run();
	test_cleaner();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
