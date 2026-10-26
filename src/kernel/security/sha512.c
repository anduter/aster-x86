/* =============================================================================
 * security/sha512.c - SHA-512 + PBKDF2-HMAC-SHA512（无外部依赖）
 *
 * 为什么自带一份实现：RSA 验签只需要 SHA-256，而口令存储要 PBKDF2-SHA512。
 * 这里不碰 trust.c（启动验签路径保持小而稳定），也不依赖 libc。
 *
 * PBKDF2（RFC 8018）固定单块输出（我们的 dkLen=32 < hLen=64）：
 *   U1 = HMAC(P, S || INT_BE(1))
 *   Ui = HMAC(P, U(i-1))
 *   T  = U1 xor U2 xor ... xor Uc
 * HMAC 的内层/外层前缀状态（ipad/opad 压缩后）每次迭代复制一次，
 * 省掉每轮 2 次哈希压缩，10000 轮在 QEMU 里也能接受。
 * ============================================================================= */
/* build.ps1 全局用 -O0（可调试性优先）；口令 KDF 是启动期热点，单独开 O2，
 * 否则 10000 轮 PBKDF2 在 QEMU TCG 里要跑几十秒。 */
#pragma GCC optimize ("O2")

#include "sha512.h"

typedef struct {
    uint64_t h[8];
    uint64_t len;             /* 已输入字节数 */
    uint8_t  buf[128];
    uint32_t used;
} sha512_ctx;

static void copy_bytes(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- > 0U) *d++ = *s++;
}

static void zero_bytes(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n-- > 0U) *d++ = 0;
}

static uint64_t rotr64(uint64_t x, uint32_t n)
{
    return (x >> n) | (x << (64U - n));
}

static const uint64_t sha512_k[80] = {
    0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL, 0xB5C0FBCFEC4D3B2FULL,
    0xE9B5DBA58189DBBCULL, 0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL,
    0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL, 0xD807AA98A3030242ULL,
    0x12835B0145706FBEULL, 0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
    0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL, 0x9BDC06A725C71235ULL,
    0xC19BF174CF692694ULL, 0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL,
    0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL, 0x2DE92C6F592B0275ULL,
    0x4A7484AA6EA6E483ULL, 0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
    0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL, 0xB00327C898FB213FULL,
    0xBF597FC7BEEF0EE4ULL, 0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL,
    0x06CA6351E003826FULL, 0x142929670A0E6E70ULL, 0x27B70A8546D22FFCULL,
    0x2E1B21385C26C926ULL, 0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
    0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL, 0x81C2C92E47EDAEE6ULL,
    0x92722C851482353BULL, 0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL,
    0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL, 0xD192E819D6EF5218ULL,
    0xD69906245565A910ULL, 0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
    0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL, 0x2748774CDF8EEB99ULL,
    0x34B0BCB5E19B48A8ULL, 0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL,
    0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL, 0x748F82EE5DEFB2FCULL,
    0x78A5636F43172F60ULL, 0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
    0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL, 0xBEF9A3F7B2C67915ULL,
    0xC67178F2E372532BULL, 0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL,
    0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL, 0x06F067AA72176FBAULL,
    0x0A637DC5A2C898A6ULL, 0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
    0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL, 0x3C9EBE0A15C9BEBCULL,
    0x431D67C49C100D4CULL, 0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL,
    0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL
};

static void sha512_init(sha512_ctx *c)
{
    c->h[0] = 0x6A09E667F3BCC908ULL; c->h[1] = 0xBB67AE8584CAA73BULL;
    c->h[2] = 0x3C6EF372FE94F82BULL; c->h[3] = 0xA54FF53A5F1D36F1ULL;
    c->h[4] = 0x510E527FADE682D1ULL; c->h[5] = 0x9B05688C2B3E6C1FULL;
    c->h[6] = 0x1F83D9ABFB41BD6BULL; c->h[7] = 0x5BE0CD19137E2179ULL;
    c->len = 0;
    c->used = 0;
}

static void sha512_compress(sha512_ctx *c, const uint8_t *p)
{
    uint64_t w[80];
    uint64_t a, b, cc, d, e, f, g, h;

    for (uint32_t i = 0; i < 16U; i++) {
        w[i] = ((uint64_t)p[i * 8U] << 56) |
               ((uint64_t)p[i * 8U + 1U] << 48) |
               ((uint64_t)p[i * 8U + 2U] << 40) |
               ((uint64_t)p[i * 8U + 3U] << 32) |
               ((uint64_t)p[i * 8U + 4U] << 24) |
               ((uint64_t)p[i * 8U + 5U] << 16) |
               ((uint64_t)p[i * 8U + 6U] << 8) |
               (uint64_t)p[i * 8U + 7U];
    }
    for (uint32_t i = 16U; i < 80U; i++) {
        uint64_t s0 = rotr64(w[i - 15U], 1) ^ rotr64(w[i - 15U], 8) ^ (w[i - 15U] >> 7);
        uint64_t s1 = rotr64(w[i - 2U], 19) ^ rotr64(w[i - 2U], 61) ^ (w[i - 2U] >> 6);

        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for (uint32_t i = 0; i < 80U; i++) {
        uint64_t s1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + s1 + ch + sha512_k[i] + w[i];
        uint64_t s0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha512_update(sha512_ctx *c, const uint8_t *data, uint32_t len)
{
    c->len += (uint64_t)len;
    while (len > 0U) {
        uint32_t n = 128U - c->used;

        if (n > len) n = len;
        copy_bytes(c->buf + c->used, data, n);
        c->used += n;
        data += n;
        len -= n;
        if (c->used == 128U) {
            sha512_compress(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha512_final(sha512_ctx *c, uint8_t out[64])
{
    uint64_t bits = c->len * 8U;
    uint8_t pad[128];
    uint8_t lenbuf[16];
    uint32_t padlen = (c->used < 112U) ? (112U - c->used) : (240U - c->used);

    zero_bytes(pad, sizeof(pad));
    pad[0] = 0x80U;
    sha512_update(c, pad, padlen);

    zero_bytes(lenbuf, sizeof(lenbuf));
    for (uint32_t i = 0; i < 8U; i++) {
        lenbuf[15U - i] = (uint8_t)(bits >> (i * 8U));
    }
    sha512_update(c, lenbuf, 16U);

    for (uint32_t i = 0; i < 8U; i++) {
        out[i * 8U]      = (uint8_t)(c->h[i] >> 56);
        out[i * 8U + 1U] = (uint8_t)(c->h[i] >> 48);
        out[i * 8U + 2U] = (uint8_t)(c->h[i] >> 40);
        out[i * 8U + 3U] = (uint8_t)(c->h[i] >> 32);
        out[i * 8U + 4U] = (uint8_t)(c->h[i] >> 24);
        out[i * 8U + 5U] = (uint8_t)(c->h[i] >> 16);
        out[i * 8U + 6U] = (uint8_t)(c->h[i] >> 8);
        out[i * 8U + 7U] = (uint8_t)c->h[i];
    }
}

void sha512_bytes(const void *data, uint32_t len, uint8_t out[64])
{
    sha512_ctx c;

    if (out == 0) return;
    sha512_init(&c);
    if (data != 0 && len > 0U) sha512_update(&c, (const uint8_t *)data, len);
    sha512_final(&c, out);
}

/* ---- HMAC-SHA512 前缀状态：ipad/opad 各压缩一个块后保存 ---- */

static void hmac_prepare(const uint8_t *key, uint32_t keylen,
                         sha512_ctx *inner, sha512_ctx *outer)
{
    uint8_t k[128];
    uint8_t pad[128];

    zero_bytes(k, sizeof(k));
    if (key != 0 && keylen > 0U) {
        if (keylen > 128U) {
            sha512_bytes(key, keylen, k);       /* 超长 key 先哈希（本系统不会走到） */
        } else {
            copy_bytes(k, key, keylen);
        }
    }

    for (uint32_t i = 0; i < 128U; i++) pad[i] = (uint8_t)(k[i] ^ 0x36U);
    sha512_init(inner);
    sha512_update(inner, pad, 128U);

    for (uint32_t i = 0; i < 128U; i++) pad[i] = (uint8_t)(k[i] ^ 0x5cU);
    sha512_init(outer);
    sha512_update(outer, pad, 128U);

    zero_bytes(k, sizeof(k));
    zero_bytes(pad, sizeof(pad));
}

/* 对 64 字节消息做一次 HMAC：inner/outer 是已经吃进 ipad/opad 的状态 */
static void hmac_block(const sha512_ctx *inner, const sha512_ctx *outer,
                       const uint8_t msg[64], uint8_t out[64])
{
    sha512_ctx a = *inner;
    sha512_ctx b = *outer;

    sha512_update(&a, msg, 64U);
    sha512_final(&a, out);
    sha512_update(&b, out, 64U);
    sha512_final(&b, out);
}

void pbkdf2_sha512(const uint8_t *password, uint32_t pwlen,
                   const uint8_t *salt, uint32_t saltlen,
                   uint32_t iterations, uint8_t *out, uint32_t outlen)
{
    sha512_ctx inner;
    sha512_ctx outer;
    uint8_t u[64];
    uint8_t t[64];
    uint8_t idx[4];

    if (out == 0 || outlen == 0U) return;
    if (iterations == 0U) iterations = 1U;
    if (outlen > 64U) outlen = 64U;

    hmac_prepare(password, pwlen, &inner, &outer);

    /* U1 = HMAC(P, salt || INT_BE(1)) */
    idx[0] = 0; idx[1] = 0; idx[2] = 0; idx[3] = 1;
    {
        sha512_ctx a = inner;

        if (salt != 0 && saltlen > 0U) sha512_update(&a, salt, saltlen);
        sha512_update(&a, idx, 4U);
        sha512_final(&a, u);
        {
            sha512_ctx b = outer;

            sha512_update(&b, u, 64U);
            sha512_final(&b, u);
        }
    }
    copy_bytes(t, u, 64U);

    for (uint32_t i = 1U; i < iterations; i++) {
        hmac_block(&inner, &outer, u, u);
        for (uint32_t j = 0; j < 64U; j++) t[j] ^= u[j];
    }

    copy_bytes(out, t, outlen);

    zero_bytes(u, sizeof(u));
    zero_bytes(t, sizeof(t));
    zero_bytes(&inner, sizeof(inner));
    zero_bytes(&outer, sizeof(outer));
}
