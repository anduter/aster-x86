/*
 * ng WP2-6 / WP3-0: 加载并运行用户态 shell（见 src/apps/shell/shell.c）。
 *
 * WP3-0 起 shell 不再是"内核线程里的一个 ring3 程序"，而是一个真正的**进程**：
 *   - proc_init() 在启动时把内嵌的 shell ELF 种进 ramfs（/bin/shell）；
 *   - 本文件用 proc_spawn("/bin/shell") 建进程（新 AS + 新内核线程 + 用户栈），
 *     然后 proc_wait_pid() 阻塞等它退出（shell 的 exit syscall -> 进程僵尸）；
 *   - 退出后从 proc 槽里读回 ring3 统计（syscalls/writes/fs/exit_code），
 *     打印与 WP2-6 完全相同的证据行。
 *
 * 这样 shell 自己也能作为父进程 spawn 别的程序（`spawn <path>` 命令），
 * 跨进程 IPC/进程表的自检见 src/ng/proc_selftest.c。
 *
 * 交互（人手敲）和脚本（run-ng 经 QMP 注入）走同一条路径：唯一区别是会话里
 * 有没有执行 `echo hi > /tmp/s.txt` / `pkg list`。所以会话证据只打证据行、
 * 不参与 PASS/FAIL；PASS 只看 shell 是否干净地跑完并退出。
 */
#include "wp26.h"

#include "elf.h"
#include "fs_syscall.h"
#include "paging.h"
#include "pkg.h"
#include "proc.h"
#include "serial.h"
#include "useras.h"
#include "vfs.h"

/* build-ng.ps1 生成的 .rodata 符号（.incbin obj-ng/shell.elf）。 */
extern const u8 ng_shell_elf_start[];
extern const u8 ng_shell_elf_end[];

#define WP26_SHELL_PATH "/bin/shell"

static int g_fails;

static void fail_item(const char *what)
{
    g_fails++;
    serial_puts("ng: shell ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
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
    struct pkg_stats pkgs_before;
    const struct pkg_stats *pkgs_after;
    const struct proc *child = NULL;
    int session_file;
    u64 pkg_delta = 0;
    u32 pid = 0;
    int status = -1;
    int rc;

    g_fails = 0;
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

    /* ---- 1. ELF 头/段校验（纯校验；真正的加载在 proc_spawn 里）--------- */
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

    /* ---- 2. 建进程：新 AS、ELF 段、16 KiB 栈、内核线程 ----------------- */
    {
        const char *argv[2];

        argv[0] = WP26_SHELL_PATH;
        argv[1] = NULL;
        rc = proc_spawn(WP26_SHELL_PATH, argv, &pid);
    }
    if (rc != PROC_OK) {
        serial_puts("ng: shell spawn rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("load");
        return g_fails;
    }
    child = proc_slot(pid);
    if (child == NULL || child->as == NULL) {
        fail_item("load");
        return g_fails;
    }
    serial_puts("ng: shell load ok (base=");
    serial_puthex(child->load_base);
    serial_puts(" end=");
    serial_puthex(child->load_end);
    serial_puts(" pages=");
    serial_putdec(child->load_pages);
    serial_puts(")\r\n");
    serial_puts("ng: shell launch ok (pid=");
    serial_putdec(pid);
    serial_puts(" ppid=");
    serial_putdec(child->ppid);
    serial_puts(" as=");
    serial_puthex((u64)child->as_pml4);
    serial_puts(")\r\n");

    /* ---- 3. 等 shell 退出（阻塞；shell 的 exit syscall 唤醒本线程）----- */
    rc = proc_wait_pid(pid, &status, NULL);
    child = proc_slot(pid);
    if (rc != PROC_OK || child == NULL) {
        serial_puts("ng: shell wait rc=");
        serial_putdec((u64)(rc < 0 ? -rc : 0));
        serial_puts("\r\n");
        fail_item("session");
        return g_fails;
    }

    serial_puts("ng: shell exit (code=");
    serial_putdec(child->exit_code & 0xFFULL);
    serial_puts(" syscalls=");
    serial_putdec(child->syscalls);
    serial_puts(" writes=");
    serial_putdec(child->writes);
    serial_puts(" fs=");
    serial_putdec(child->fs_calls);
    serial_puts(")\r\n");

    if (status != 0 || child->exited == 0 || child->writes == 0 ||
        child->syscalls == 0 || child->user_rsp_ok == 0 ||
        child->bad_writes != 0 || (child->exit_code & 0xFFULL) != 0) {
        fail_item("session");
    } else {
        serial_puts("ng: shell session ok (exit=0 rsp=ok)\r\n");
    }

    /* ---- 4. 会话证据（脚本会话才有；不影响 PASS/FAIL）----------------- */
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

    /* ---- 5. 回收进程槽 ------------------------------------------------- */
    (void)proc_reap_pid(pid);
    return g_fails;
}
