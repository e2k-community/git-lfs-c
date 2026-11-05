#include "sha256.h"
#include "util.h"

#include <pthread.h>
#include <string.h>

/*
 * Блочная функция: 64 раунда полностью развёрнуты, расписание сообщения
 * считается по ходу раундов в кольце из 16 слов, а переменные состояния не
 * переставляются, а переименовываются аргументами макроса. Maj считается
 * через переносимое между раундами a^b: Maj(a,b,c) = b ^ ((a^b) & (b^c)),
 * где b^c — это a^b прошлого раунда.
 *
 * Вариант выбран замерами experiments/sha256 на Эльбрус-8СВ (e2k v5):
 * 141 МБ/с с clang и 123 МБ/с с lcc против 63 МБ/с у OpenSSL 3.5.
 * Заранее посчитанное расписание и вращения через 64-битное удвоение слова
 * там оказались медленнее.
 */

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t load_be32(const unsigned char *p)
{
	uint32_t v;

	/* так, а не сдвигами байтов: на e2k это заметно быстрее */
	memcpy(&v, p, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	v = __builtin_bswap32(v);
#endif
	return v;
}

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define S0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define S1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define s0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define s1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))
#define CH(e, f, g) ((g) ^ ((e) & ((f) ^ (g))))

#define ROUND(a, b, c, d, e, f, g, h, i) do {                                \
	uint32_t t1 = h + S1(e) + CH(e, f, g) + K[i] + X[(i) & 15];          \
	uint32_t ab = a ^ b;                                                 \
	d += t1;                                                             \
	h = t1 + S0(a) + (b ^ (ab & bc));                                    \
	bc = ab;                                                             \
} while (0)
#define SCHED(i) (X[(i) & 15] += s1(X[((i) - 2) & 15]) + X[((i) - 7) & 15] + s0(X[((i) - 15) & 15]))
#define ROUND_S(a, b, c, d, e, f, g, h, i) do { SCHED(i); ROUND(a, b, c, d, e, f, g, h, i); } while (0)
#define EIGHT(R, i) do {                                                     \
	R(a, b, c, d, e, f, g, h, (i) + 0); R(h, a, b, c, d, e, f, g, (i) + 1); \
	R(g, h, a, b, c, d, e, f, (i) + 2); R(f, g, h, a, b, c, d, e, (i) + 3); \
	R(e, f, g, h, a, b, c, d, (i) + 4); R(d, e, f, g, h, a, b, c, (i) + 5); \
	R(c, d, e, f, g, h, a, b, (i) + 6); R(b, c, d, e, f, g, h, a, (i) + 7); \
} while (0)

static void sha256_blocks(uint32_t st[8], const unsigned char *p, size_t n)
{
	while (n--) {
		uint32_t X[16];
		uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
		uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
		uint32_t bc = b ^ c;
		int i;

		for (i = 0; i < 16; i++)
			X[i] = load_be32(p + 4 * i);
		EIGHT(ROUND, 0); EIGHT(ROUND, 8);
		EIGHT(ROUND_S, 16); EIGHT(ROUND_S, 24); EIGHT(ROUND_S, 32);
		EIGHT(ROUND_S, 40); EIGHT(ROUND_S, 48); EIGHT(ROUND_S, 56);
		st[0] += a; st[1] += b; st[2] += c; st[3] += d;
		st[4] += e; st[5] += f; st[6] += g; st[7] += h;
		p += 64;
	}
}

void sha256_builtin_init(struct sha256_builtin *c)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(c->st, iv, sizeof(iv));
	c->buflen = 0;
	c->total = 0;
}

void sha256_builtin_update(struct sha256_builtin *c, const void *data, size_t n)
{
	const unsigned char *p = data;

	c->total += n;
	if (c->buflen) {
		size_t k = 64 - c->buflen < n ? 64 - c->buflen : n;

		memcpy(c->buf + c->buflen, p, k);
		c->buflen += k;
		p += k;
		n -= k;
		if (c->buflen < 64)
			return;
		sha256_blocks(c->st, c->buf, 1);
		c->buflen = 0;
	}
	if (n >= 64) {
		sha256_blocks(c->st, p, n / 64);
		p += n & ~(size_t)63;
		n &= 63;
	}
	memcpy(c->buf, p, n);
	c->buflen = n;
}

void sha256_builtin_final(struct sha256_builtin *c, unsigned char out[32])
{
	uint64_t bits = c->total * 8;
	unsigned char pad[72] = { 0x80 };
	size_t padlen = (c->buflen < 56 ? 56 : 120) - c->buflen;
	int i;

	for (i = 0; i < 8; i++)
		pad[padlen + i] = (unsigned char)(bits >> (56 - 8 * i));
	sha256_builtin_update(c, pad, padlen + 8);
	for (i = 0; i < 8; i++) {
		out[4 * i] = (unsigned char)(c->st[i] >> 24);
		out[4 * i + 1] = (unsigned char)(c->st[i] >> 16);
		out[4 * i + 2] = (unsigned char)(c->st[i] >> 8);
		out[4 * i + 3] = (unsigned char)c->st[i];
	}
}

#ifdef LFS_BUILTIN_SHA256

void sha256_init(struct sha256 *c)
{
	sha256_builtin_init(&c->b);
}

void sha256_update(struct sha256 *c, const void *data, size_t n)
{
	sha256_builtin_update(&c->b, data, n);
}

void sha256_final(struct sha256 *c, unsigned char out[32])
{
	sha256_builtin_final(&c->b, out);
}

const char *sha256_impl(void)
{
	return "builtin";
}

void sha256_prepare(void)
{
}

#else

static const EVP_MD *sha256_md;
static pthread_once_t sha256_once = PTHREAD_ONCE_INIT;

/* В OpenSSL 3 EVP_sha256() каждый раз ищет реализацию — ищем один раз */
static void sha256_md_init(void)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	sha256_md = EVP_MD_fetch(NULL, "SHA256", NULL);
#endif
	if (!sha256_md)
		sha256_md = EVP_sha256();
}

void sha256_prepare(void)
{
	pthread_once(&sha256_once, sha256_md_init);
}

void sha256_init(struct sha256 *c)
{
	sha256_prepare();
	c->md = EVP_MD_CTX_new();
	if (!c->md || !EVP_DigestInit_ex(c->md, sha256_md, NULL))
		die("sha256 init failed");
}

void sha256_update(struct sha256 *c, const void *data, size_t n)
{
	if (!EVP_DigestUpdate(c->md, data, n))
		die("sha256 update failed");
}

void sha256_final(struct sha256 *c, unsigned char out[32])
{
	if (!EVP_DigestFinal_ex(c->md, out, NULL))
		die("sha256 final failed");
	EVP_MD_CTX_free(c->md);
	c->md = NULL;
}

const char *sha256_impl(void)
{
	return "openssl";
}

#endif
