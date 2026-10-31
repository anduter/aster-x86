/*
 * ng WP2-6: 加载并运行用户态 shell（见 wp26.h / src/apps/shell/shell.c）。
 *
 * 这个文件只做"内核侧的启动 + 收尾"，不解析任何命令：
 *   - ELF64 校验/加载复用 WP1-7 的 elf64_check()/elf64_load()，环境回调与
 *     elf_selftest.c 同构（PMM 页 + user_as 页表）；
 *   - 用户栈 16 KiB（4 页，RW+NX），基址/守护页来自 useras.h；
 *   - syscall_run_user() 进 ring3；shell 用 syscall 1 退出后回到这里；
 *   - 退出后检查：rc/exit_code/user_rsp/写次数/坏指针，并读回 shell 会话
 *     种下的 /tmp/s.txt（"hi\n"）与 pkg 列表计数作为端到端证据。
 *
 * 交互（人手敲）和脚本（run-ng 经 QMP 注入）走的是同一条路径：唯一区别是
 * 会话里有没有执行 `echo hi > /tmp/s.txt` / `pkg list`。所以这两个会话证据
 * 只打证据行、不参与 PASS/FAIL；PASS 只看 shell 是否干净地跑完并退出。
 */
#include "wp26.h"

#include "elf.h"
#include "fs_syscall.h"
#include "paging.h"
#include "pkg.h"
#include "pmm.h"
#include "serial.h"
#include "syscall.h"
#include "useras.h"
#include "vfs.h"

/* build-ng.ps1 生成的 .rodata 符号（.incbin obj-ng/shell.elf）。 */
extern const u8 ng_shell_elf_start[];
extern const u8 ng_shell_elf_end[];

#define WP26_STACK_PAGES 4u

static int g_fails;

static void fail_item(const char *what)
{
    g_fails++;
    serial_puts("ng: shell ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

/* ------------------------------------------------------------------------- */
/* ELF 加载环境：PMM 页 + 用户地址空间（与 elf_selftest.c 同构）              */
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
/* 会话证据                                                                  */
/* ------------------------------------------------------------------------- */

/* shell 的 `echo hi > /tmp/s.txt` 是否真的落到了 VFS 上（3 字节 "hi\n"）。 */
static int session_file_ok(void)
{
    struct vfs_ctx *c = fs_sys_ctx();
    char buf[8];
    int fd;
    int n;

    if (c == NULL) {
        return 0;
    }
    fd = vfs_open(c, "/tmp/s.txt", VFS_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = vfs_read(c, fd, buf, (u64)sizeof(buf));
    (void)vfs_close(c, fd);
    if (n != 3) {
        return 0;
    }
    return (buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n') ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                      */
/* ------------------------------------------------------------------------- */

int wp26_selftest(void)
{
    const u8 *blob = ng_shell_elf_start;
    u64 size = (u64)(usize)ng_shell_elf_end - (u64)(usize)ng_shell_elf_start;
    struct elf64_image plan;
    struct elf64_load_info info;
    struct elf_env env;
    struct user_as as;
    struct ring3_result res;
    struct pkg_stats pkgs_before;
    const struct pkg_stats *pkgs_after;
    int as_ready = 0;
    int in_user = 0;
    int session_file;
    u64 pkg_delta = 0;
    int rc;
    u32 i;

    g_fails = 0;
    res.syscalls = 0;
    res.writes = 0;
    res.bytes = 0;
    res.fs_calls = 0;
    res.exit_code = 0;
    res.exited = 0;
    res.user_rsp_ok = 0;
    res.bad_writes = 0;
    pkgs_before.packages = 0;
    pkgs_before.lists = 0;
    pkgs_before.verifies = 0;
    pkgs_before.failures = 0;
    pkgs_before.seeds = 0;
    pkgs_before.hash_calls = 0;
    {
        const struct pkg_stats *p = pkg_stat();

        if (p != NULL) {
            pkgs_before = *p;
        }
    }

    /* ---- 1. ELF 头/段校验 --------------------------------------------- */
    rc = elf64_check(blob, size, NG_USER_VA_MIN, NG_USER_VA_MAX, &plan);
    if (rc != ELF_OK) {
        serial_puts("ng: shell elf rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("elf");
        return g_fails;
    }
    if (plan.base != NG_USER_VA_MIN || plan.entry < plan.base ||
        plan.entry >= plan.end || plan.seg_count == 0) {
        fail_item("elf");
        return g_fails;
    }
    serial_puts("ng: shell elf ok (size=");
    serial_putdec(size);
    serial_puts(" entry=");
    serial_puthex(plan.entry);
    serial_puts(" segs=");
    serial_putdec((u64)plan.seg_count);
    serial_puts(")\r\n");

    /* ---- 2. 用户地址空间 + 逐段映射 ----------------------------------- */
    if (as_create(&as) != AS_OK) {
        fail_item("as");
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
        serial_puts("ng: shell load rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("load");
        goto cleanup;
    }
    serial_puts("ng: shell load ok (base=");
    serial_puthex(info.base);
    serial_puts(" end=");
    serial_puthex(info.end);
    serial_puts(" pages=");
    serial_putdec((u64)info.pages);
    serial_puts(")\r\n");

    /* ---- 3. 16 KiB 用户栈（4 页 RW+NX；下方一页不映射）--------------- */
    for (i = 0; i < WP26_STACK_PAGES; i++) {
        u64 phys = pmm_alloc_page();
        u64 flags = PG_USER | PG_WRITE;

        if (phys == 0) {
            fail_item("stack");
            goto cleanup;
        }
        if (pg_nx_on()) {
            flags |= PG_NX;
        }
        if (as_map(&as, NG_USER_STACK_BASE + (u64)i * 0x1000ULL, phys,
                   flags) != AS_OK) {
            (void)pmm_free_page(phys);
            fail_item("stack");
            goto cleanup;
        }
    }
    if (as_walk(&as, NG_USER_STACK_GUARD) != 0 ||
        as_walk(&as, NG_USER_STACK_TOP - 1) == 0) {
        fail_item("stack");
        goto cleanup;
    }

    /* ---- 4. 进 ring3：shell 一直跑到 syscall 1 ------------------------ */
    if (as_switch(&as) != AS_OK) {
        fail_item("launch");
        goto cleanup;
    }
    in_user = 1;
    (void)syscall_kernel_stack_sync();
    serial_puts("ng: shell launch ok\r\n");

    rc = syscall_run_user(info.entry, NG_USER_STACK_TOP - 16,
                          NG_USER_STACK_BASE, NG_USER_STACK_TOP, &res);
    as_switch_kernel();
    in_user = 0;

    serial_puts("ng: shell exit (code=");
    serial_putdec(res.exit_code);
    serial_puts(" syscalls=");
    serial_putdec(res.syscalls);
    serial_puts(" writes=");
    serial_putdec(res.writes);
    serial_puts(" fs=");
    serial_putdec(res.fs_calls);
    serial_puts(")\r\n");

    if (rc != 0 || !res.exited || res.exit_code != 0 || res.writes == 0 ||
        res.syscalls == 0 || !res.user_rsp_ok || res.bad_writes != 0) {
        fail_item("session");
    } else {
        serial_puts("ng: shell session ok (exit=0 rsp=ok)\r\n");
    }

    /* ---- 5. 会话证据（脚本会话才有；不影响 PASS/FAIL）----------------- */
    session_file = session_file_ok();
    if (session_file) {
        serial_puts("ng: shell session file ok (s.txt=3)\r\n");
    } else {
        serial_puts("ng: shell session file: /tmp/s.txt not found "
                    "(interactive session)\r\n");
    }
    pkgs_after = pkg_stat();
    if (pkgs_after != NULL && pkgs_after->lists >= pkgs_before.lists) {
        pkg_delta = pkgs_after->lists - pkgs_before.lists;
    }
    if (pkg_delta > 0) {
        serial_puts("ng: shell session pkg ok (lists=+");
        serial_putdec(pkg_delta);
        serial_puts(")\r\n");
    } else {
        serial_puts("ng: shell session pkg: no package list in this session\r\n");
    }

cleanup:
    if (in_user) {
        as_switch_kernel();
    }
    if (as_ready) {
        as_destroy(&as);
    }
    return g_fails;
}
