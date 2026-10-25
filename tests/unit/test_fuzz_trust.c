/* =============================================================================
 * tests/unit/test_fuzz_trust.c - trust.c 的确定性随机输入循环（无 libFuzzer 时的替代）
 *
 * 两类输入：
 *   1. 完全随机：image/signature/modulus 三个缓冲区各自随机，要求永不返回 1；
 *   2. 结构化变异：从合法夹具出发，随机翻转 1..8 个 bit（消息/签名/模数），要求拒绝。
 * 固定种子 -> 可复现；失败时打印迭代号与缓冲区，便于回放。
 * ========================================================================== */
#include "unit.h"
#include "fixtures/signature_fixture.h"
#include "../../src/kernel/security/trust.h"

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

static uint32_t fuzz_state = 0x9E3779B9u;

static uint32_t rnd(void)
{
    fuzz_state ^= fuzz_state << 13;
    fuzz_state ^= fuzz_state >> 17;
    fuzz_state ^= fuzz_state << 5;
    return fuzz_state;
}

int main(void)
{
    static unsigned char image[512];
    static unsigned char sig[256];
    static unsigned char mod[256];
    unsigned int i;
    int accepted = 0;

    UNIT_BEGIN("fuzz/trust");

    for (i = 0; i < 300U; i++) {
        uint32_t len = 1U + (rnd() % (uint32_t)sizeof(image));
        unsigned int k;

        for (k = 0; k < len; k++) { image[k] = (unsigned char)rnd(); }
        for (k = 0; k < 256U; k++) { sig[k] = (unsigned char)rnd(); }
        for (k = 0; k < 256U; k++) { mod[k] = (unsigned char)rnd(); }

        if (trust_verify_image(image, len, sig, mod) == 1) {
            accepted++;
            printf("FAIL random input #%u accepted\n", i);
        }
    }
    CHECK_U32(accepted, 0U);

    /* 结构化变异：合法夹具 + 随机 bit flips，必须全部拒绝 */
    accepted = 0;
    for (i = 0; i < 120U; i++) {
        unsigned char m2[sizeof(fixture_msg)];
        unsigned char s2[256];
        unsigned char n2[256];
        unsigned int flips = 1U + (rnd() % 8U);
        unsigned int f;

        for (f = 0; f < sizeof(fixture_msg); f++) { m2[f] = fixture_msg[f]; }
        for (f = 0; f < 256U; f++) { s2[f] = fixture_signature[f]; n2[f] = fixture_modulus[f]; }

        for (f = 0; f < flips; f++) {
            uint32_t which = rnd() % 3U;
            if (which == 0U) {
                uint32_t at = rnd() % (uint32_t)sizeof(fixture_msg);
                m2[at] ^= (unsigned char)(1U << (rnd() % 8U));
            } else if (which == 1U) {
                uint32_t at = rnd() % 256U;
                s2[at] ^= (unsigned char)(1U << (rnd() % 8U));
            } else {
                uint32_t at = rnd() % 256U;
                n2[at] ^= (unsigned char)(1U << (rnd() % 8U));
            }
        }
        if (trust_verify_image(m2, (uint32_t)sizeof(m2), s2, n2) == 1) {
            accepted++;
            printf("FAIL mutated input #%u accepted (flips=%u)\n", i, flips);
        }
    }
    CHECK_U32(accepted, 0U);

    UNIT_END();
}
