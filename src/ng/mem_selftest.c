/*
 * ng WP1-4: 内核侧内存自检（WP1-4 的四条出口自检行的产生处）。
 */
#include "mem_selftest.h"

#include "heap.h"
#include "memmap.h"
#include "paging.h"
#include "phys.h"
#include "pmm.h"
#include "serial.h"

/* 链接脚本给的段边界（见 src/ng/link.ld）。 */
extern char __image_start[];
extern char __kernel_end[];
extern char __fat16_img_start[];
extern char __fat16_img_end[];
extern char __bigdata_start[];
extern char __bigdata_end[];

#define LOW_MEM_RESERVED 0x100000ULL /* 低 1 MiB：IVT/BDA/VGA/引导结构 */

static void print_kv_hex(const char *key, u64 value)
{
    serial_puts(key);
    serial_puthex(value);
}

static void print_kv_dec(const char *key, u64 value)
{
    serial_puts(key);
    serial_putdec(value);
}

/* ------------------------------------------------------------------------- */
/* 启动内存图 -> PMM                                                          */
/* ------------------------------------------------------------------------- */

int mem_boot_init(u64 boot_info)
{
    static ng_memmap map;
    const pmm_stats *st;
    int fails = 0;
    int rc;
    u32 i;
    u32 free_before;
    u64 p = 0;
    u64 q = 0;
    int alloc_ok = 1;
    int double_ok = 0;
    int selftest_ok = 0;

    serial_puts("ng: boot info=");
    serial_puthex(boot_info);
    serial_puts("\r\n");

    rc = memmap_parse(boot_info, &map);
    if (rc != MEMMAP_OK) {
        serial_puts("ng: memmap FAIL (rc=");
        serial_putdec((u64)(-rc));
        serial_puts(")\r\n");
        return 1;
    }

    serial_puts("ng: memmap src=");
    serial_puts(memmap_source_name(map.source));
    print_kv_dec(" entries=", (u64)map.count);
    print_kv_dec(" usable=", map.usable_pages);
    serial_puts(" pages boot=");
    serial_puthex(map.boot_start);
    serial_puts("..");
    serial_puthex(map.boot_end);
    serial_puts("\r\n");
    serial_puts("ng: memmap ok (usable=");
    serial_putdec(map.usable_pages);
    serial_puts(" pages)\r\n");

    rc = pmm_init(0, NG_PHYS_LIMIT);
    if (rc != PMM_OK) {
        serial_puts("ng: pmm init FAIL\r\n");
        return fails + 1;
    }
    for (i = 0; i < map.count; i++) {
        if (map.entries[i].type == MEMMAP_RAM) {
            pmm_add_usable(map.entries[i].addr, map.entries[i].size);
        }
    }

    /* 保留区：低 1 MiB、内核镜像（含 .bss 位图/栈）、引导结构。 */
    pmm_reserve(0, LOW_MEM_RESERVED);
    pmm_reserve((u64)(usize)__image_start,
                (u64)(usize)__kernel_end - (u64)(usize)__image_start);
    /* WP2-2：2 MiB 处的 FAT16 镜像段（独立 PT_LOAD，PMM 不得分配）。 */
    pmm_reserve((u64)(usize)__fat16_img_start,
                (u64)(usize)__fat16_img_end -
                    (u64)(usize)__fat16_img_start);
    /* WP3-3：紧随其后的用户 ELF blob 段（同一个 PD[1] 恒等大页）。 */
    if ((u64)(usize)__bigdata_end > (u64)(usize)__bigdata_start) {
        pmm_reserve((u64)(usize)__bigdata_start,
                    (u64)(usize)__bigdata_end -
                        (u64)(usize)__bigdata_start);
    }
    if (map.boot_end > map.boot_start) {
        pmm_reserve(map.boot_start, map.boot_end - map.boot_start);
    }

    st = pmm_stat();
    print_kv_hex("ng: pmm window=", st->base);
    serial_puts("..");
    serial_puthex(st->limit);
    print_kv_dec(" total=", st->total_pages);
    print_kv_dec(" usable=", st->usable_pages);
    print_kv_dec(" free=", st->free_pages);
    serial_puts("\r\n");

    /* ---- PMM 自检：单页/连续页、账目复原、double-free、保留页、越界 ---- */
    free_before = pmm_free_count();

    p = pmm_alloc_page();
    if (p == 0 || !NG_IS_PAGE_ALIGNED(p) || !pmm_owns(p) || pmm_is_reserved(p)) {
        alloc_ok = 0;
    } else {
        volatile u64 *w = (volatile u64 *)(usize)p;
        w[0] = 0xA5A5C3C35A5A3C3CULL;
        if (w[0] != 0xA5A5C3C35A5A3C3CULL) {
            alloc_ok = 0;
        }
    }

    q = pmm_alloc_pages(4);
    if (q == 0 || !NG_IS_PAGE_ALIGNED(q)) {
        alloc_ok = 0;
    } else {
        u32 k;
        for (k = 0; k < 4; k++) {
            if (!pmm_owns(q + (u64)k * NG_PAGE_SIZE) ||
                pmm_is_reserved(q + (u64)k * NG_PAGE_SIZE)) {
                alloc_ok = 0;
            }
        }
    }

    if (p != 0 && pmm_free_page(p) != PMM_OK) {
        alloc_ok = 0;
    }
    if (q != 0 && pmm_free_pages(q, 4) != PMM_OK) {
        alloc_ok = 0;
    }
    if (pmm_free_count() != free_before) {
        alloc_ok = 0;
    }

    if (alloc_ok) {
        serial_puts("ng: pmm alloc/free ok\r\n");
    } else {
        fails++;
        serial_puts("ng: pmm alloc/free FAIL\r\n");
    }

    /* double free：同一页连 free 两次，第二次必须被拒。 */
    p = pmm_alloc_page();
    if (p != 0) {
        u32 dbl_before = pmm_stat()->reject_double;
        int f1 = pmm_free_page(p);
        int f2 = pmm_free_page(p);

        if (f1 == PMM_OK && f2 == PMM_EDOUBLE &&
            pmm_stat()->reject_double == dbl_before + 1) {
            double_ok = 1;
        }
    }
    if (double_ok) {
        serial_puts("ng: pmm double-free rejected\r\n");
    } else {
        fails++;
        serial_puts("ng: pmm double-free FAIL\r\n");
    }

    /* 保留页（0 号页 = 低 1 MiB）与越界释放也必须被拒。 */
    if (pmm_free_page(0) == PMM_ERESERVED &&
        pmm_free_page(NG_PHYS_LIMIT) == PMM_ERANGE) {
        selftest_ok = 1;
    }
    if (!selftest_ok) {
        fails++;
        serial_puts("ng: pmm range/reserved rejection FAIL\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* heap                                                                       */
/* ------------------------------------------------------------------------- */

int mem_heap_selftest(void)
{
    int fails = 0;
    u8 *e;
    u8 *a;
    u8 *b;
    u8 *c;
    void *al;
    int ok;

    heap_init();

    /* 1) 先做 double-free：第一次分配落在 region 起点，释放后与剩余空闲块
     *    合并（块头仍是自己），第二次释放必须 EDOUBLE。 */
    e = (u8 *)kmalloc(16);
    ok = (e != NULL);
    if (ok) {
        int f1 = kfree(e);
        int f2 = kfree(e);
        ok = (f1 == HEAP_OK &&
              (f2 == HEAP_EDOUBLE || f2 == HEAP_EINVAL) &&
              heap_double_free_count() + heap_bad_free_count() > 0);
    }
    if (!ok) {
        fails++;
        serial_puts("ng: heap double-free FAIL\r\n");
    }

    /* 2) kmalloc + 写入/读回 + 16 字节对齐 + 互不重叠。 */
    a = (u8 *)kmalloc(64);
    b = (u8 *)kmalloc(128);
    c = (u8 *)kmalloc(64);
    ok = a != NULL && b != NULL && c != NULL;
    if (ok) {
        u32 i;
        ok = ((u64)(usize)a & 15ULL) == 0 && ((u64)(usize)b & 15ULL) == 0 &&
             ((u64)(usize)c & 15ULL) == 0;
        ok = ok && a != b && b != c && a != c;
        for (i = 0; i < 64 && ok; i++) {
            a[i] = (u8)(0x10u + i);
        }
        for (i = 0; i < 64 && ok; i++) {
            if (a[i] != (u8)(0x10u + i)) {
                ok = 0;
            }
        }
    }
    if (ok) {
        serial_puts("ng: heap kmalloc ok\r\n");
    } else {
        fails++;
        serial_puts("ng: heap kmalloc FAIL\r\n");
    }

    /* 3) realloc 扩大/缩小都保留数据。 */
    if (a != NULL) {
        u32 i;
        a[0] = 0x5A;
        a = (u8 *)krealloc(a, 256);
        ok = (a != NULL && a[0] == 0x5A);
        if (ok) {
            for (i = 0; i < 256; i++) {
                a[i] = (u8)i;
            }
            a = (u8 *)krealloc(a, 32);
            ok = (a != NULL && a[7] == (u8)7 && a[31] == (u8)31);
        }
        if (!ok) {
            fails++;
            serial_puts("ng: heap realloc FAIL\r\n");
        }
    }

    /* 4) 对齐分配：payload 地址必须按 align 对齐，且可写可读。 */
    al = kmalloc_aligned(100, 64);
    ok = al != NULL && (((u64)(usize)al) & 63ULL) == 0;
    if (ok) {
        u32 i;
        volatile u8 *w = (volatile u8 *)al;
        for (i = 0; i < 100; i++) {
            w[i] = (u8)(i ^ 0x3Cu);
        }
        for (i = 0; i < 100; i++) {
            if (w[i] != (u8)(i ^ 0x3Cu)) {
                ok = 0;
            }
        }
        (void)kfree(al);
    }
    if (!ok) {
        fails++;
        serial_puts("ng: heap aligned FAIL\r\n");
    }

    /* 5) 全部释放后必须合并成“每 region 一个空闲块”，不能有泄漏残留。 */
    if (a != NULL) {
        (void)kfree(a);
    }
    if (b != NULL) {
        (void)kfree(b);
    }
    if (c != NULL) {
        (void)kfree(c);
    }

    ok = heap_live_allocs() == 0 && heap_live_bytes() == 0 &&
         heap_block_count() == heap_region_count() && heap_check() == 0;
    if (ok) {
        serial_puts("ng: heap coalesce ok\r\n");
    } else {
        fails++;
        serial_puts("ng: heap coalesce FAIL\r\n");
    }

    if (heap_region_count() != 1 ||
        heap_free_bytes() + HEAP_HDR_SIZE !=
            (u64)HEAP_GROW_MIN_PAGES * NG_PAGE_SIZE) {
        fails++;
        serial_puts("ng: heap region accounting FAIL\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* 页表池来自 PMM                                                             */
/* ------------------------------------------------------------------------- */

int mem_pgtbl_selftest(void)
{
    u32 before = pg_table_count();
    void *table = pg_alloc_table();
    int ok;

    ok = table != NULL && NG_IS_PAGE_ALIGNED((u64)(usize)table) &&
         pg_table_count() == before + 1;
    if (ok) {
        volatile u64 *t = (volatile u64 *)(usize)table;
        u32 i;
        for (i = 0; i < 512; i++) {
            if (t[i] != 0) {
                ok = 0;
            }
        }
    }
    if (ok) {
        serial_puts("ng: pgtbl from pmm ok (tables=");
        serial_putdec((u64)pg_table_count());
        serial_puts(")\r\n");
        return 0;
    }
    serial_puts("ng: pgtbl from pmm FAIL\r\n");
    return 1;
}
