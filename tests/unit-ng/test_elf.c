/*
 * ng WP1-7 host 单测：ELF64 校验 + 加载（fake 页池，无内核/QEMU）。
 *
 * 覆盖：
 *   - 头/程序头校验：magic/class/data/version/type/machine/ehsize/phentsize/
 *     phnum/phoff 边界与对齐；
 *   - 段校验：filesz>memsz、文件区间越界、vaddr 对齐、p_align、用户窗口、
 *     W^X、段重叠、入口不在可执行段；
 *   - 加载：逐段页数/权限（U、W、NX）、文件字节、.bss 与页尾清零、
 *     bss_bytes、out 参数可空；
 *   - 失败回滚：alloc 中途失败（ENOMEM）、map 中途失败（EMAP），已映射页
 *     全部 unmap+free，不泄漏。
 *
 * elf.c 不 include 页表/PMM/串口，所以这里用 fake 页池直接驱动同一份代码。
 */
#include <stdio.h>
#include <string.h>

#include "elf.h"
#include "test_util.h"

#define IMG_SIZE 0x3010u
#define FAKE_PAGES 32u
#define FAKE_BASE 0x100000ULL
#define FAKE_PAGE 4096ULL

static unsigned char g_img[IMG_SIZE];
static unsigned char g_tmp[IMG_SIZE];

static unsigned char g_mem[FAKE_PAGES][FAKE_PAGE];
static int g_used[FAKE_PAGES];

struct fake_map_rec {
    u64 va;
    u64 phys;
    u64 flags;
};

static struct fake_map_rec g_map[64];
static int g_map_count;
static int g_alloc_fail_after; /* 0 = 下一次分配失败；-1 = 不失败 */
static int g_map_fail_after;   /* 0 = 下一次映射失败；-1 = 不失败 */
static int g_frees;
static int g_unmaps;

/* ------------------------------------------------------------------------- */
/* 镜像构造                                                                   */
/* ------------------------------------------------------------------------- */

static void wr16(unsigned char *p, unsigned int off, unsigned int v)
{
    p[off] = (unsigned char)(v & 0xFFu);
    p[off + 1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void wr32(unsigned char *p, unsigned int off, unsigned long long v)
{
    int i;

    for (i = 0; i < 4; i++) {
        p[off + (unsigned int)i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
    }
}

static void wr64(unsigned char *p, unsigned int off, unsigned long long v)
{
    int i;

    for (i = 0; i < 8; i++) {
        p[off + (unsigned int)i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
    }
}

static void set_phdr(unsigned char *p, unsigned int idx, unsigned int type,
                     unsigned int flags, unsigned long long off,
                     unsigned long long vaddr, unsigned long long filesz,
                     unsigned long long memsz)
{
    unsigned int ph = 64u + idx * 56u;

    wr32(p, ph + 0, type);
    wr32(p, ph + 4, flags);
    wr64(p, ph + 8, off);
    wr64(p, ph + 16, vaddr);
    wr64(p, ph + 24, 0); /* paddr */
    wr64(p, ph + 32, filesz);
    wr64(p, ph + 40, memsz);
    wr64(p, ph + 48, 4096);
}

static void build_valid(unsigned char *p)
{
    unsigned int i;

    memset(p, 0, IMG_SIZE);
    p[0] = 0x7F;
    p[1] = 'E';
    p[2] = 'L';
    p[3] = 'F';
    p[4] = 2; /* ELFCLASS64 */
    p[5] = 1; /* ELFDATA2LSB */
    p[6] = 1; /* EV_CURRENT */
    wr16(p, 16, 2);  /* ET_EXEC */
    wr16(p, 18, 62); /* EM_X86_64 */
    wr32(p, 20, 1);
    wr64(p, 24, 0x40000004ULL); /* entry：.text 内 */
    wr64(p, 32, 64);            /* e_phoff */
    wr16(p, 52, 64);
    wr16(p, 54, 56);
    wr16(p, 56, 3);

    /* text：RX，filesz < memsz（页尾 + 剩余页是 .bss） */
    set_phdr(p, 0, 1, 5, 0x1000, 0x40000000ULL, 0x20, 0x1000);
    /* rodata：R+NX */
    set_phdr(p, 1, 1, 4, 0x2000, 0x40001000ULL, 0x10, 0x10);
    /* data+bss：RW+NX，memsz > filesz */
    set_phdr(p, 2, 1, 6, 0x3000, 0x40002000ULL, 0x10, 0x30);

    for (i = 0; i < 0x20; i++) {
        p[0x1000 + i] = (unsigned char)(0xA0 + i);
    }
    for (i = 0; i < 0x10; i++) {
        p[0x2000 + i] = (unsigned char)(0xB0 + i);
    }
    for (i = 0; i < 0x10; i++) {
        p[0x3000 + i] = (unsigned char)(0xC0 + i);
    }
}

/* 在合法镜像上做一次变异，返回 elf64_check 的错误码。 */
static int mutated_rc(unsigned int off, unsigned int width, unsigned long long v)
{
    int rc;

    memcpy(g_tmp, g_img, IMG_SIZE);
    if (width == 1) {
        g_tmp[off] = (unsigned char)v;
    } else if (width == 2) {
        wr16(g_tmp, off, (unsigned int)v);
    } else if (width == 4) {
        wr32(g_tmp, off, v);
    } else {
        wr64(g_tmp, off, v);
    }
    rc = elf64_check(g_tmp, IMG_SIZE, ELF_USER_VA_MIN, ELF_USER_VA_MAX, NULL);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* fake 页池                                                                  */
/* ------------------------------------------------------------------------- */

static void fake_reset(void)
{
    memset(g_used, 0, sizeof(g_used));
    g_map_count = 0;
    g_alloc_fail_after = -1;
    g_map_fail_after = -1;
    g_frees = 0;
    g_unmaps = 0;
}

static int page_index(u64 phys)
{
    if (phys < FAKE_BASE || (phys - FAKE_BASE) % FAKE_PAGE != 0) {
        return -1;
    }
    if ((phys - FAKE_BASE) / FAKE_PAGE >= FAKE_PAGES) {
        return -1;
    }
    return (int)((phys - FAKE_BASE) / FAKE_PAGE);
}

static u64 f_alloc(void *ctx)
{
    unsigned int i;

    (void)ctx;
    if (g_alloc_fail_after == 0) {
        return 0;
    }
    if (g_alloc_fail_after > 0) {
        g_alloc_fail_after--;
    }
    for (i = 0; i < FAKE_PAGES; i++) {
        if (!g_used[i]) {
            g_used[i] = 1;
            memset(g_mem[i], 0, FAKE_PAGE);
            return FAKE_BASE + (u64)i * FAKE_PAGE;
        }
    }
    return 0;
}

static void f_free(void *ctx, u64 phys)
{
    int idx = page_index(phys);

    (void)ctx;
    if (idx >= 0) {
        g_used[idx] = 0;
        g_frees++;
    }
}

static unsigned char *f_ptr(void *ctx, u64 phys)
{
    int idx = page_index(phys);

    (void)ctx;
    return (idx >= 0) ? g_mem[idx] : NULL;
}

static int f_map(void *ctx, u64 va, u64 phys, u64 flags)
{
    (void)ctx;
    if (g_map_fail_after == 0) {
        return -1;
    }
    if (g_map_fail_after > 0) {
        g_map_fail_after--;
    }
    if (g_map_count >= 64) {
        return -1;
    }
    g_map[g_map_count].va = va;
    g_map[g_map_count].phys = phys;
    g_map[g_map_count].flags = flags;
    g_map_count++;
    return 0;
}

static void f_unmap(void *ctx, u64 va)
{
    int i;

    (void)ctx;
    for (i = 0; i < g_map_count; i++) {
        if (g_map[i].va == va) {
            f_free(NULL, g_map[i].phys);
            g_map[i] = g_map[g_map_count - 1];
            g_map_count--;
            g_unmaps++;
            return;
        }
    }
}

static struct elf_env make_env(void)
{
    struct elf_env env;

    env.ctx = NULL;
    env.alloc_page = f_alloc;
    env.free_page = f_free;
    env.page_ptr = f_ptr;
    env.map_page = f_map;
    env.unmap_page = f_unmap;
    return env;
}

static int count_used(void)
{
    unsigned int i;
    int n = 0;

    for (i = 0; i < FAKE_PAGES; i++) {
        if (g_used[i]) {
            n++;
        }
    }
    return n;
}

static int find_map(u64 va, int *out_idx)
{
    int i;

    for (i = 0; i < g_map_count; i++) {
        if (g_map[i].va == va) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 校验测试                                                                   */
/* ------------------------------------------------------------------------- */

static int test_check(void)
{
    struct elf64_image plan;
    int rc;

    build_valid(g_img);
    rc = elf64_check(g_img, IMG_SIZE, ELF_USER_VA_MIN, ELF_USER_VA_MAX, &plan);
    CHECK(rc == ELF_OK);
    CHECK(plan.entry == 0x40000004ULL);
    CHECK(plan.base == 0x40000000ULL);
    CHECK(plan.end == 0x40002030ULL);
    CHECK(plan.phnum == 3);
    CHECK(plan.seg_count == 3);
    CHECK(plan.load_flags == 7);
    CHECK(plan.seg[0].pages == 1);
    CHECK(plan.seg[0].filesz == 0x20ULL);
    CHECK(plan.seg[0].memsz == 0x1000ULL);
    CHECK(plan.seg[2].filesz == 0x10ULL);
    CHECK(plan.seg[2].memsz == 0x30ULL);

    /* out 可为 NULL：只校验不产出规划。 */
    CHECK(elf64_check(g_img, IMG_SIZE, ELF_USER_VA_MIN, ELF_USER_VA_MAX, NULL) ==
          ELF_OK);

    /* NULL / 过小镜像。 */
    CHECK(elf64_check(NULL, IMG_SIZE, ELF_USER_VA_MIN, ELF_USER_VA_MAX, NULL) ==
          ELF_ENULL);
    CHECK(elf64_check(g_img, 32, ELF_USER_VA_MIN, ELF_USER_VA_MAX, NULL) ==
          ELF_ESIZE);

    /* 逐条拒绝：期望的错误码必须精确（证明是"因为正确的原因"被拒）。 */
    CHECK(mutated_rc(0, 1, 0) == ELF_EHDR);                 /* magic */
    CHECK(mutated_rc(4, 1, 1) == ELF_EHDR);                 /* class */
    CHECK(mutated_rc(5, 1, 2) == ELF_EHDR);                 /* data */
    CHECK(mutated_rc(16, 2, 1) == ELF_EHDR);                /* e_type */
    CHECK(mutated_rc(18, 2, 3) == ELF_EHDR);                /* e_machine */
    CHECK(mutated_rc(20, 4, 2) == ELF_EHDR);                /* e_version */
    CHECK(mutated_rc(52, 2, 63) == ELF_EHDR);               /* e_ehsize */
    CHECK(mutated_rc(54, 2, 55) == ELF_EPHDR);              /* e_phentsize */
    CHECK(mutated_rc(56, 2, 0) == ELF_EPHDR);               /* e_phnum=0 */
    CHECK(mutated_rc(56, 2, ELF_MAX_PHDRS + 1) == ELF_EPHDR); /* phnum>max */
    CHECK(mutated_rc(32, 8, 4) == ELF_EPHDR);               /* phoff 未对齐 */
    CHECK(mutated_rc(32, 8, IMG_SIZE) == ELF_EPHDR);        /* phoff 越界 */
    CHECK(mutated_rc(64 + 2 * 56 + 32, 8, 0x40) == ELF_ESEG); /* filesz>memsz */
    CHECK(mutated_rc(64 + 8, 8, IMG_SIZE - 8) == ELF_EOFF); /* 文件区间越界 */
    CHECK(mutated_rc(64 + 4, 4, 7) == ELF_ESEG);            /* W^X */
    CHECK(mutated_rc(64 + 4, 4, 5 | 0x10) == ELF_ESEG);     /* 保留 flag 位 */
    CHECK(mutated_rc(64 + 48, 8, 0x800) == ELF_ESEG);       /* p_align<page */
    CHECK(mutated_rc(64 + 16, 8, 0x40000800ULL) == ELF_ESEG); /* vaddr 未对齐 */
    CHECK(mutated_rc(64 + 16, 8, 0x30000000ULL) == ELF_EVA);  /* < user_min */
    CHECK(mutated_rc(64 + 16, 8, 0x8000000000ULL) == ELF_EVA); /* > user_max */
    CHECK(mutated_rc(24, 8, 0x40002000ULL) == ELF_EENTRY);  /* 入口不在 X 段 */
    CHECK(mutated_rc(64 + 56 + 16, 8, 0x40000000ULL) == ELF_EOVERLAP);
    CHECK(mutated_rc(24, 8, 0x50000000ULL) == ELF_EENTRY);

    /* 4 个 phdr，最后一个是 PT_NOTE：必须被忽略，仍然合法。 */
    build_valid(g_tmp);
    wr16(g_tmp, 56, 4);
    wr32(g_tmp, 64 + 3 * 56 + 0, ELF_PT_NOTE);
    rc = elf64_check(g_tmp, IMG_SIZE, ELF_USER_VA_MIN, ELF_USER_VA_MAX, &plan);
    CHECK(rc == ELF_OK);
    CHECK(plan.seg_count == 3);

    /* 全部改成 PT_NOTE：没有 PT_LOAD。 */
    build_valid(g_tmp);
    wr32(g_tmp, 64 + 0, ELF_PT_NOTE);
    wr32(g_tmp, 64 + 56, ELF_PT_NOTE);
    wr32(g_tmp, 64 + 2 * 56, ELF_PT_NOTE);
    CHECK(elf64_check(g_tmp, IMG_SIZE, ELF_USER_VA_MIN, ELF_USER_VA_MAX, NULL) ==
          ELF_ENOSEG);

    /* 0 长度 PT_LOAD 合法但不映射。 */
    build_valid(g_tmp);
    set_phdr(g_tmp, 1, 1, 4, 0x2000, 0x40001000ULL, 0, 0);
    CHECK(elf64_check(g_tmp, IMG_SIZE, ELF_USER_VA_MIN, ELF_USER_VA_MAX, &plan) ==
          ELF_OK);
    CHECK(plan.seg[1].pages == 0);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 加载测试                                                                   */
/* ------------------------------------------------------------------------- */

static int test_load(void)
{
    struct elf64_image plan;
    struct elf64_load_info info;
    struct elf_env env;
    int rc;
    int idx;
    unsigned char *pg;

    build_valid(g_img);
    fake_reset();
    env = make_env();
    memset(&plan, 0, sizeof(plan));
    memset(&info, 0, sizeof(info));
    rc = elf64_load(g_img, IMG_SIZE, &env, ELF_USER_VA_MIN, ELF_USER_VA_MAX,
                    &plan, &info);
    CHECK(rc == ELF_OK);
    CHECK(info.segs == 3);
    CHECK(info.pages == 3);
    CHECK(info.entry == 0x40000004ULL);
    CHECK(info.base == 0x40000000ULL);
    CHECK(info.end == 0x40002030ULL);
    CHECK(info.bss_bytes == 0x1000ULL); /* (0x1000-0x20) + (0x30-0x10) */
    CHECK(plan.seg_count == 3);
    CHECK(g_map_count == 3);
    CHECK(g_frees == 0);
    CHECK(g_unmaps == 0);

    /* 权限：text=U+RX、rodata=U+R+NX、data=U+RW+NX。 */
    CHECK(find_map(0x40000000ULL, &idx));
    CHECK(idx == 0);
    CHECK(g_map[0].flags == ELF_MAP_USER);
    CHECK(find_map(0x40001000ULL, &idx));
    CHECK(g_map[1].flags == (ELF_MAP_USER | ELF_MAP_NX));
    CHECK(find_map(0x40002000ULL, &idx));
    CHECK(g_map[2].flags == (ELF_MAP_USER | ELF_MAP_WRITE | ELF_MAP_NX));

    /* 文件字节 + .bss 清零。 */
    CHECK(find_map(0x40000000ULL, &idx));
    pg = f_ptr(NULL, g_map[idx].phys);
    CHECK(pg != NULL);
    CHECK(pg[0] == 0xA0);
    CHECK(pg[0x1F] == 0xBF);
    CHECK(pg[0x20] == 0x00); /* 文件尾之后 */
    CHECK(pg[0xFFF] == 0x00);
    CHECK(find_map(0x40002000ULL, &idx));
    pg = f_ptr(NULL, g_map[idx].phys);
    CHECK(pg != NULL);
    CHECK(pg[0] == 0xC0);
    CHECK(pg[0x0F] == 0xCF);
    CHECK(pg[0x10] == 0x00); /* memsz-filesz = 0x20 的 .bss */
    CHECK(pg[0x2F] == 0x00);
    CHECK(find_map(0x40001000ULL, &idx));
    pg = f_ptr(NULL, g_map[idx].phys);
    CHECK(pg != NULL);
    CHECK(pg[0] == 0xB0);

    /* out 参数可为 NULL。 */
    fake_reset();
    CHECK(elf64_load(g_img, IMG_SIZE, &env, ELF_USER_VA_MIN, ELF_USER_VA_MAX,
                     NULL, NULL) == ELF_OK);
    CHECK(g_map_count == 3);

    /* env 回调缺失。 */
    {
        struct elf_env bad = make_env();

        bad.map_page = NULL;
        CHECK(elf64_load(g_img, IMG_SIZE, &bad, ELF_USER_VA_MIN, ELF_USER_VA_MAX,
                         NULL, NULL) == ELF_EINPUT);
    }

    /* 坏镜像：load 透传 check 的错误码。 */
    CHECK(elf64_load(g_img, 32, &env, ELF_USER_VA_MIN, ELF_USER_VA_MAX, NULL,
                     NULL) == ELF_ESIZE);

    /* alloc 在第二页失败：第一页必须 unmap+free，页池回到全空。 */
    fake_reset();
    env = make_env();
    g_alloc_fail_after = 1;
    rc = elf64_load(g_img, IMG_SIZE, &env, ELF_USER_VA_MIN, ELF_USER_VA_MAX,
                    NULL, NULL);
    CHECK(rc == ELF_ENOMEM);
    CHECK(g_map_count == 0);
    CHECK(g_unmaps == 1);
    CHECK(count_used() == 0);

    /* map 在第二页失败：同样回滚。 */
    fake_reset();
    env = make_env();
    g_map_fail_after = 1;
    rc = elf64_load(g_img, IMG_SIZE, &env, ELF_USER_VA_MIN, ELF_USER_VA_MAX,
                    NULL, NULL);
    CHECK(rc == ELF_EMAP);
    CHECK(g_map_count == 0);
    CHECK(g_unmaps == 1);
    CHECK(count_used() == 0);

    /* 第三页分配失败：前两页回滚。 */
    fake_reset();
    env = make_env();
    g_alloc_fail_after = 2;
    rc = elf64_load(g_img, IMG_SIZE, &env, ELF_USER_VA_MIN, ELF_USER_VA_MAX,
                    NULL, NULL);
    CHECK(rc == ELF_ENOMEM);
    CHECK(g_unmaps == 2);
    CHECK(count_used() == 0);
    return 0;
}

int test_elf(void)
{
    test_check();
    test_load();
    return 0;
}
