/* =============================================================================
 * tests/unit/test_fuzz_pipe.c - pipe.c 环形缓冲的随机读写循环 + 影子队列校验
 *
 * 每步随机选择 write/read，长度受"当前占用/空闲"约束（避免真实内核会阻塞的路径），
 * 用影子缓冲区校验读出的字节顺序；另外随机抽查非法下标必须返回 -1。
 * 固定种子 -> 可复现。
 * ========================================================================== */
#include "unit.h"
#include "include/proc.h"
#include "../../src/kernel/fs/pipe.h"

#define CAP 1024U

void *unit_memset(void *dst, int c, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--) { *d++ = (unsigned char)c; }
    return dst;
}

void proc_block(unsigned int chan) { (void)chan; }
void proc_wakeup(unsigned int chan) { (void)chan; }

static uint32_t fuzz_state = 0x12345678u;

static uint32_t rnd(void)
{
    fuzz_state ^= fuzz_state << 13;
    fuzz_state ^= fuzz_state >> 17;
    fuzz_state ^= fuzz_state << 5;
    return fuzz_state;
}

int main(void)
{
    static unsigned char shadow[CAP];
    static char out[CAP];
    static char in[CAP];
    uint32_t head = 0, tail = 0, count = 0;
    unsigned int bad = 0;
    int idx;
    unsigned int i;

    UNIT_BEGIN("fuzz/pipe");

    idx = pipe_create();
    CHECK(idx >= 0);

    for (i = 0; i < 4000U; i++) {
        uint32_t freeBytes = CAP - count;
        uint32_t op = rnd() % 100U;

        if (op < 55U && freeBytes > 0U) {
            uint32_t n = 1U + (rnd() % (freeBytes < 64U ? freeBytes : 64U));
            uint32_t k;

            for (k = 0; k < n; k++) { in[k] = (char)rnd(); }
            if (pipe_write(idx, in, n) != (int)n) {
                printf("FAIL write #%u n=%u\n", i, n);
                bad++;
                break;
            }
            for (k = 0; k < n; k++) {
                shadow[(head + k) % CAP] = (unsigned char)in[k];
            }
            head = (head + n) % CAP;
            count += n;
        } else if (op < 95U && count > 0U) {
            uint32_t n = 1U + (rnd() % (count < 64U ? count : 64U));
            uint32_t k;

            if (pipe_read(idx, out, n) != (int)n) {
                printf("FAIL read #%u n=%u\n", i, n);
                bad++;
                break;
            }
            for (k = 0; k < n; k++) {
                if ((unsigned char)out[k] != shadow[(tail + k) % CAP]) {
                    printf("FAIL data #%u at %u: %02x != %02x\n",
                           i, k, (unsigned char)out[k], shadow[(tail + k) % CAP]);
                    bad++;
                    break;
                }
            }
            tail = (tail + n) % CAP;
            count -= n;
        } else {
            /* 非法下标：不能崩、不能改状态 */
            if (pipe_write(-1, "x", 1) != -1 || pipe_write(8, "x", 1) != -1) {
                printf("FAIL invalid write index #%u\n", i);
                bad++;
                break;
            }
            if (pipe_read(-1, out, 1) != -1 || pipe_read(8, out, 1) != -1) {
                printf("FAIL invalid read index #%u\n", i);
                bad++;
                break;
            }
        }
    }
    CHECK_U32(bad, 0U);
    CHECK(count <= CAP);

    /* 收尾：把剩余数据全部读走并核对 */
    if (count > 0U) {
        uint32_t k;
        CHECK(pipe_read(idx, out, count) == (int)count);
        for (k = 0; k < count; k++) {
            if ((unsigned char)out[k] != shadow[(tail + k) % CAP]) {
                printf("FAIL drain at %u\n", k);
                bad++;
                break;
            }
        }
        CHECK_U32(bad, 0U);
    }

    UNIT_END();
}
