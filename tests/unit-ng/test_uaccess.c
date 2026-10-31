/*
 * ng WP1-8 host 单测：uaccess.c（copyin/copyout/copyin_str）与页边界。
 *
 * 页表由 fake_ng_env.c 提供：测试把宿主的静态缓冲区 page 登记成 U=1/W=1 的
 * “用户页”，uaccess_range_ok() 的逐页校验走 fake 页表，而真正的字节拷贝发生
 * 在宿主内存上。于是可以在不跑 QEMU 的情况下把下面这些边界钉住：
 *
 *   - 参数非法：uva=0 / len=0 / 末地址溢出 / 超过 0x00007FFF_FFFF_FFFF；
 *   - 页级权限：U=0（内核页）、W=0（只读页）在 copyout 时被拒；
 *   - 跨页：范围跨入未映射页时整体 EFAULT，且内核缓冲一个字节都不被写；
 *   - 跨页成功：两个相邻映射页边界的 8 字节拷贝内容正确；
 *   - copyin_str：正常串、无 NUL 截断、max=0/1、坏指针、跨页坏指针。
 */
#include <string.h>

#include "fake_ng_env.h"
#include "paging.h"
#include "test_util.h"
#include "uaccess.h"

#define PG_SIZE 4096ULL

static u8 arena[6 * PG_SIZE] __attribute__((aligned(4096)));
static u64 base; /* arena 的 4 KiB 对齐首地址 */

static void reset_arena(void)
{
    fake_pg_reset();
    memset(arena, 0, sizeof(arena));
    base = ((u64)(usize)arena + (PG_SIZE - 1)) & ~(PG_SIZE - 1);
}

/* ------------------------------------------------------------------------- */
/* 1. 参数与范围（未映射 / 0 / 溢出 / 用户上界）                              */
/* ------------------------------------------------------------------------- */

static void t_range_args(void)
{
    reset_arena();

    CHECK(uaccess_range_ok(0, 16, 0) == 0);   /* uva=0 */
    CHECK(uaccess_range_ok(base, 0, 0) == 0); /* len=0 */
    CHECK(uaccess_range_ok(base, 16, 0) == 0); /* 未映射 */

    /* 末地址溢出：uva+len-1 回绕。 */
    CHECK(uaccess_range_ok(0xFFFFFFFFFFFFFFFFULL, 2, 0) == 0);
    /* 末地址超过 canonical 低半区用户上限。 */
    CHECK(uaccess_range_ok(0x00007FFFFFFFFFFFULL, 2, 0) == 0);
    CHECK(uaccess_range_ok(0x0000800000000000ULL, 1, 0) == 0);

    /* pml4 = NULL（没有活动地址空间）时一律拒绝。 */
    fake_pg_set_null_pml4(1);
    CHECK(uaccess_range_ok(base, 1, 0) == 0);
    fake_pg_set_null_pml4(0);

    CHECK(fake_pg_map(base, PG_USER | PG_WRITE) == PG_OK);
    CHECK(uaccess_range_ok(base, 1, 0) == 1);
    CHECK(uaccess_range_ok(base, PG_SIZE, 1) == 1);
    CHECK(uaccess_range_ok(base + PG_SIZE - 1, 1, 0) == 1);
    /* 末字节跨进未映射的下一页。 */
    CHECK(uaccess_range_ok(base + PG_SIZE - 1, 2, 0) == 0);
}

/* ------------------------------------------------------------------------- */
/* 2. 拷贝语义                                                                */
/* ------------------------------------------------------------------------- */

static void t_copy(void)
{
    u8 src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    u8 dst[8];

    reset_arena();
    CHECK(fake_pg_map(base, PG_USER | PG_WRITE) == PG_OK);

    memset(dst, 0, sizeof(dst));
    memcpy((void *)(usize)(base + 16), src, sizeof(src));
    CHECK(copyin(dst, base + 16, sizeof(src)) == UACCESS_OK);
    CHECK(memcmp(dst, src, sizeof(src)) == 0);

    memcpy((void *)(usize)(base + 32), src, sizeof(src));
    CHECK(copyout(base + 64, src, sizeof(src)) == UACCESS_OK);
    CHECK(memcmp((void *)(usize)(base + 64), src, sizeof(src)) == 0);

    /* 空缓冲参数。 */
    CHECK(copyin(NULL, base, 4) == UACCESS_EFAULT);
    CHECK(copyout(base, NULL, 4) == UACCESS_EFAULT);

    /* 坏指针时 dst 必须原样（校验先于拷贝）。 */
    memset(dst, 0xAA, sizeof(dst));
    CHECK(copyin(dst, base + PG_SIZE - 3, 8) == UACCESS_EFAULT);
    CHECK(dst[0] == 0xAA && dst[7] == 0xAA);

    /* 跨两个映射页的 8 字节：内容真的跨页正确。 */
    CHECK(fake_pg_map(base + PG_SIZE, PG_USER | PG_WRITE) == PG_OK);
    memset((void *)(usize)(base + PG_SIZE - 4), 0x11, 4);
    memset((void *)(usize)(base + PG_SIZE), 0x22, 4);
    memset(dst, 0, sizeof(dst));
    CHECK(copyin(dst, base + PG_SIZE - 4, 8) == UACCESS_OK);
    CHECK(dst[0] == 0x11 && dst[3] == 0x11);
    CHECK(dst[4] == 0x22 && dst[7] == 0x22);
}

/* ------------------------------------------------------------------------- */
/* 3. 页权限：U=0 / W=0                                                       */
/* ------------------------------------------------------------------------- */

static void t_page_perms(void)
{
    u8 src[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    u8 dst[4];
    u32 i;

    reset_arena();
    CHECK(fake_pg_map(base, PG_USER | PG_WRITE) == PG_OK);
    CHECK(fake_pg_map(base + PG_SIZE, PG_USER) == PG_OK);        /* 只读用户页 */
    CHECK(fake_pg_map(base + 2 * PG_SIZE, 0) == PG_OK);          /* 内核页 U=0 */

    /* 只读用户页：读可以，写（copyout）拒绝。 */
    CHECK(uaccess_range_ok(base + PG_SIZE, 4, 0) == 1);
    CHECK(uaccess_range_ok(base + PG_SIZE, 4, 1) == 0);
    memcpy((void *)(usize)(base + PG_SIZE), src, sizeof(src));
    memset(dst, 0, sizeof(dst));
    CHECK(copyin(dst, base + PG_SIZE, sizeof(dst)) == UACCESS_OK);
    CHECK(memcmp(dst, src, sizeof(src)) == 0);
    CHECK(copyout(base + PG_SIZE, src, sizeof(src)) == UACCESS_EFAULT);
    CHECK(memcmp((void *)(usize)(base + PG_SIZE), src, sizeof(src)) == 0);

    /* 内核页 U=0：读写都拒绝（copyin/copyout 都不该碰它）。 */
    CHECK(uaccess_range_ok(base + 2 * PG_SIZE, 4, 0) == 0);
    CHECK(uaccess_range_ok(base + 2 * PG_SIZE, 4, 1) == 0);
    for (i = 0; i < 4; i++) {
        dst[i] = 0;
    }
    CHECK(copyin(dst, base + 2 * PG_SIZE, 4) == UACCESS_EFAULT);
    CHECK(dst[0] == 0 && dst[3] == 0);

    /* 只读页跨页写：第一页可写、第二页只读，整体照样拒绝。 */
    CHECK(uaccess_range_ok(base + PG_SIZE - 2, 4, 1) == 0);
}

/* ------------------------------------------------------------------------- */
/* 4. copyin_str                                                              */
/* ------------------------------------------------------------------------- */

static void t_copyin_str(void)
{
    char s[16];

    reset_arena();
    CHECK(fake_pg_map(base, PG_USER | PG_WRITE) == PG_OK);

    memcpy((void *)(usize)(base + 64), "ng", 3);
    memset(s, 0x7F, sizeof(s));
    CHECK(copyin_str(s, base + 64, sizeof(s)) == 2);
    CHECK(strcmp(s, "ng") == 0);

    /* 空串：拷贝到 NUL 就停。 */
    CHECK(copyin_str(s, base + 32, sizeof(s)) == 0);
    CHECK(s[0] == '\0');

    /* 无 NUL：返回 max-1，且 dst[max-1] 一定 NUL。 */
    CHECK(fake_pg_map(base + PG_SIZE, PG_USER | PG_WRITE) == PG_OK);
    memset((void *)(usize)(base + PG_SIZE), 'A', 16);
    memset(s, 0, sizeof(s));
    CHECK(copyin_str(s, base + PG_SIZE, 8) == 7);
    CHECK(s[0] == 'A' && s[6] == 'A' && s[7] == '\0');

    /* max=1：只能写 NUL。 */
    CHECK(copyin_str(s, base + PG_SIZE, 1) == 0);
    CHECK(s[0] == '\0');

    /* 坏指针 / 跨页坏指针 / 参数非法。 */
    memset(s, 0x7F, sizeof(s));
    /* page1 末尾两字节非 NUL，第 3 个字节跨进未映射的 page2。 */
    ((u8 *)(usize)(base + 2 * PG_SIZE - 2))[0] = 'X';
    ((u8 *)(usize)(base + 2 * PG_SIZE - 2))[1] = 'Y';
    CHECK(copyin_str(s, base + 2 * PG_SIZE - 2, 8) == UACCESS_EFAULT);
    /* 失败点之前的字节已被拷入，失败点写 NUL（不会漏出未初始化栈内容）。 */
    CHECK(s[0] == 'X' && s[1] == 'Y' && s[2] == '\0');
    CHECK(copyin_str(s, base + 4 * PG_SIZE, 8) == UACCESS_EFAULT);
    CHECK(copyin_str(NULL, base, 8) == UACCESS_EFAULT);
    CHECK(copyin_str(s, base, 0) == UACCESS_EFAULT);
    CHECK(copyin_str(s, 0, 8) == UACCESS_EFAULT);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int test_uaccess(void)
{
    t_range_args();
    t_copy();
    t_page_perms();
    t_copyin_str();
    return 0;
}
