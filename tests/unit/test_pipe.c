/* =============================================================================
 * tests/unit/test_pipe.c - fs/pipe.c（环形缓冲）host 单元测试
 *
 * 编译时通过 tests/unit/shim 提供最小 kernel/proc 接口：
 * 自旋锁与 proc_block/proc_wakeup 退化为 no-op（单线程、不阻塞）。
 * 因此测试只会在"有数据可读"或"写端全关"时读管道，避免真实内核里会阻塞的路径。
 * ========================================================================== */
#include "unit.h"
#include "include/proc.h"
#include "../../src/kernel/fs/pipe.h"

#define PIPE_CAP 1024U

/* pipe.c 里的 memset 被 -Dmemset=unit_memset 改名后由这里提供 */
void *unit_memset(void *dst, int c, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--) { *d++ = (unsigned char)c; }
    return dst;
}

void proc_block(unsigned int chan) { (void)chan; }
void proc_wakeup(unsigned int chan) { (void)chan; }

int main(void)
{
    int idx[8];
    int extra;
    int i;
    char out[1600];
    char expect[1600];
    char chunk[800];
    struct process child;

    UNIT_BEGIN("pipe/ring");

    for (i = 0; i < 8; i++) {
        idx[i] = pipe_create();
        CHECK(idx[i] >= 0);
        CHECK_U32(idx[i], i);              /* 首次分配按槽位顺序 */
    }
    extra = pipe_create();
    CHECK(extra == -1);                    /* PIPE_MAX = 8 */

    /* 非法下标 */
    CHECK(pipe_write(-1, "x", 1) == -1);
    CHECK(pipe_write(8, "x", 1) == -1);
    CHECK(pipe_read(-1, out, 1) == -1);
    CHECK(pipe_read(8, out, 1) == -1);

    /* 基本写读 + 部分读 */
    CHECK(pipe_write(0, "hello", 5) == 5);
    CHECK(pipe_read(0, out, 5) == 5);
    CHECK_MEM(out, "hello", 5);
    CHECK(pipe_write(0, "abcdef", 6) == 6);
    CHECK(pipe_read(0, out, 2) == 2);
    CHECK_MEM(out, "ab", 2);
    CHECK(pipe_read(0, out, 4) == 4);
    CHECK_MEM(out, "cdef", 4);

    /* 环形回绕：写 800 -> 读 700 -> 写 700 -> 读剩下的 800 */
    for (i = 0; i < 800; i++) { chunk[i] = (char)(i % 251); }
    CHECK(pipe_write(0, chunk, 800) == 800);
    CHECK(pipe_read(0, out, 700) == 700);
    CHECK_MEM(out, chunk, 700);
    for (i = 0; i < 700; i++) { chunk[i] = (char)(0x80 + (i % 100)); }
    CHECK(pipe_write(0, chunk, 700) == 700);
    for (i = 0; i < 100; i++) { expect[i] = (char)((700 + i) % 251); }
    for (i = 0; i < 700; i++) { expect[100 + i] = chunk[i]; }
    CHECK(pipe_read(0, out, 800) == 800);
    CHECK_MEM(out, expect, 800);

    /* 写满 1KB 后不阻塞的路径：正好写满，再全部读走 */
    {
        char big[PIPE_CAP];
        char back[PIPE_CAP];
        for (i = 0; i < (int)PIPE_CAP; i++) { big[i] = (char)(i ^ 0x5A); }
        CHECK(pipe_write(0, big, PIPE_CAP) == (int)PIPE_CAP);
        CHECK(pipe_read(0, back, PIPE_CAP) == (int)PIPE_CAP);
        CHECK_MEM(back, big, PIPE_CAP);
    }

    /* EOF：写端全关 + 空缓冲 -> 读返回 0；随后槽位可回收 */
    pipe_unref_write(1);
    CHECK(pipe_read(1, out, 16) == 0);
    pipe_unref_read(1);
    CHECK(pipe_read(1, out, 16) == -1);    /* 槽位已回收 */

    /* fork 继承写端引用计数：两次 unref 才 EOF */
    memset(&child, 0, sizeof(child));
    child.fds[0].used = true;
    child.fds[0].kind = FD_KIND_PIPE_W;
    child.fds[0].handle = 2;
    pipe_fork_inherit(&child);
    pipe_unref_write(2);                   /* writers: 2 -> 1 */
    CHECK(pipe_write(2, "x", 1) == 1);
    CHECK(pipe_read(2, out, 1) == 1);
    CHECK_MEM(out, "x", 1);
    pipe_unref_write(2);                   /* writers: 1 -> 0 */
    CHECK(pipe_read(2, out, 16) == 0);     /* EOF */

    /* 槽位复用：回收 3 后再 create 拿回最小空闲槽位（1 在上面已回收） */
    pipe_unref_write(3);
    CHECK(pipe_read(3, out, 1) == 0);
    pipe_unref_read(3);
    CHECK_U32(pipe_create(), 1);

    /* fork_inherit(NULL) 必须安全 */
    pipe_fork_inherit(0);

    UNIT_END();
}