/* =============================================================================
 * tests/unit/test_trust.c - security/trust.c 的 host 单元测试
 *
 * 编译：-DBOOT_VERIFY（该模式下 trust.c 不依赖内核库），并把
 * memcpy/memset/memcmp 改名后由本文件提供最小实现。
 *
 * 覆盖：SHA-256 标准向量 + 分组边界、sha256_bytes 空输入、RSA-2048
 * PKCS#1 v1.5 验签的正/负样例（夹具见 fixtures/signature_fixture.h）。
 * ========================================================================== */
#include "unit.h"
#include "fixtures/signature_fixture.h"
#include "../../src/kernel/security/trust.h"

/* trust.c 在 BOOT_VERIFY 下声明这三个符号；编译时已改名，避免与 libc 冲突 */
void *unit_memcpy(void *dst, const void *src, uint32_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--) { *d++ = *s++; }
    return dst;
}

void *unit_memset(void *dst, int c, uint32_t n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--) { *d++ = (unsigned char)c; }
    return dst;
}

int unit_memcmp(const void *a, const void *b, uint32_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (n--) {
        if (*x != *y) { return (int)*x - (int)*y; }
        x++;
        y++;
    }
    return 0;
}

static const unsigned char sha_empty[32] = {
    0xE3, 0xB0, 0xC4, 0x42, 0x98, 0xFC, 0x1C, 0x14,
    0x9A, 0xFB, 0xF4, 0xC8, 0x99, 0x6F, 0xB9, 0x24,
    0x27, 0xAE, 0x41, 0xE4, 0x64, 0x9B, 0x93, 0x4C,
    0xA4, 0x95, 0x99, 0x1B, 0x78, 0x52, 0xB8, 0x55
};

static const unsigned char sha_abc[32] = {
    0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
    0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
    0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
    0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
};

/* 56 字节：刚好落在 SHA-256 填充的边界上 */
static const unsigned char sha_56[32] = {
    0x24, 0x8D, 0x6A, 0x61, 0xD2, 0x06, 0x38, 0xB8,
    0xE5, 0xC0, 0x26, 0x93, 0x0C, 0x3E, 0x60, 0x39,
    0xA3, 0x3C, 0xE4, 0x59, 0x64, 0xFF, 0x21, 0x67,
    0xF6, 0xEC, 0xED, 0xD4, 0x19, 0xDB, 0x06, 0xC1
};

static const unsigned char sha_100[32] = {
    0xB1, 0x3B, 0x0B, 0x84, 0x1D, 0x6F, 0xDA, 0xBB,
    0xDC, 0x7D, 0xDD, 0xC8, 0x14, 0x16, 0x19, 0x63,
    0xD8, 0x5D, 0xC5, 0xC1, 0xA6, 0x22, 0x81, 0x00,
    0xBA, 0x27, 0xA4, 0x93, 0x23, 0xF6, 0x87, 0x87
};

static const unsigned char sha_a64[32] = {
    0xFF, 0xE0, 0x54, 0xFE, 0x7A, 0xE0, 0xCB, 0x6D,
    0xC6, 0x5C, 0x3A, 0xF9, 0xB6, 0x1D, 0x52, 0x09,
    0xF4, 0x39, 0x85, 0x1D, 0xB4, 0x3D, 0x0B, 0xA5,
    0x99, 0x73, 0x37, 0xDF, 0x15, 0x46, 0x68, 0xEB
};

int main(void)
{
    static const char msg56[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    /* "Aster-" x 15 + "0123456789" = 90 + 10 = 100 字节 */
    static const char aster100[] =
        "Aster-Aster-Aster-Aster-Aster-Aster-Aster-Aster-Aster-Aster-"
        "Aster-Aster-Aster-Aster-Aster-0123456789";
    char a64[65];
    unsigned char out[32];
    unsigned char tampered[sizeof(fixture_msg)];
    unsigned char bad_sig[256];
    unsigned char bad_mod[256];
    unsigned int i;

    UNIT_BEGIN("trust/sha256");

    sha256_bytes("", 0, out);
    CHECK_MEM(out, sha_empty, 32);
    sha256_bytes(0, 0, out);
    CHECK_MEM(out, sha_empty, 32);

    sha256_bytes("abc", 3, out);
    CHECK_MEM(out, sha_abc, 32);

    sha256_bytes(msg56, (uint32_t)sizeof(msg56) - 1U, out);
    CHECK_MEM(out, sha_56, 32);

    CHECK_U32(sizeof(aster100), 101);
    sha256_bytes(aster100, 100, out);
    CHECK_MEM(out, sha_100, 32);

    for (i = 0; i < 64U; i++) { a64[i] = 'a'; }
    a64[64] = '\0';
    sha256_bytes(a64, 64, out);
    CHECK_MEM(out, sha_a64, 32);

    printf("\n-- trust/rsa --\n");
    CHECK_U32(sizeof(fixture_msg), 22);
    CHECK(fixture_modulus[0] != 0x00);          /* RSA-2048 模数最高字节非零 */
    CHECK(trust_verify_image(fixture_msg, (uint32_t)sizeof(fixture_msg),
                             fixture_signature, fixture_modulus) == 1);

    /* 篡改消息：必须拒绝 */
    for (i = 0; i < sizeof(fixture_msg); i++) { tampered[i] = fixture_msg[i]; }
    tampered[0] ^= 0x01U;
    CHECK(trust_verify_image(tampered, (uint32_t)sizeof(tampered),
                             fixture_signature, fixture_modulus) == 0);

    /* 篡改签名：必须拒绝 */
    for (i = 0; i < 256U; i++) { bad_sig[i] = fixture_signature[i]; }
    bad_sig[255] ^= 0x01U;
    CHECK(trust_verify_image(fixture_msg, (uint32_t)sizeof(fixture_msg),
                             bad_sig, fixture_modulus) == 0);

    /* 篡改模数：必须拒绝（或至少不返回 1） */
    for (i = 0; i < 256U; i++) { bad_mod[i] = fixture_modulus[i]; }
    bad_mod[255] ^= 0x01U;
    CHECK(trust_verify_image(fixture_msg, (uint32_t)sizeof(fixture_msg),
                             fixture_signature, bad_mod) == 0);

    /* 参数校验 */
    CHECK(trust_verify_image(0, 22, fixture_signature, fixture_modulus) == 0);
    CHECK(trust_verify_image(fixture_msg, 0, fixture_signature, fixture_modulus) == 0);
    CHECK(trust_verify_image(fixture_msg, 22, 0, fixture_modulus) == 0);
    CHECK(trust_verify_image(fixture_msg, 22, fixture_signature, 0) == 0);

    UNIT_END();
}
