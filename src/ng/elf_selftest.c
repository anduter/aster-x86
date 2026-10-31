/*
 * ng WP1-7: ELF64 加载器 + ring3 hello 自检。
 *
 * 证据行（run-ng 全部命中才算 PASS）：
 *   ng: elf64 header ok
 *   ng: elf64 header entry=0x... phnum=... type=2 machine=62
 *   ng: elf seg0/1/2 ...                 （段边界/权限/PTE 原值）
 *   ng: elf64 bad image rejected (n=23)
 *   ng: elf64 map ok (segs=3)
 *   ng: elf64 load base=... end=... entry=... pages=... bss=...
 *   ng: user stack 16KiB ok ...
 *   ng: syscall stack per-thread ok ...
 *   ng: uaccess ok (bad ptr EFAULT)
 *   ng: elf64 data/bss ok
 *   ng: ring3 exec ok
 *   ng: ring3 cr3=0x... (as pml4)
 *   <用户态> ng: ring3 hello
 *   ng: ring3 exit ok
 *   ng: WP1-7 selftest PASS              （main.c）
 *
 * 所有负向验证都是"改一份内嵌镜像的副本，再调 elf64_check"，不需要额外文件
 * 系统；host 侧用 tests/unit-ng/test_elf.c 覆盖同一组规则。
 */
#include "elf_selftest.h"

#include "elf.h"
#include "gdt.h"
#include "io.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "syscall.h"
#include "thread.h"
#include "uaccess.h"
#include "useras.h"

/* build-ng.ps1 生成的 .rodata 符号（.incbin obj-ng/hello.elf）。 */
extern const u8 ng_user_elf_start[];
extern const u8 ng_user_elf_end[];

#define BAD_IMAGE_MAX 16384u
#define USER_STACK_PAGES 4u
#define USER_STACK_BYTES (USER_STACK_PAGES * 0x1000ULL)

static u8 g_bad_image[BAD_IMAGE_MAX] __attribute__((aligned(16)));
static int g_fails;

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void fail_item(const char *what)
{
    g_fails++;
    serial_puts("ng: ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

static void wr8(u8 *p, u32 off, u32 v)
{
    p[off] = (u8)v;
}

static void wr16(u8 *p, u32 off, u32 v)
{
    p[off] = (u8)(v & 0xFFu);
    p[off + 1] = (u8)((v >> 8) & 0xFFu);
}

static void wr32(u8 *p, u32 off, u32 v)
{
    p[off] = (u8)(v & 0xFFu);
    p[off + 1] = (u8)((v >> 8) & 0xFFu);
    p[off + 2] = (u8)((v >> 16) & 0xFFu);
    p[off + 3] = (u8)((v >> 24) & 0xFFu);
}

static void wr64(u8 *p, u32 off, u64 v)
{
    wr32(p, off, (u32)(v & 0xFFFFFFFFULL));
    wr32(p, off + 4, (u32)((v >> 32) & 0xFFFFFFFFULL));
}

static u64 rd64(const u8 *p, u32 off)
{
    return (u64)p[off] | ((u64)p[off + 1] << 8) | ((u64)p[off + 2] << 16) |
           ((u64)p[off + 3] << 24) | ((u64)p[off + 4] << 32) |
           ((u64)p[off + 5] << 40) | ((u64)p[off + 6] << 48) |
           ((u64)p[off + 7] << 56);
}

static void copy_bytes(u8 *dst, const u8 *src, u64 n)
{
    u64 i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* ------------------------------------------------------------------------- */
/* elf_env 的内核实现：PMM 页 + 用户地址空间                                  */
/* ------------------------------------------------------------------------- */

static u64 k_alloc_page(void *ctx)
{
    (void)ctx;
    return pmm_alloc_page();
}

static void k_free_page(void *ctx, u64 phys)
{
    (void)ctx;
    (void)pmm_free_page(phys);
}

static u8 *k_page_ptr(void *ctx, u64 phys)
{
    (void)ctx;
    return (u8 *)(usize)phys; /* 恒等映射：物理地址可直接写 */
}

static int k_map_page(void *ctx, u64 va, u64 phys, u64 flags)
{
    struct user_as *as = (struct user_as *)ctx;
    u64 pg = PG_USER;

    if ((flags & ELF_MAP_WRITE) != 0) {
        pg |= PG_WRITE;
    }
    if ((flags & ELF_MAP_NX) != 0 && pg_nx_on()) {
        pg |= PG_NX;
    }
    return (as_map(as, va, phys, pg) == AS_OK) ? 0 : -1;
}

static void k_unmap_page(void *ctx, u64 va)
{
    (void)as_unmap_free((struct user_as *)ctx, va);
}

/* ------------------------------------------------------------------------- */
/* per-thread 内核栈：一个真实线程 + 两次同步                                  */
/* ------------------------------------------------------------------------- */

static volatile int g_rsp0_done;
static volatile u64 g_probe_top;
static volatile u64 g_probe_rsp0;
static volatile u64 g_probe_tss;

static void rsp0_probe(void *arg)
{
    thread_t *self = thread_self();

    (void)arg;
    g_probe_top = (self != NULL) ? (u64)(usize)self->stack_top : 0;
    g_probe_rsp0 = syscall_kernel_stack_sync();
    g_probe_tss = gdt_tss_rsp0();
    g_rsp0_done = 1;
    thread_exit();
}

static int check_per_thread_stack(void)
{
    thread_t *t;
    u64 boot_top;
    u32 i;

    g_rsp0_done = 0;
    g_probe_top = 0;
    g_probe_rsp0 = 0;
    g_probe_tss = 0;
    t = thread_create(rsp0_probe, NULL);
    if (t == NULL) {
        fail_item("syscall stack");
        return 1;
    }
    for (i = 0; i < 100000u && !g_rsp0_done; i++) {
        sched_yield();
    }
    if (!g_rsp0_done) {
        fail_item("syscall stack");
        return 1;
    }
    /* 回到 boot 线程后再同步一次：RSP0 应该跟着当前线程变回去。 */
    boot_top = syscall_kernel_stack_sync();
    if (g_probe_top == 0 || g_probe_rsp0 != g_probe_top ||
        g_probe_tss != g_probe_top || boot_top == 0 || boot_top == g_probe_top) {
        fail_item("syscall stack");
        return 1;
    }

    serial_puts("ng: syscall stack per-thread ok (probe=");
    serial_puthex(g_probe_top);
    serial_puts(" rsp0=");
    serial_puthex(g_probe_rsp0);
    serial_puts(" tss.rsp0=");
    serial_puthex(g_probe_tss);
    serial_puts(" boot=");
    serial_puthex(boot_top);
    serial_puts(")\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* ELF 负向验证                                                               */
/* ------------------------------------------------------------------------- */

struct bad_mutation {
    u32 off;
    u32 width; /* 1/2/4/8 */
    u64 value;
    int want;
    const char *name;
};

static int run_bad_case(const u8 *blob, u64 size, const struct bad_mutation *m)
{
    int rc;

    copy_bytes(g_bad_image, blob, size);
    switch (m->width) {
    case 1:
        wr8(g_bad_image, m->off, (u32)m->value);
        break;
    case 2:
        wr16(g_bad_image, m->off, (u32)m->value);
        break;
    case 4:
        wr32(g_bad_image, m->off, (u32)m->value);
        break;
    default:
        wr64(g_bad_image, m->off, m->value);
        break;
    }
    rc = elf64_check(g_bad_image, size, NG_USER_VA_MIN, NG_USER_VA_MAX, NULL);
    if (rc != m->want) {
        serial_puts("ng: elf64 bad case FAIL ");
        serial_puts(m->name);
        serial_puts(" (want ");
        serial_putdec((u64)(-m->want));
        serial_puts(" got ");
        serial_putdec((u64)(-rc));
        serial_puts(")\r\n");
        return 0;
    }
    return 1;
}

/* 全 phdr 改成 PT_NOTE：没有任何 PT_LOAD。 */
static int run_no_load_case(const u8 *blob, u64 size, u32 phoff, u32 phnum)
{
    u32 i;
    int rc;

    copy_bytes(g_bad_image, blob, size);
    for (i = 0; i < phnum; i++) {
        wr32(g_bad_image, phoff + i * ELF_PHDR_SIZE, ELF_PT_NOTE);
    }
    rc = elf64_check(g_bad_image, size, NG_USER_VA_MIN, NG_USER_VA_MAX, NULL);
    return (rc == ELF_ENOSEG) ? 1 : 0;
}

static int check_bad_images(const u8 *blob, u64 size, u32 phoff, u32 phnum)
{
    const u32 p0 = phoff;
    const u32 p1 = phoff + ELF_PHDR_SIZE;
    const u32 p2 = phoff + 2u * ELF_PHDR_SIZE;
    struct bad_mutation muts[22];
    u32 n = 0;
    u32 accepted = 0;
    u32 i;

    muts[n].off = 0;
    muts[n].width = 1;
    muts[n].value = 0;
    muts[n].want = ELF_EHDR;
    muts[n++].name = "ident.magic";
    muts[n].off = 4;
    muts[n].width = 1;
    muts[n].value = 1;
    muts[n].want = ELF_EHDR;
    muts[n++].name = "ident.class";
    muts[n].off = 5;
    muts[n].width = 1;
    muts[n].value = 2;
    muts[n].want = ELF_EHDR;
    muts[n++].name = "ident.data";
    muts[n].off = 16;
    muts[n].width = 2;
    muts[n].value = 1;
    muts[n].want = ELF_EHDR;
    muts[n++].name = "e_type";
    muts[n].off = 18;
    muts[n].width = 2;
    muts[n].value = 3;
    muts[n].want = ELF_EHDR;
    muts[n++].name = "e_machine";
    muts[n].off = 20;
    muts[n].width = 4;
    muts[n].value = 2;
    muts[n].want = ELF_EHDR;
    muts[n++].name = "e_version";
    muts[n].off = 52;
    muts[n].width = 2;
    muts[n].value = 63;
    muts[n].want = ELF_EHDR;
    muts[n++].name = "e_ehsize";
    muts[n].off = 54;
    muts[n].width = 2;
    muts[n].value = 55;
    muts[n].want = ELF_EPHDR;
    muts[n++].name = "e_phentsize";
    muts[n].off = 56;
    muts[n].width = 2;
    muts[n].value = 0;
    muts[n].want = ELF_EPHDR;
    muts[n++].name = "e_phnum=0";
    muts[n].off = 56;
    muts[n].width = 2;
    muts[n].value = ELF_MAX_PHDRS + 1u;
    muts[n].want = ELF_EPHDR;
    muts[n++].name = "e_phnum>max";
    muts[n].off = 32;
    muts[n].width = 8;
    muts[n].value = 4; /* e_phoff 未按 8 字节对齐 */
    muts[n].want = ELF_EPHDR;
    muts[n++].name = "e_phoff unaligned";
    muts[n].off = 32;
    muts[n].width = 8;
    muts[n].value = size; /* 表在文件末尾之外（0 项可用） */
    muts[n].want = ELF_EPHDR;
    muts[n++].name = "e_phoff past end";
    muts[n].off = p2 + 32;
    muts[n].width = 8;
    muts[n].value = rd64(blob, p2 + 40) + 1; /* filesz = memsz + 1（hello 的
                                              * .bss 大小会随 WP 变化，别写死） */
    muts[n].want = ELF_ESEG;
    muts[n++].name = "p_filesz>p_memsz";
    muts[n].off = p0 + 8;
    muts[n].width = 8;
    muts[n].value = size - 8u; /* offset+filesz 越界 */
    muts[n].want = ELF_EOFF;
    muts[n++].name = "p_offset+filesz>image";
    muts[n].off = p0 + 4;
    muts[n].width = 4;
    muts[n].value = ELF_PF_R | ELF_PF_W | ELF_PF_X; /* W^X */
    muts[n].want = ELF_ESEG;
    muts[n++].name = "w^x segment";
    muts[n].off = p0 + 4;
    muts[n].width = 4;
    muts[n].value = ELF_PF_R | 0x10u; /* 保留标志位 */
    muts[n].want = ELF_ESEG;
    muts[n++].name = "p_flags reserved";
    muts[n].off = p0 + 48;
    muts[n].width = 8;
    muts[n].value = 0x800; /* < 4 KiB / 非页对齐粒度 */
    muts[n].want = ELF_ESEG;
    muts[n++].name = "p_align<page";
    muts[n].off = p0 + 16;
    muts[n].width = 8;
    muts[n].value = 0x40000800ULL; /* 段起始未页对齐 */
    muts[n].want = ELF_ESEG;
    muts[n++].name = "p_vaddr unaligned";
    muts[n].off = p0 + 16;
    muts[n].width = 8;
    muts[n].value = 0x30000000ULL; /* 低于用户窗口 */
    muts[n].want = ELF_EVA;
    muts[n++].name = "p_vaddr<user_min";
    muts[n].off = p0 + 16;
    muts[n].width = 8;
    muts[n].value = 0x8000000000ULL; /* 高于用户窗口（512 GiB） */
    muts[n].want = ELF_EVA;
    muts[n++].name = "p_vaddr>user_max";
    muts[n].off = 24;
    muts[n].width = 8;
    muts[n].value = 0x40002000ULL; /* 入口落在只读数据段 */
    muts[n].want = ELF_EENTRY;
    muts[n++].name = "entry outside exec";
    muts[n].off = p1 + 16;
    muts[n].width = 8;
    muts[n].value = 0x40000000ULL; /* 与 .text 段重叠 */
    muts[n].want = ELF_EOVERLAP;
    muts[n++].name = "segments overlap";

    for (i = 0; i < n; i++) {
        accepted += (u32)run_bad_case(blob, size, &muts[i]);
    }
    accepted += (u32)run_no_load_case(blob, size, phoff, phnum);

    if (accepted != n + 1) {
        fail_item("elf64 bad image");
    } else {
        serial_puts("ng: elf64 bad image rejected (n=");
        serial_putdec((u64)(n + 1));
        serial_puts(")\r\n");
    }
    return (accepted == n + 1) ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 段权限 / 内容证据                                                          */
/* ------------------------------------------------------------------------- */

static const char *seg_flags_str(const struct elf64_seg *s)
{
    if ((s->flags & ELF_PF_X) != 0) {
        return "R-X";
    }
    if ((s->flags & ELF_PF_W) != 0) {
        return "RW";
    }
    return "R";
}

static int check_seg_pte(const struct user_as *as, const struct elf64_seg *s)
{
    u64 pte = as_walk(as, s->vaddr);
    int want_w = ((s->flags & ELF_PF_W) != 0) ? 1 : 0;
    int want_nx = ((s->flags & ELF_PF_X) == 0) ? 1 : 0;
    int nx = pg_nx_on();
    int got_w = (pte & PG_WRITE) ? 1 : 0;
    int got_nx = (pte & PG_NX) ? 1 : 0;

    if ((pte & PG_PRESENT) == 0 || (pte & PG_USER) == 0 || (pte & PG_PS) != 0) {
        return 0;
    }
    if (got_w != want_w) {
        return 0;
    }
    if (nx && got_nx != want_nx) {
        return 0;
    }
    return 1;
}

static void print_seg(u32 i, const struct elf64_seg *s, const struct user_as *as)
{
    serial_puts("ng: elf seg");
    serial_putdec(i);
    serial_puts(" vaddr=");
    serial_puthex(s->vaddr);
    serial_puts(" filesz=");
    serial_puthex(s->filesz);
    serial_puts(" memsz=");
    serial_puthex(s->memsz);
    serial_puts(" pages=");
    serial_putdec(s->pages);
    serial_puts(" flags=");
    serial_puthex(s->flags);
    serial_puts(" (");
    serial_puts(seg_flags_str(s));
    serial_puts(") pte=");
    serial_puthex(as_walk(as, s->vaddr));
    serial_puts("\r\n");
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int elf_selftest(void)
{
    const u8 *blob = ng_user_elf_start;
    u64 size;
    struct elf64_image plan;
    struct elf64_load_info info;
    struct elf_env env;
    struct user_as as;
    struct ring3_result res;
    int as_ready = 0;
    int rc;
    u32 i;
    u32 phoff;
    u32 phnum;

    g_fails = 0;
    size = (u64)(usize)ng_user_elf_end - (u64)(usize)ng_user_elf_start;
    if (size < ELF_EHDR_SIZE || size > BAD_IMAGE_MAX) {
        serial_puts("ng: elf64 image size=");
        serial_putdec(size);
        serial_puts(" unsupported\r\n");
        fail_item("elf64 header");
        return g_fails;
    }

    /* ---- 1. 头/段校验 ------------------------------------------------- */
    rc = elf64_check(blob, size, NG_USER_VA_MIN, NG_USER_VA_MAX, &plan);
    if (rc != ELF_OK) {
        serial_puts("ng: elf64 check rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("elf64 header");
        return g_fails;
    }
    if (plan.base != NG_USER_VA_MIN || plan.entry < plan.base ||
        plan.entry >= plan.end || plan.seg_count == 0) {
        fail_item("elf64 header");
        return g_fails;
    }
    serial_puts("ng: elf64 header ok\r\n");
    serial_puts("ng: elf64 header entry=");
    serial_puthex(plan.entry);
    serial_puts(" phnum=");
    serial_putdec((u64)plan.phnum);
    serial_puts(" type=2 machine=62\r\n");
    for (i = 0; i < plan.seg_count; i++) {
        const struct elf64_seg *s = &plan.seg[i];

        serial_puts("ng: elf seg");
        serial_putdec(i);
        serial_puts(" vaddr=");
        serial_puthex(s->vaddr);
        serial_puts(" filesz=");
        serial_puthex(s->filesz);
        serial_puts(" memsz=");
        serial_puthex(s->memsz);
        serial_puts(" pages=");
        serial_putdec(s->pages);
        serial_puts(" flags=");
        serial_puthex(s->flags);
        serial_puts(" (");
        serial_puts(seg_flags_str(s));
        serial_puts(")\r\n");
    }

    /* ---- 2. 坏镜像拒绝 ------------------------------------------------ */
    phoff = 0;
    phnum = 0;
    {
        const struct elf64_ehdr *eh = (const struct elf64_ehdr *)blob;

        phoff = (u32)eh->phoff;
        phnum = eh->phnum;
    }
    g_fails += check_bad_images(blob, size, phoff, phnum);

    /* ---- 3. per-thread 内核栈 ----------------------------------------- */
    g_fails += check_per_thread_stack();

    /* ---- 4. 用户地址空间 + ELF 逐段映射 ------------------------------- */
    if (as_create(&as) != AS_OK) {
        fail_item("elf64 map");
        return g_fails;
    }
    as_ready = 1;
    env.ctx = &as;
    env.alloc_page = k_alloc_page;
    env.free_page = k_free_page;
    env.page_ptr = k_page_ptr;
    env.map_page = k_map_page;
    env.unmap_page = k_unmap_page;

    rc = elf64_load(blob, size, &env, NG_USER_VA_MIN, NG_USER_VA_MAX, &plan,
                    &info);
    if (rc != ELF_OK) {
        serial_puts("ng: elf64 load rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("elf64 map");
        goto cleanup;
    }
    serial_puts("ng: elf64 map ok (segs=");
    serial_putdec(info.segs);
    serial_puts(")\r\n");
    serial_puts("ng: elf64 load base=");
    serial_puthex(info.base);
    serial_puts(" end=");
    serial_puthex(info.end);
    serial_puts(" entry=");
    serial_puthex(info.entry);
    serial_puts(" pages=");
    serial_putdec(info.pages);
    serial_puts(" bss=");
    serial_puthex(info.bss_bytes);
    serial_puts("\r\n");

    for (i = 0; i < plan.seg_count; i++) {
        if (!check_seg_pte(&as, &plan.seg[i])) {
            fail_item("elf64 seg pte");
        }
        print_seg(i, &plan.seg[i], &as);
    }

    /* ---- 5. 用户栈（16 KiB + 守护页）--------------------------------- */
    {
        int stack_ok = 1;

        for (i = 0; i < USER_STACK_PAGES; i++) {
            u64 phys = pmm_alloc_page();
            u64 flags = PG_USER | PG_WRITE;

            if (phys == 0) {
                stack_ok = 0;
                break;
            }
            if (pg_nx_on()) {
                flags |= PG_NX;
            }
            if (as_map(&as, NG_USER_STACK_BASE + (u64)i * 0x1000ULL, phys, flags) !=
                AS_OK) {
                (void)pmm_free_page(phys);
                stack_ok = 0;
                break;
            }
        }
        if (!stack_ok || as_walk(&as, NG_USER_STACK_GUARD) != 0 ||
            as_walk(&as, NG_USER_STACK_BASE) == 0 ||
            as_walk(&as, NG_USER_STACK_TOP - 1) == 0) {
            fail_item("user stack");
            goto cleanup;
        }
        serial_puts("ng: user stack 16KiB ok (base=");
        serial_puthex(NG_USER_STACK_BASE);
        serial_puts(" top=");
        serial_puthex(NG_USER_STACK_TOP);
        serial_puts(" guard=unmapped)\r\n");
    }

    /* ---- 6. 切到用户地址空间：copyin/copyout + 数据证据 --------------- */
    if (as_switch(&as) != AS_OK) {
        fail_item("ring3");
        goto cleanup;
    }
    {
        u8 tmp[16];
        u8 pattern[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
        u8 readback[8];
        int ua_ok = 1;
        u32 k;

        for (k = 0; k < 8; k++) {
            readback[k] = 0;
        }
        /* 好指针：读用户 .data / .bss，写用户栈并读回。 */
        if (copyin(tmp, 0x40002000ULL, 8) != UACCESS_OK ||
            rd64(tmp, 0) != 0x1122334455667788ULL) {
            ua_ok = 0;
        }
        if (copyin(tmp, 0x40002008ULL, 8) != UACCESS_OK || rd64(tmp, 0) != 0) {
            ua_ok = 0;
        }
        if (copyout(NG_USER_STACK_TOP - 32, pattern, 8) != UACCESS_OK ||
            copyin(readback, NG_USER_STACK_TOP - 32, 8) != UACCESS_OK) {
            ua_ok = 0;
        }
        for (k = 0; k < 8; k++) {
            if (readback[k] != pattern[k]) {
                ua_ok = 0;
            }
        }
        /* 坏指针：null / 内核地址 / 只读用户页，全部必须 EFAULT（不 #PF）。 */
        if (copyin(tmp, 0, 8) != UACCESS_EFAULT ||
            copyin(tmp, 0x100000ULL, 8) != UACCESS_EFAULT ||
            copyout(0x40000000ULL, pattern, 8) != UACCESS_EFAULT) {
            ua_ok = 0;
        }
        if (!ua_ok) {
            fail_item("uaccess");
        } else {
            serial_puts("ng: uaccess ok (bad ptr EFAULT)\r\n");
            serial_puts("ng: elf64 data/bss ok\r\n");
        }
    }

    /* ---- 7. ring3 执行 hello ------------------------------------------ */
    (void)syscall_kernel_stack_sync();
    serial_puts("ng: ring3 exec ok\r\n");
    serial_puts("ng: ring3 cr3=");
    serial_puthex(pg_cr3());
    serial_puts(" (as pml4=");
    serial_puthex(as.pml4_phys);
    serial_puts(")\r\n");

    rc = syscall_run_user(info.entry, NG_USER_STACK_TOP - 16, NG_USER_STACK_BASE,
                          NG_USER_STACK_TOP, &res);
    /* 无论用户程序怎么退出，都先回内核地址空间再继续。 */
    as_switch_kernel();

    if (rc != 0) {
        fail_item("ring3 exec");
    } else if (res.exit_code != 0 || res.writes < 1 || res.bad_writes != 0 ||
               !res.user_rsp_ok || res.fs_calls < 6) {
        /* WP2-1：hello 现在除了 "ng: ring3 hello" 还会跑 fs demo（open /
         * write / read / stat / close / listdir），所以 writes 不再固定为 1，
         * 但必须至少有一次用户态 write，且 20..27 至少被调用 6 次。 */
        fail_item("ring3 exec");
    } else {
        serial_puts("ng: ring3 exit ok\r\n");
    }

    /* ---- 8. 回收 ------------------------------------------------------ */
cleanup:
    if (as_ready) {
        as_switch_kernel();
        as_destroy(&as);
    }
    return g_fails;
}
