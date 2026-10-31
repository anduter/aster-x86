/*
 * ng WP1-4: 内存图解析（PVH start_info / multiboot2 mmap）。
 *
 * 只做“结构 -> ng_memmap”的纯逻辑，不打印、不分配、不碰硬件；host 单测
 * 用合成结构直接覆盖这两条路径（见 tests/unit-ng/test_memmap.c）。
 */
#include "memmap.h"

#include "phys.h"

#define PVH_MAGIC 0x336EC578u
#define MB2_TAG_END 0u
#define MB2_TAG_MMAP 6u
#define MB2_MAX_SIZE (1u << 20) /* 1 MiB 上限：防止坏 total_size 把循环拖死 */
#define PVH_MAX_ENTRIES 4096u

/* ---- PVH（Xen hvm_start_info）布局 ---- */
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
} pvh_start_info_t;

typedef struct __attribute__((packed)) {
    u64 addr;
    u64 size;
    u32 type;
    u32 reserved;
} pvh_memmap_entry_t;

/* ---- multiboot2 布局 ---- */
typedef struct __attribute__((packed)) {
    u32 total_size;
    u32 reserved;
} mb2_info_t;

typedef struct __attribute__((packed)) {
    u32 type;
    u32 size;
} mb2_tag_t;

typedef struct __attribute__((packed)) {
    u32 type;
    u32 size;
    u32 entry_size;
    u32 entry_version;
} mb2_mmap_tag_t;

typedef struct __attribute__((packed)) {
    u64 addr;
    u64 len;
    u32 type;
    u32 zero;
} mb2_mmap_entry_t;

/* 地址加法饱和：坏固件给的 addr+size 溢出时不要回绕成小地址。 */
static u64 sat_add(u64 a, u64 b)
{
    u64 max = ~(u64)0;
    return (a > max - b) ? max : a + b;
}

static u32 pvh_type(u32 type)
{
    switch (type) {
    case 1:
        return MEMMAP_RAM;
    case 2:
        return MEMMAP_RESERVED;
    case 3:
        return MEMMAP_ACPI;
    case 4:
        return MEMMAP_NVS;
    case 5:
        return MEMMAP_UNUSABLE;
    default:
        return MEMMAP_RESERVED;
    }
}

static u32 mb2_type(u32 type)
{
    switch (type) {
    case 1:
        return MEMMAP_RAM;
    case 3:
        return MEMMAP_ACPI;
    case 4:
        return MEMMAP_NVS;
    case 5:
        return MEMMAP_UNUSABLE;
    default:
        return MEMMAP_RESERVED;
    }
}

static void map_reset(ng_memmap *m)
{
    u32 i;

    m->source = MEMMAP_SRC_NONE;
    m->count = 0;
    m->usable_pages = 0;
    m->usable_bytes = 0;
    m->boot_start = 0;
    m->boot_end = 0;
    for (i = 0; i < MEMMAP_MAX_ENTRIES; i++) {
        m->entries[i].addr = 0;
        m->entries[i].size = 0;
        m->entries[i].type = 0;
        m->entries[i].reserved = 0;
    }
}

static int map_add(ng_memmap *m, u64 addr, u64 size, u32 type)
{
    if (size == 0) {
        return MEMMAP_OK;
    }
    if (m->count >= MEMMAP_MAX_ENTRIES) {
        return MEMMAP_EFULL;
    }
    m->entries[m->count].addr = addr;
    m->entries[m->count].size = size;
    m->entries[m->count].type = type;
    m->entries[m->count].reserved = 0;
    m->count++;
    return MEMMAP_OK;
}

/*
 * 统计可用页：只算 type=RAM 与 [0, NG_PHYS_LIMIT) 的交集，起点向上取整、
 * 终点向下取整——只有完整落在可用区里的页才会计入。
 */
static void map_finish(ng_memmap *m)
{
    u64 pages = 0;
    u64 bytes = 0;
    u32 i;

    for (i = 0; i < m->count; i++) {
        const ng_memrange *r = &m->entries[i];
        u64 start;
        u64 end;

        if (r->type != MEMMAP_RAM) {
            continue;
        }
        if (r->addr >= NG_PHYS_LIMIT) {
            continue;
        }
        start = NG_PAGE_ALIGN_UP(r->addr);
        end = sat_add(r->addr, r->size);
        if (end > NG_PHYS_LIMIT) {
            end = NG_PHYS_LIMIT;
        }
        end = NG_PAGE_ALIGN_DOWN(end);
        if (end <= start) {
            continue;
        }
        pages += (end - start) >> NG_PAGE_SHIFT;
        bytes += end - start;
    }
    m->usable_pages = pages;
    m->usable_bytes = bytes;
}

static int parse_pvh(u64 info, ng_memmap *m)
{
    const pvh_start_info_t *si;
    const pvh_memmap_entry_t *entries;
    u64 map_end;
    u32 i;
    u32 entries_n;

    if (info == 0 || (info & 3u) != 0) {
        return MEMMAP_ENOENT;
    }
    si = (const pvh_start_info_t *)(usize)info;
    if (si->magic != PVH_MAGIC) {
        return MEMMAP_ENOENT;
    }
    if (si->version < 1 || si->memmap_paddr == 0 || si->memmap_entries == 0) {
        return MEMMAP_EBAD;
    }
    entries_n = si->memmap_entries;
    if (entries_n > PVH_MAX_ENTRIES) {
        return MEMMAP_EBAD;
    }

    entries = (const pvh_memmap_entry_t *)(usize)si->memmap_paddr;
    for (i = 0; i < entries_n; i++) {
        int rc = map_add(m, entries[i].addr, entries[i].size,
                         pvh_type(entries[i].type));

        if (rc != MEMMAP_OK) {
            return rc;
        }
    }

    map_end = sat_add(si->memmap_paddr,
                      (u64)entries_n * sizeof(pvh_memmap_entry_t));
    m->source = MEMMAP_SRC_PVH;
    m->boot_start = info;
    m->boot_end = map_end;
    return MEMMAP_OK;
}

static int parse_mb2(u64 info, ng_memmap *m)
{
    const mb2_info_t *mi;
    const u8 *base;
    u32 total;
    u32 off;
    int seen_mmap = 0;

    if (info == 0 || (info & 7u) != 0) {
        return MEMMAP_ENOENT;
    }
    mi = (const mb2_info_t *)(usize)info;
    if (mi->total_size < 16u || mi->total_size > MB2_MAX_SIZE) {
        return MEMMAP_ENOENT;
    }
    base = (const u8 *)(usize)info;
    total = mi->total_size;

    off = 8;
    while (off + 8u <= total) {
        const mb2_tag_t *tag = (const mb2_tag_t *)(usize)(base + off);
        u32 tag_size = tag->size;

        if (tag_size < 8u || off + tag_size > total) {
            return MEMMAP_EBAD;
        }
        if (tag->type == MB2_TAG_END) {
            break;
        }
        if (tag->type == MB2_TAG_MMAP) {
            const mb2_mmap_tag_t *mt = (const mb2_mmap_tag_t *)(usize)tag;
            u64 count;
            u64 j;

            if (tag_size < sizeof(mb2_mmap_tag_t) || mt->entry_size < sizeof(mb2_mmap_entry_t)) {
                return MEMMAP_EBAD;
            }
            count = (tag_size - sizeof(mb2_mmap_tag_t)) / mt->entry_size;
            for (j = 0; j < count; j++) {
                const mb2_mmap_entry_t *e = (const mb2_mmap_entry_t *)(usize)(
                    (const u8 *)mt + sizeof(mb2_mmap_tag_t) + j * mt->entry_size);
                int rc = map_add(m, e->addr, e->len, mb2_type(e->type));

                if (rc != MEMMAP_OK) {
                    return rc;
                }
            }
            seen_mmap = 1;
        }
        off += (tag_size + 7u) & ~7u;
    }

    if (!seen_mmap) {
        return MEMMAP_EBAD;
    }
    m->source = MEMMAP_SRC_MB2;
    m->boot_start = info;
    m->boot_end = sat_add(info, total);
    return MEMMAP_OK;
}

const char *memmap_source_name(u32 source)
{
    switch (source) {
    case MEMMAP_SRC_PVH:
        return "pvh";
    case MEMMAP_SRC_MB2:
        return "mb2";
    default:
        return "none";
    }
}

int memmap_parse(u64 boot_info, ng_memmap *out)
{
    int rc;

    if (out == NULL) {
        return MEMMAP_EBAD;
    }
    map_reset(out);

    rc = parse_pvh(boot_info, out);
    if (rc == MEMMAP_ENOENT) {
        map_reset(out);
        rc = parse_mb2(boot_info, out);
    }
    if (rc != MEMMAP_OK) {
        map_reset(out);
        return rc;
    }
    map_finish(out);
    if (out->usable_pages == 0) {
        map_reset(out);
        return MEMMAP_EBAD;
    }
    return MEMMAP_OK;
}
