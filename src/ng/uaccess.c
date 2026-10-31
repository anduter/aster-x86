/*
 * ng WP1-7: copyin/copyout 实现（页级校验 + SMAP 窗口）。
 *
 * 校验和拷贝分两步：先把整个范围逐页走一遍当前页表，任何一页不满足就
 * 立刻返回 EFAULT（不触碰内存）；通过后才在 stac/clac 里逐字节拷贝。
 * 拷贝期间不会缺页（页已 present 且可写位已校验），因此坏指针永远不会
 * 把内核带进 #PF。
 */
#include "uaccess.h"

#include "io.h"
#include "paging.h"

/* x86-64 低半区用户地址上限（canonical lower half）。 */
#define UACCESS_USER_VA_MAX 0x00007FFFFFFFFFFFULL

static int user_page_ok(u64 *pml4, u64 page, int write)
{
    u64 pte = pg_walk_in(pml4, page);

    if ((pte & PG_PRESENT) == 0 || (pte & PG_USER) == 0) {
        return 0;
    }
    if (write && (pte & PG_WRITE) == 0) {
        return 0;
    }
    return 1;
}

int uaccess_range_ok(u64 uva, u64 len, int write)
{
    u64 *pml4 = pg_current_pml4();
    u64 last;
    u64 page;
    u64 last_page;

    if (pml4 == NULL || uva == 0 || len == 0) {
        return 0;
    }
    last = uva + len - 1;
    if (last < uva || last > UACCESS_USER_VA_MAX) {
        return 0;
    }
    page = uva & ~0xFFFULL;
    last_page = last & ~0xFFFULL;
    for (;;) {
        if (!user_page_ok(pml4, page, write)) {
            return 0;
        }
        if (page >= last_page) {
            break;
        }
        page += 0x1000ULL;
    }
    return 1;
}

static void copy_bytes(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

int copyin(void *dst, u64 uva, u64 len)
{
    if (dst == NULL || !uaccess_range_ok(uva, len, 0)) {
        return UACCESS_EFAULT;
    }
    if (pg_smap_on()) {
        stac();
    }
    copy_bytes(dst, (const void *)(usize)uva, len);
    if (pg_smap_on()) {
        clac();
    }
    return UACCESS_OK;
}

int copyout(u64 uva, const void *src, u64 len)
{
    if (src == NULL || !uaccess_range_ok(uva, len, 1)) {
        return UACCESS_EFAULT;
    }
    if (pg_smap_on()) {
        stac();
    }
    copy_bytes((void *)(usize)uva, src, len);
    if (pg_smap_on()) {
        clac();
    }
    return UACCESS_OK;
}

int copyin_str(char *dst, u64 uva, u64 max)
{
    u64 i;

    if (dst == NULL || max == 0 || uva == 0) {
        return UACCESS_EFAULT;
    }
    for (i = 0; i + 1 < max; i++) {
        u8 c = 0;

        if (copyin(&c, uva + i, 1) != UACCESS_OK) {
            dst[i] = '\0';
            return UACCESS_EFAULT;
        }
        dst[i] = (char)c;
        if (c == 0) {
            return (int)i;
        }
    }
    dst[max - 1] = '\0';
    return (int)(max - 1);
}
