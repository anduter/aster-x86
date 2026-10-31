/*
 * ng WP2-5: SHA-256 实现（FIPS 180-4，L1）。
 *
 * 算法是教科书实现：512 位块 + 64 轮压缩，消息长度按 64 位大端追加。
 * 刻意不用 libc/内建函数，保证内核（-ffreestanding）与 host 单测同源。
 */
#include "sha256.h"

static const u32 g_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static u32 rotr32(u32 x, u32 n)
{
    return (x >> n) | (x << (32u - n));
}

static void store_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 load_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) |
           (u32)p[3];
}

static void sha256_block(struct sha256_ctx *ctx, const u8 *block)
{
    u32 w[64];
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    u32 e;
    u32 f;
    u32 g;
    u32 h;
    u32 i;

    for (i = 0; i < 16u; i++) {
        w[i] = load_be32(block + i * 4u);
    }
    for (i = 16u; i < 64u; i++) {
        u32 s0 = rotr32(w[i - 15u], 7) ^ rotr32(w[i - 15u], 18) ^
                 (w[i - 15u] >> 3);
        u32 s1 = rotr32(w[i - 2u], 17) ^ rotr32(w[i - 2u], 19) ^
                 (w[i - 2u] >> 10);

        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64u; i++) {
        u32 s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 t1 = h + s1 + ch + g_k[i] + w[i];
        u32 s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, u64 len)
{
    const u8 *p = (const u8 *)data;
    u64 left;

    if (ctx == NULL || (data == NULL && len != 0)) {
        return;
    }
    ctx->bitlen += len * 8u;
    left = len;

    while (left > 0) {
        u32 take = (u32)(SHA256_BLOCK_BYTES - ctx->buflen);

        if ((u64)take > left) {
            take = (u32)left;
        }
        {
            u32 i;

            for (i = 0; i < take; i++) {
                ctx->buf[ctx->buflen + i] = p[i];
            }
        }
        ctx->buflen += take;
        p += take;
        left -= take;
        if (ctx->buflen == SHA256_BLOCK_BYTES) {
            sha256_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *ctx, u8 out[SHA256_DIGEST_BYTES])
{
    u64 bits;
    u32 i;

    if (ctx == NULL || out == NULL) {
        return;
    }
    bits = ctx->bitlen;

    /* 0x80，然后补零到 56 mod 64，最后 8 字节是大端位长。 */
    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56u) {
        while (ctx->buflen < SHA256_BLOCK_BYTES) {
            ctx->buf[ctx->buflen++] = 0;
        }
        sha256_block(ctx, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56u) {
        ctx->buf[ctx->buflen++] = 0;
    }
    for (i = 0; i < 8u; i++) {
        ctx->buf[56u + i] = (u8)(bits >> (56u - i * 8u));
    }
    sha256_block(ctx, ctx->buf);
    ctx->buflen = 0;

    for (i = 0; i < 8u; i++) {
        store_be32(out + i * 4u, ctx->state[i]);
    }
}

void sha256(const void *data, u64 len, u8 out[SHA256_DIGEST_BYTES])
{
    struct sha256_ctx ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void sha256_hex(const u8 digest[SHA256_DIGEST_BYTES],
                char out[SHA256_HEX_MAX])
{
    static const char digits[] = "0123456789abcdef";
    u32 i;

    if (digest == NULL || out == NULL) {
        return;
    }
    for (i = 0; i < SHA256_DIGEST_BYTES; i++) {
        out[i * 2u] = digits[(digest[i] >> 4) & 0xFu];
        out[i * 2u + 1u] = digits[digest[i] & 0xFu];
    }
    out[SHA256_HEX_BYTES] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    return -1;
}

int sha256_hex_valid(const char *hex)
{
    u32 i;

    if (hex == NULL) {
        return 0;
    }
    for (i = 0; i < SHA256_HEX_BYTES; i++) {
        if (hex_val(hex[i]) < 0) {
            return 0;
        }
    }
    return hex[SHA256_HEX_BYTES] == '\0';
}

int sha256_hex_parse(const char *hex, u8 out[SHA256_DIGEST_BYTES])
{
    u32 i;

    if (!sha256_hex_valid(hex) || out == NULL) {
        return 0;
    }
    for (i = 0; i < SHA256_DIGEST_BYTES; i++) {
        out[i] = (u8)((hex_val(hex[i * 2u]) << 4) | hex_val(hex[i * 2u + 1u]));
    }
    return 1;
}
