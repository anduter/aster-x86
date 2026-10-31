/*
 * ng WP1-4: 启动内存图解析（PVH start_info / multiboot2 mmap）。
 *
 * 两个来源都只是“读固定布局的结构”，不碰端口/控制寄存器，所以同一份代码
 * 可以被 host 单测用假的 PVH/multiboot2 结构直接调用。
 *
 * 进入 kmain 时 boot_info 的含义取决于引导路径（见 entry.S）：
 *   - QEMU -kernel（PVH）：boot_info = hvm_start_info 物理地址，magic
 *     0x336EC578；memmap_paddr 指向 hvm_memmap_table_entry 数组（24 字节/
 *     项：u64 addr、u64 size、u32 type、u32 reserved）。type 1 = RAM。
 *   - multiboot2：boot_info = multiboot2 information 物理地址；tag 6 = mmap
 *     （u64 addr、u64 len、u32 type、u32 zero），type 1 = available。
 *     注意：multiboot2 的 magic 在 EAX 里而不是 info 结构里；entry.S 目前只
 *     转交 EBX，所以这条路径用结构校验（total_size + tag 链）识别，未在
 *     QEMU 实机验证（QEMU -kernel 走 PVH）。
 *
 * 恒等映射保证 boot_info 可以直接按地址解引用（QEMU PVH 把它和 memmap 放在
 * 0x1000–0x2000 一带，1 MiB 以下）。
 */
#ifndef NG_MEMMAP_H
#define NG_MEMMAP_H

#include "types.h"

#define MEMMAP_MAX_ENTRIES 64

/* 统一后的内存类型（取 PVH 的编号，multiboot2 做映射） */
#define MEMMAP_RAM 1
#define MEMMAP_RESERVED 2
#define MEMMAP_ACPI 3
#define MEMMAP_NVS 4
#define MEMMAP_UNUSABLE 5

/* 来源 */
#define MEMMAP_SRC_NONE 0
#define MEMMAP_SRC_PVH 1
#define MEMMAP_SRC_MB2 2

/* 返回码 */
#define MEMMAP_OK 0
#define MEMMAP_ENOENT (-1) /* 两个 magic 都不匹配 */
#define MEMMAP_EBAD (-2)   /* 结构损坏 / 没有可用 RAM */
#define MEMMAP_EFULL (-3)  /* 条目数超过 MEMMAP_MAX_ENTRIES */

typedef struct {
    u64 addr;
    u64 size;
    u32 type;
    u32 reserved;
} ng_memrange;

typedef struct {
    u32 source; /* MEMMAP_SRC_* */
    u32 count;
    ng_memrange entries[MEMMAP_MAX_ENTRIES];
    /*
     * 可用 RAM 的页数/字节数：只统计 type=RAM 且落在 [0, NG_PHYS_LIMIT)
     * 的整页（首尾按页对齐裁剪）。这是“内存图报告值”，不是 PMM 扣除保留
     * 区之后的空闲页数。
     */
    u64 usable_pages;
    u64 usable_bytes;
    /* 引导数据结构自身占用的范围（要保留，不能被 PMM 分配出去） */
    u64 boot_start;
    u64 boot_end;
} ng_memmap;

/*
 * 解析 boot_info。成功返回 MEMMAP_OK；否则 MEMMAP_E*。
 * 先试 PVH，magic 不匹配再试 multiboot2；两者都失败返回 MEMMAP_ENOENT。
 */
int memmap_parse(u64 boot_info, ng_memmap *out);

const char *memmap_source_name(u32 source);

#endif /* NG_MEMMAP_H */
