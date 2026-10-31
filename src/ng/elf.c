/*
 * ng WP1-7: ELF64 校验 + 加载（纯逻辑 + env 回调）。
 *
 * 本文件不 include paging/heap/serial，host 与内核共用同一份实现：
 *   - 内核：elf_selftest.c 把 env 绑到 PMM + 用户地址空间（useras.c）；
 *   - host：tests/unit-ng/test_elf.c 把 env 绑到静态 fake 页池。
 *
 * 校验顺序是"先头、再 phdr 表、再逐段、最后段间关系/入口"，每个失败都有独立
 * 错误码；加载阶段只做已经校验过的事，所以失败只剩 ENOMEM/EMAP 两种。
 */
#include "elf.h"

/* ------------------------------------------------------------------------- */
/* 小工具（freestanding，不用 libc）                                          */
/* ------------------------------------------------------------------------- */

static void elf_memzero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static void elf_memcpy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/* 2 的幂（0/1 不算）。 */
static int elf_is_pow2(u64 v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static u64 elf_min_u64(u64 a, u64 b)
{
    return (a < b) ? a : b;
}

/* ------------------------------------------------------------------------- */
/* 纯校验                                                                     */
/* ------------------------------------------------------------------------- */

static int elf_check_ident(const struct elf64_ehdr *eh)
{
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F') {
        return ELF_EHDR;
    }
    if (eh->ident[ELF_EI_CLASS] != ELF_CLASS64) {
        return ELF_EHDR;
    }
    if (eh->ident[ELF_EI_DATA] != ELF_DATA2LSB) {
        return ELF_EHDR;
    }
    if (eh->ident[ELF_EI_VERSION] != ELF_VERSION_CURRENT) {
        return ELF_EHDR;
    }
    if (eh->version != ELF_VERSION_CURRENT) {
        return ELF_EHDR;
    }
    if (eh->type != ELF_ET_EXEC) {
        return ELF_EHDR;
    }
    if (eh->machine != ELF_EM_X86_64) {
        return ELF_EHDR;
    }
    if (eh->ehsize != ELF_EHDR_SIZE) {
        return ELF_EHDR;
    }
    return ELF_OK;
}

/*
 * 校验一个 PT_LOAD 并把规划写进 seg。返回 ELF_OK 或错误码。
 * 这里只做"单段自身"的检查；段间重叠与入口在 elf64_check 里做。
 */
static int elf_check_segment(const struct elf64_phdr *ph, u64 image_size, u64 user_min,
                             u64 user_max, struct elf64_seg *seg)
{
    u64 pages;

    if ((ph->flags & ~(u64)ELF_PF_MASK) != 0) {
        return ELF_ESEG;
    }
    /* W^X：可写段不允许同时可执行（本加载器不建 RWX 映射）。 */
    if ((ph->flags & ELF_PF_W) != 0 && (ph->flags & ELF_PF_X) != 0) {
        return ELF_ESEG;
    }
    if (ph->filesz > ph->memsz) {
        return ELF_ESEG;
    }
    /* 文件区间必须在镜像内（无 64 位溢出）。 */
    if (ph->offset > image_size || ph->filesz > image_size - ph->offset) {
        return ELF_EOFF;
    }
    /* 有效段必须在用户窗口内（无 64 位溢出）。 */
    if (ph->memsz > user_max - user_min) {
        return ELF_EVA;
    }
    if (ph->vaddr < user_min || ph->vaddr > user_max) {
        return ELF_EVA;
    }
    if (ph->memsz > user_max - ph->vaddr) {
        return ELF_EVA;
    }
    /* 4 KiB 粒度映射要求段起始页对齐、p_align 是 ≥4 KiB 的 2 的幂。 */
    if ((ph->vaddr & (ELF_PAGE_SIZE - 1)) != 0) {
        return ELF_ESEG;
    }
    if (ph->align < ELF_PAGE_SIZE || !elf_is_pow2(ph->align)) {
        return ELF_ESEG;
    }
    pages = (ph->memsz + ELF_PAGE_SIZE - 1) / ELF_PAGE_SIZE;
    if (pages > ELF_MAX_SEG_PAGES) {
        return ELF_ESEG;
    }
    seg->flags = ph->flags;
    seg->pages = (u32)pages;
    seg->offset = ph->offset;
    seg->vaddr = ph->vaddr;
    seg->filesz = ph->filesz;
    seg->memsz = ph->memsz;
    seg->align = ph->align;
    return ELF_OK;
}

int elf64_check(const void *image, u64 size, u64 user_min, u64 user_max,
                struct elf64_image *out)
{
    const u8 *img = (const u8 *)image;
    const struct elf64_ehdr *eh;
    struct elf64_image plan;
    u64 ph_end;
    u32 i;
    u32 j;

    if (image == NULL) {
        return ELF_ENULL;
    }
    if (size < ELF_EHDR_SIZE) {
        return ELF_ESIZE;
    }
    if (user_min > user_max || user_min == 0) {
        return ELF_EVA;
    }

    eh = (const struct elf64_ehdr *)img;
    {
        int rc = elf_check_ident(eh);

        if (rc != ELF_OK) {
            return rc;
        }
    }

    if (eh->phoff == 0 || (eh->phoff & 7u) != 0) {
        return ELF_EPHDR;
    }
    if (eh->phentsize != ELF_PHDR_SIZE || eh->phnum == 0 ||
        eh->phnum > ELF_MAX_PHDRS) {
        return ELF_EPHDR;
    }
    if (eh->phoff > size || (u64)eh->phnum > (size - eh->phoff) / ELF_PHDR_SIZE) {
        return ELF_EPHDR;
    }
    ph_end = eh->phoff + (u64)eh->phnum * ELF_PHDR_SIZE;
    if (ph_end > size || ph_end < eh->phoff) {
        return ELF_EPHDR;
    }

    elf_memzero(&plan, sizeof(plan));
    plan.entry = eh->entry;
    plan.phnum = eh->phnum;

    for (i = 0; i < (u32)eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(img + eh->phoff + (u64)i * ELF_PHDR_SIZE);
        struct elf64_seg seg;
        int rc;

        if (ph->type != ELF_PT_LOAD) {
            continue; /* PT_NOTE/PT_PHDR 等忽略（本加载器不需要它们） */
        }
        if (plan.seg_count >= ELF_MAX_SEGS) {
            return ELF_ESEG;
        }
        elf_memzero(&seg, sizeof(seg));
        rc = elf_check_segment(ph, size, user_min, user_max, &seg);
        if (rc != ELF_OK) {
            return rc;
        }
        plan.load_flags |= seg.flags;
        plan.seg[plan.seg_count] = seg;
        plan.seg_count++;
    }

    if (plan.seg_count == 0) {
        return ELF_ENOSEG;
    }

    /* base/end + 段间重叠（含 0 长度段不参与）。 */
    plan.base = plan.seg[0].vaddr;
    plan.end = plan.seg[0].vaddr + plan.seg[0].memsz;
    for (i = 0; i < plan.seg_count; i++) {
        const struct elf64_seg *a = &plan.seg[i];

        if (a->memsz == 0) {
            continue;
        }
        if (a->vaddr < plan.base) {
            plan.base = a->vaddr;
        }
        if (a->vaddr + a->memsz > plan.end) {
            plan.end = a->vaddr + a->memsz;
        }
        for (j = i + 1; j < plan.seg_count; j++) {
            const struct elf64_seg *b = &plan.seg[j];

            if (b->memsz == 0) {
                continue;
            }
            if (a->vaddr < b->vaddr + b->memsz && b->vaddr < a->vaddr + a->memsz) {
                return ELF_EOVERLAP;
            }
        }
    }

    /* 入口必须落在可执行且非空的 PT_LOAD 里。 */
    {
        int entry_ok = 0;

        for (i = 0; i < plan.seg_count; i++) {
            const struct elf64_seg *s = &plan.seg[i];

            if ((s->flags & ELF_PF_X) == 0 || s->memsz == 0) {
                continue;
            }
            if (plan.entry >= s->vaddr && plan.entry < s->vaddr + s->memsz) {
                entry_ok = 1;
                break;
            }
        }
        if (!entry_ok) {
            return ELF_EENTRY;
        }
    }

    if (out != NULL) {
        *out = plan;
    }
    return ELF_OK;
}

/* ------------------------------------------------------------------------- */
/* 加载                                                                       */
/* ------------------------------------------------------------------------- */

static void elf_rollback(const struct elf_env *env, const struct elf64_image *plan,
                         u32 done_segs, u64 cur_vaddr, u32 done_pages)
{
    u32 i;
    u32 p;

    for (i = 0; i < done_segs && i < plan->seg_count; i++) {
        for (p = 0; p < plan->seg[i].pages; p++) {
            env->unmap_page(env->ctx, plan->seg[i].vaddr + (u64)p * ELF_PAGE_SIZE);
        }
    }
    for (p = 0; p < done_pages; p++) {
        env->unmap_page(env->ctx, cur_vaddr + (u64)p * ELF_PAGE_SIZE);
    }
}

int elf64_load(const void *image, u64 size, const struct elf_env *env, u64 user_min,
               u64 user_max, struct elf64_image *out_plan,
               struct elf64_load_info *out_info)
{
    struct elf64_image plan;
    struct elf64_load_info info;
    const u8 *img = (const u8 *)image;
    int rc;
    u32 si;

    if (env == NULL || env->alloc_page == NULL || env->free_page == NULL ||
        env->page_ptr == NULL || env->map_page == NULL ||
        env->unmap_page == NULL) {
        return ELF_EINPUT;
    }
    elf_memzero(&plan, sizeof(plan));
    elf_memzero(&info, sizeof(info));

    rc = elf64_check(image, size, user_min, user_max, &plan);
    if (rc != ELF_OK) {
        return rc;
    }

    for (si = 0; si < plan.seg_count; si++) {
        const struct elf64_seg *seg = &plan.seg[si];
        u64 map_flags = ELF_MAP_USER;
        u64 p;

        /* PF_W→可写；PF_X→可执行（不带 NX）；其余只读 + NX。 */
        if ((seg->flags & ELF_PF_W) != 0) {
            map_flags |= ELF_MAP_WRITE;
        }
        if ((seg->flags & ELF_PF_X) == 0) {
            map_flags |= ELF_MAP_NX;
        }

        for (p = 0; p < seg->pages; p++) {
            u64 va = seg->vaddr + p * ELF_PAGE_SIZE;
            u64 phys = env->alloc_page(env->ctx);
            u8 *dst;
            u64 file_off = p * ELF_PAGE_SIZE;

            if (phys == 0) {
                elf_rollback(env, &plan, si, seg->vaddr, (u32)p);
                return ELF_ENOMEM;
            }
            dst = env->page_ptr(env->ctx, phys);
            if (dst == NULL) {
                env->free_page(env->ctx, phys);
                elf_rollback(env, &plan, si, seg->vaddr, (u32)p);
                return ELF_EINPUT;
            }
            /* 整页清零：既覆盖 .bss，也覆盖文件尾与页尾之间的空洞。 */
            elf_memzero(dst, ELF_PAGE_SIZE);
            if (file_off < seg->filesz) {
                u64 n = elf_min_u64(ELF_PAGE_SIZE, seg->filesz - file_off);

                elf_memcpy(dst, img + seg->offset + file_off, n);
            }
            if (env->map_page(env->ctx, va, phys, map_flags) != 0) {
                env->free_page(env->ctx, phys);
                elf_rollback(env, &plan, si, seg->vaddr, (u32)p);
                return ELF_EMAP;
            }
            info.pages++;
        }
        info.bss_bytes += seg->memsz - seg->filesz;
    }

    info.entry = plan.entry;
    info.base = plan.base;
    info.end = plan.end;
    info.segs = plan.seg_count;

    if (out_plan != NULL) {
        *out_plan = plan;
    }
    if (out_info != NULL) {
        *out_info = info;
    }
    return ELF_OK;
}
