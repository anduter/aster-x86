/*
 * ng WP1-7: ELF64（ET_EXEC / EM_X86_64）静态加载器。
 *
 * 分两半，边界是 `struct elf_env`：
 *   1. `elf64_check()` 是**纯校验 + 段规划**：只读镜像字节，不碰页表、不碰 PMM、
 *      不 include 任何内核模块。host 单测（tests/unit-ng/test_elf.c）直接调它，
 *      用合成 ELF 覆盖每一条拒绝路径。
 *   2. `elf64_load()` 按规划逐段映射：每页走 env 的 alloc/page_ptr/map 回调，
 *      文件字节拷进去、`.bss` 补零，失败时回滚已经映射的页。内核对 env 的实现
 *      在 elf_selftest.c（PMM + 用户地址空间），host 单测用 fake 页池。
 *
 * 本 WP 的加载窗口固定为用户低半区：[0x40000000, 0x0000007FFFFFFFFF]
 * （PML4[0] 覆盖的 <512 GiB 范围）——这样用户地址空间的页表子树永远不在
 * PML4[1..255] 上，AS 销毁时可以只走自己的子树，不会碰内核共享表。
 *
 * 安全策略（拒绝而不是"尽力而为"）：
 *   - ELF64、小端、ET_EXEC、EM_X86_64、e_version=1、e_ehsize=64、phentsize=56；
 *   - e_phoff 8 字节对齐、phdr 表整段在文件内、1 ≤ e_phnum ≤ 16；
 *   - 每个 PT_LOAD：filesz ≤ memsz、文件区间在镜像内、vaddr 4 KiB 对齐、
 *     p_align 是 ≥4096 的 2 的幂、[vaddr, vaddr+memsz) 在用户窗口内且不溢出；
 *   - W^X：PF_W 与 PF_X 同时置位的段直接拒绝（PF_W→RW、PF_X→RX、其余 R+NX）；
 *   - 段之间不允许重叠（按 [vaddr, vaddr+memsz) 两两比较，O(n²)，n ≤ 16）；
 *   - e_entry 必须落在某个 PF_X 且非空的 PT_LOAD 里；
 *   - 至少一个 PT_LOAD。
 */
#ifndef NG_ELF_H
#define NG_ELF_H

#include "types.h"

/* ---- ELF64 常量（够用即可，不引 elf.h）---- */
#define ELF_EI_NIDENT 16u
#define ELF_EI_CLASS 4u
#define ELF_EI_DATA 5u
#define ELF_EI_VERSION 6u
#define ELF_CLASS64 2u
#define ELF_DATA2LSB 1u
#define ELF_VERSION_CURRENT 1u

#define ELF_ET_EXEC 2u
#define ELF_EM_X86_64 62u

#define ELF_PT_LOAD 1u
#define ELF_PT_NOTE 4u

#define ELF_PF_X 0x1u
#define ELF_PF_W 0x2u
#define ELF_PF_R 0x4u
#define ELF_PF_MASK 0x7u

#define ELF_EHDR_SIZE 64u
#define ELF_PHDR_SIZE 56u
#define ELF_PAGE_SIZE 4096u

/* 本加载器的硬上限：段数与每个 PT_LOAD 的页数（回滚路径按页记录）。 */
#define ELF_MAX_PHDRS 16u
#define ELF_MAX_SEGS 16u
#define ELF_MAX_SEG_PAGES 256u /* 1 MiB / 段 */

/* 用户加载窗口（与 useras.h 的 NG_USER_VA_* 同值，elf.c 不依赖 useras.h）。 */
#define ELF_USER_VA_MIN 0x0000000040000000ULL
#define ELF_USER_VA_MAX 0x0000007FFFFFFFFFULL

/* ---- 错误码（负值）---- */
#define ELF_OK 0
#define ELF_ENULL (-1)    /* image == NULL */
#define ELF_ESIZE (-2)    /* 镜像小于 ELF64 头 */
#define ELF_EHDR (-3)     /* e_ident/e_type/e_machine/e_version/e_ehsize 非法 */
#define ELF_EPHDR (-4)    /* e_phoff/e_phentsize/e_phnum 非法或 phdr 越界 */
#define ELF_ENOSEG (-5)   /* 没有 PT_LOAD */
#define ELF_ESEG (-6)     /* PT_LOAD 字段非法（filesz>memsz/W^X/对齐/页数） */
#define ELF_EOFF (-7)     /* p_offset + p_filesz 超出镜像 */
#define ELF_EVA (-8)      /* 段地址不在用户窗口/溢出 */
#define ELF_EOVERLAP (-9) /* 两个 PT_LOAD 的 [vaddr,vaddr+memsz) 重叠 */
#define ELF_EENTRY (-10)  /* e_entry 不在可执行 PT_LOAD 内 */
#define ELF_ENOMEM (-11)  /* env->alloc_page 返回 0 */
#define ELF_EMAP (-12)    /* env->map_page 失败 */
#define ELF_EINPUT (-13)  /* env 回调缺失 / page_ptr 返回 NULL */

/* ---- ELF64 结构（布局由架构固定，加断言防手滑）---- */
struct elf64_ehdr {
    u8 ident[ELF_EI_NIDENT];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};

struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

_Static_assert(sizeof(struct elf64_ehdr) == ELF_EHDR_SIZE, "Elf64_Ehdr is 64 bytes");
_Static_assert(sizeof(struct elf64_phdr) == ELF_PHDR_SIZE, "Elf64_Phdr is 56 bytes");
_Static_assert(__builtin_offsetof(struct elf64_ehdr, phoff) == 32, "e_phoff offset");
_Static_assert(__builtin_offsetof(struct elf64_ehdr, phnum) == 56, "e_phnum offset");
_Static_assert(__builtin_offsetof(struct elf64_phdr, vaddr) == 16, "p_vaddr offset");
_Static_assert(__builtin_offsetof(struct elf64_phdr, memsz) == 40, "p_memsz offset");
_Static_assert(__builtin_offsetof(struct elf64_phdr, align) == 48, "p_align offset");

/* 校验产出的段规划。pages = ceil(memsz/4096)，由 elf64_load 使用。 */
struct elf64_seg {
    u32 flags; /* ELF_PF_* */
    u32 pages;
    u64 offset;
    u64 vaddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

struct elf64_image {
    u64 entry;
    u64 base; /* min(p_vaddr) */
    u64 end;  /* max(p_vaddr + p_memsz)，未页对齐 */
    u16 phnum;
    u32 seg_count;
    u32 load_flags; /* 所有段 flags 的并集 */
    struct elf64_seg seg[ELF_MAX_SEGS];
};

/*
 * 纯校验：成功返回 ELF_OK 并把规划填进 *out（out 可为 NULL，只做检查）。
 * user_min/user_max 是允许的虚拟地址闭区间（本 WP 用 ELF_USER_VA_*）。
 */
int elf64_check(const void *image, u64 size, u64 user_min, u64 user_max,
                struct elf64_image *out);

/* ---- 加载环境（内核对它实现 PMM + 用户页表；host 单测用 fake 页池）---- */
#define ELF_MAP_USER 0x1u  /* 用户可访问（U/S=1） */
#define ELF_MAP_WRITE 0x2u /* 可写（否则只读） */
#define ELF_MAP_NX 0x4u    /* 不可执行（PF_X=0 时置位） */

struct elf_env {
    void *ctx;
    /* 分配一页 4 KiB 物理页，返回物理地址；0 = 失败。 */
    u64 (*alloc_page)(void *ctx);
    /* 释放一页尚未映射（或刚回滚）的物理页。 */
    void (*free_page)(void *ctx, u64 phys);
    /* 物理页的可写指针（内核恒等映射下就是 phys；host 用 fake 缓冲区）。 */
    u8 *(*page_ptr)(void *ctx, u64 phys);
    /* 把 va 映射到 phys；返回 0 成功，非 0 失败。 */
    int (*map_page)(void *ctx, u64 va, u64 phys, u64 flags);
    /* 回滚：解除 va 的映射并释放其物理页。 */
    void (*unmap_page)(void *ctx, u64 va);
};

struct elf64_load_info {
    u64 entry;
    u64 base;
    u64 end;
    u32 segs;
    u32 pages;
    u64 bss_bytes; /* Σ(memsz - filesz) */
};

/*
 * 校验 + 加载。成功返回 ELF_OK；失败时已映射的页全部回滚（unmap_page），
 * 刚分配但未映射的页用 free_page 归还。env 的 5 个回调都必须非 NULL。
 */
int elf64_load(const void *image, u64 size, const struct elf_env *env, u64 user_min,
               u64 user_max, struct elf64_image *out_plan,
               struct elf64_load_info *out_info);

#endif /* NG_ELF_H */
