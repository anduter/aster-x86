/*
 * ng WP1-4 host 单测：内存图解析（合成的 PVH start_info / multiboot2 结构）。
 *
 * 这两条路径在真实引导时读物理地址；host 上把“物理地址”换成普通指针即可，
 * 解析代码本身不区分（QEMU PVH 的 start_info 落在 1 MiB 以下，恒等映射可直接
 * 解引用）。
 */
#include "memmap.h"
#include "phys.h"
#include "test_util.h"

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 version;
    u32 flags;
    u32 nr_modules;
    u64 modlist_paddr;
    u64 cmdline_paddr;
    u64 rsdp_paddr;
    u64 memmap_paddr;
    u32 memmap_entries;
    u32 reserved;
} fake_pvh_t;

typedef struct __attribute__((packed)) {
    u64 addr;
    u64 size;
    u32 type;
    u32 reserved;
} fake_mm_t;

static fake_pvh_t pvh __attribute__((aligned(8)));
static fake_mm_t mm[5] __attribute__((aligned(8)));
static u8 mb2[1024] __attribute__((aligned(8)));

static void pvh_reset(void)
{
    u32 i;

    pvh.magic = 0x336EC578u;
    pvh.version = 1;
    pvh.flags = 0;
    pvh.nr_modules = 0;
    pvh.modlist_paddr = 0;
    pvh.cmdline_paddr = 0;
    pvh.rsdp_paddr = 0;
    pvh.memmap_paddr = (u64)(usize)mm;
    pvh.memmap_entries = 5;
    pvh.reserved = 0;

    for (i = 0; i < 5; i++) {
        mm[i].addr = 0;
        mm[i].size = 0;
        mm[i].type = 2;
        mm[i].reserved = 0;
    }
    mm[0].addr = 0x0;
    mm[0].size = 0x9FC00; /* 639 KiB */
    mm[0].type = 1;
    mm[1].addr = 0x9FC00;
    mm[1].size = 0x400;
    mm[1].type = 2;
    mm[2].addr = 0x100000;
    mm[2].size = 0xF00000; /* 15 MiB */
    mm[2].type = 1;
    mm[3].addr = 0x40000000; /* 恰好在 1 GiB 上界，忽略 */
    mm[3].size = 0x100000;
    mm[3].type = 1;
    mm[4].addr = 0x100000000ULL; /* 4 GiB 以上，忽略 */
    mm[4].size = 0x1000;
    mm[4].type = 1;
}

static void mb2_build(void)
{
    u8 *b = mb2;
    u64 *q;
    u32 *t;
    u32 total;

    for (u32 i = 0; i < (u32)sizeof(mb2); i++) {
        mb2[i] = 0;
    }
    /* info header: total_size, reserved */
    t = (u32 *)(usize)mb2;
    t[0] = 0; /* 下面填 */
    t[1] = 0;

    /* tag 6: mmap，16 字节头 + 2 个 24 字节 entry */
    t = (u32 *)(usize)(b + 8);
    t[0] = 6;       /* type */
    t[1] = 16 + 2 * 24; /* size */
    t[2] = 24;      /* entry_size */
    t[3] = 0;       /* entry_version */

    q = (u64 *)(usize)(b + 24);
    q[0] = 0x100000;
    q[1] = 0x100000;
    t = (u32 *)(usize)(b + 24 + 16);
    t[0] = 1;
    t[1] = 0;

    q = (u64 *)(usize)(b + 48);
    q[0] = 0x200000;
    q[1] = 0x100000;
    t = (u32 *)(usize)(b + 48 + 16);
    t[0] = 1;
    t[1] = 0;

    /* end tag */
    t = (u32 *)(usize)(b + 72);
    t[0] = 0;
    t[1] = 8;

    total = 80;
    t = (u32 *)(usize)mb2;
    t[0] = total;
}

int test_memmap(void)
{
    ng_memmap map;
    int rc;

    /* ---- PVH：混合 RAM/保留区 + 上界外条目被忽略 ---- */
    pvh_reset();
    rc = memmap_parse((u64)(usize)&pvh, &map);
    CHECK(rc == MEMMAP_OK);
    CHECK(map.source == MEMMAP_SRC_PVH);
    CHECK(map.count == 5);
    CHECK(map.entries[0].type == MEMMAP_RAM);
    CHECK(map.entries[1].type == MEMMAP_RESERVED);
    CHECK(map.entries[2].type == MEMMAP_RAM);
    CHECK(map.usable_pages == 159 + 3840);
    CHECK(map.usable_bytes == (u64)(159 + 3840) * NG_PAGE_SIZE);
    CHECK(map.boot_start == (u64)(usize)&pvh);
    CHECK(map.boot_end == (u64)(usize)mm + 5 * 24);
    CHECK(map.boot_end > map.boot_start);

    /* ---- 坏的 PVH：版本/条目数/表地址 ---- */
    pvh_reset();
    pvh.version = 0;
    CHECK(memmap_parse((u64)(usize)&pvh, &map) == MEMMAP_EBAD);

    pvh_reset();
    pvh.memmap_entries = 0;
    CHECK(memmap_parse((u64)(usize)&pvh, &map) == MEMMAP_EBAD);

    pvh_reset();
    pvh.memmap_paddr = 0;
    CHECK(memmap_parse((u64)(usize)&pvh, &map) == MEMMAP_EBAD);

    /* ---- 两个 magic 都不匹配 ---- */
    pvh_reset();
    pvh.magic = 0;
    CHECK(memmap_parse((u64)(usize)&pvh, &map) == MEMMAP_ENOENT);

    /* ---- multiboot2：结构校验路径 ---- */
    mb2_build();
    rc = memmap_parse((u64)(usize)mb2, &map);
    CHECK(rc == MEMMAP_OK);
    CHECK(map.source == MEMMAP_SRC_MB2);
    CHECK(map.count == 2);
    CHECK(map.entries[0].type == MEMMAP_RAM);
    CHECK(map.entries[0].addr == 0x100000);
    CHECK(map.entries[1].addr == 0x200000);
    CHECK(map.usable_pages == 512);
    CHECK(map.usable_bytes == 0x200000);
    CHECK(map.boot_start == (u64)(usize)mb2);
    CHECK(map.boot_end == (u64)(usize)mb2 + 80);

    /* ---- 坏的 multiboot2：total_size=0 / 截断的 tag ---- */
    mb2_build();
    ((u32 *)(usize)mb2)[0] = 0;
    CHECK(memmap_parse((u64)(usize)mb2, &map) == MEMMAP_ENOENT);

    mb2_build();
    {
        u32 *t = (u32 *)(usize)(mb2 + 8);

        t[1] = 1u << 20; /* tag size 越界 */
    }
    CHECK(memmap_parse((u64)(usize)mb2, &map) == MEMMAP_EBAD);

    CHECK(memmap_source_name(MEMMAP_SRC_PVH)[0] == 'p');
    CHECK(memmap_source_name(MEMMAP_SRC_MB2)[0] == 'm');
    CHECK(memmap_source_name(MEMMAP_SRC_NONE)[0] == 'n');
    return 0;
}
