/* =============================================================================
 * core/trust.c - RSA-2048 / SHA-256 PKCS#1 v1.5 镜像签名验证
 *
 * 构建时由 sign_image.ps1 用 OpenSSL 对 user_shell.elf 签名；
 * 内核在安装/执行 shell 前验证签名。签名数组由 obj/shell_signature.c 提供。
 *
 * 这是教学用最小实现：RSA 模幂用 2048 位大整数做二进制长除法，
 * 密码学强度来自 RSA-2048，不追求常数时间或侧信道防护。
 * ============================================================================= */
#include "trust.h"
#ifdef BOOT_VERIFY
/* 启动验证器是 freestanding 32 位 flat binary，不依赖内核库。 */
void *memcpy(void *dst, const void *src, uint32_t n);
void *memset(void *dst, int c, uint32_t n);
int memcmp(const void *a, const void *b, uint32_t n);
#else
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/kernel.h"
#endif

/* -------------------------------------------------------------------------
 * SHA-256
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t used;
} sha256_ctx;

static const uint32_t sha_k[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};

static uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

static void sha256_compress(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;

    for (uint32_t i = 0; i < 16U; i++) {
        w[i] = ((uint32_t)p[i * 4U] << 24) |
               ((uint32_t)p[i * 4U + 1U] << 16) |
               ((uint32_t)p[i * 4U + 2U] << 8) |
               (uint32_t)p[i * 4U + 3U];
    }
    for (uint32_t i = 16U; i < 64U; i++) {
        uint32_t s0 = rotr32(w[i - 15U], 7) ^ rotr32(w[i - 15U], 18) ^ (w[i - 15U] >> 3);
        uint32_t s1 = rotr32(w[i - 2U], 17) ^ rotr32(w[i - 2U], 19) ^ (w[i - 2U] >> 10);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for (uint32_t i = 0; i < 64U; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + sha_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6A09E667U; c->h[1] = 0xBB67AE85U;
    c->h[2] = 0x3C6EF372U; c->h[3] = 0xA54FF53AU;
    c->h[4] = 0x510E527FU; c->h[5] = 0x9B05688CU;
    c->h[6] = 0x1F83D9ABU; c->h[7] = 0x5BE0CD19U;
    c->len = 0; c->used = 0;
}

static void sha256_update(sha256_ctx *c, const uint8_t *data, uint32_t len)
{
    c->len += len;
    while (len > 0U) {
        uint32_t n = 64U - c->used;
        if (n > len) n = len;
        memcpy(c->buf + c->used, data, n);
        c->used += n;
        data += n;
        len -= n;
        if (c->used == 64U) {
            sha256_compress(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8U;
    uint8_t pad[64];
    uint8_t lenbuf[8];
    uint32_t padlen = (c->used < 56U) ? (56U - c->used) : (120U - c->used);

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80U;
    sha256_update(c, pad, padlen);

    for (uint32_t i = 0; i < 8U; i++) {
        lenbuf[7U - i] = (uint8_t)(bits >> (i * 8U));
    }
    sha256_update(c, lenbuf, 8U);

    for (uint32_t i = 0; i < 8U; i++) {
        out[i * 4U]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4U + 1U] = (uint8_t)(c->h[i] >> 16);
        out[i * 4U + 2U] = (uint8_t)(c->h[i] >> 8);
        out[i * 4U + 3U] = (uint8_t)c->h[i];
    }
}

static void sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    sha256_ctx c;

    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* -------------------------------------------------------------------------
 * RSA-2048 公钥运算：s^65537 mod n
 * ------------------------------------------------------------------------- */
#define RSA_LIMBS 64

typedef struct { uint32_t v[RSA_LIMBS]; } rsa_num;

static void bytes_to_num(const uint8_t in[256], rsa_num *n)
{
    for (uint32_t i = 0; i < RSA_LIMBS; i++) {
        uint32_t off = (RSA_LIMBS - 1U - i) * 4U;
        n->v[i] = ((uint32_t)in[off] << 24) |
                  ((uint32_t)in[off + 1U] << 16) |
                  ((uint32_t)in[off + 2U] << 8) |
                  (uint32_t)in[off + 3U];
    }
}

static void num_to_bytes(const rsa_num *n, uint8_t out[256])
{
    for (uint32_t i = 0; i < RSA_LIMBS; i++) {
        uint32_t off = (RSA_LIMBS - 1U - i) * 4U;
        out[off]     = (uint8_t)(n->v[i] >> 24);
        out[off + 1U] = (uint8_t)(n->v[i] >> 16);
        out[off + 2U] = (uint8_t)(n->v[i] >> 8);
        out[off + 3U] = (uint8_t)n->v[i];
    }
}

static int num_cmp(const rsa_num *a, const rsa_num *b)
{
    for (int i = RSA_LIMBS - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) return (a->v[i] > b->v[i]) ? 1 : -1;
    }
    return 0;
}

/* rem[0..64] 与 n[0..63]（n[64]=0）比较 */
static int rem_cmp65(const uint32_t rem[65], const rsa_num *n)
{
    if (rem[64] != 0U) return 1;
    for (int i = RSA_LIMBS - 1; i >= 0; i--) {
        if (rem[i] != n->v[i]) return (rem[i] > n->v[i]) ? 1 : -1;
    }
    return 0;
}

static void rem_sub65(uint32_t rem[65], const rsa_num *n)
{
    uint64_t borrow = 0;

    for (uint32_t i = 0; i < 65U; i++) {
        uint64_t sub = (uint64_t)((i < RSA_LIMBS) ? n->v[i] : 0U) + borrow;
        uint64_t old = rem[i];
        rem[i] = (uint32_t)(old - sub);
        borrow = (old < sub) ? 1U : 0U;
    }
}

/* a * b mod n，使用 4096 位乘法和逐位长除。 */
static void mulmod(const rsa_num *a, const rsa_num *b,
                   const rsa_num *n, rsa_num *out)
{
    uint32_t prod[128];
    uint32_t rem[65];

    memset(prod, 0, sizeof(prod));
    memset(rem, 0, sizeof(rem));

    for (uint32_t i = 0; i < RSA_LIMBS; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < RSA_LIMBS; j++) {
            uint64_t t = (uint64_t)a->v[i] * b->v[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        uint32_t k = i + RSA_LIMBS;
        while (carry != 0U) {
            uint64_t t = (uint64_t)prod[k] + carry;
            prod[k] = (uint32_t)t;
            carry = t >> 32;
            k++;
        }
    }

    for (int bit = 4095; bit >= 0; bit--) {
        uint32_t carry = (prod[(uint32_t)bit >> 5] >> ((uint32_t)bit & 31U)) & 1U;

        for (uint32_t j = 0; j < 65U; j++) {
            uint32_t next = rem[j] >> 31;
            rem[j] = (rem[j] << 1) | carry;
            carry = next;
        }
        if (rem_cmp65(rem, n) >= 0) {
            rem_sub65(rem, n);
        }
    }

    for (uint32_t i = 0; i < RSA_LIMBS; i++) out->v[i] = rem[i];
}

static void modexp65537(const rsa_num *base, const rsa_num *n, rsa_num *out)
{
    rsa_num result, b;
    uint32_t e = 65537U;

    memset(&result, 0, sizeof(result));
    result.v[0] = 1U;
    b = *base;

    while (e != 0U) {
        if ((e & 1U) != 0U) {
            mulmod(&result, &b, n, &result);
        }
        e >>= 1;
        if (e != 0U) {
            mulmod(&b, &b, n, &b);
        }
    }
    *out = result;
}

static int rsa_verify_sha256(const uint8_t hash[32],
                             const uint8_t signature[256],
                             const uint8_t modulus[256])
{
    static const uint8_t digest_info[19] = {
        0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86,
        0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
        0x00, 0x04, 0x20
    };
    rsa_num n, s, em;
    uint8_t em_bytes[256];
    uint8_t expected[256];

    bytes_to_num(modulus, &n);
    bytes_to_num(signature, &s);
    if (num_cmp(&s, &n) >= 0) return 0;

    modexp65537(&s, &n, &em);
    num_to_bytes(&em, em_bytes);

    memset(expected, 0, sizeof(expected));
    expected[0] = 0x00U;
    expected[1] = 0x01U;
    memset(expected + 2, 0xFF, 202U);
    expected[204] = 0x00U;
    memcpy(expected + 205, digest_info, sizeof(digest_info));
    memcpy(expected + 224, hash, 32U);

    return memcmp(em_bytes, expected, sizeof(expected)) == 0;
}

int trust_verify_image(const uint8_t *image, uint32_t size,
                       const uint8_t *signature, const uint8_t *modulus)
{
    uint8_t hash[32];

    if (image == 0 || size == 0U || signature == 0 || modulus == 0) return 0;
    sha256(image, size, hash);
    return rsa_verify_sha256(hash, signature, modulus);
}

/* -------------------------------------------------------------------------
 * 启动自检（仅内核版本）
 * ------------------------------------------------------------------------- */
#ifndef BOOT_VERIFY
extern uint8_t user_shell_blob_start[];
extern uint8_t user_shell_blob_end[];
extern const uint8_t shell_signature[256];
extern const uint8_t shell_modulus[256];

int trust_verify_shell(const uint8_t *image, uint32_t size)
{
    return trust_verify_image(image, size, shell_signature, shell_modulus);
}

void trust_selftest(void)
{
    uint32_t size = (uint32_t)(uintptr_t)(user_shell_blob_end - user_shell_blob_start);
    int valid = trust_verify_image(user_shell_blob_start, size,
                                   shell_signature, shell_modulus);
    int tampered_rejected = 0;

    if (valid) {
        kprintln("trust: shell signature OK");
    } else {
        kprintln("trust: shell signature INVALID");
    }

    uint8_t *copy = (uint8_t *)malloc(size);
    if (copy != 0) {
        memcpy(copy, user_shell_blob_start, size);
        copy[size / 2U] ^= 0x01U;
        tampered_rejected = !trust_verify_image(copy, size,
                                                shell_signature, shell_modulus);
        free(copy);
    }

    if (tampered_rejected) {
        kprintln("trust: tampered image rejected");
    } else {
        kprintln("trust: tamper check FAILED");
    }

    if (!valid || !tampered_rejected) {
        panic("trust: signature self-test failed");
    }
}
#endif
