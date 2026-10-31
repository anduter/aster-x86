/*
 * ng WP3-0: 进程内核胶水（spawn / wait / kill / exit / syscall 40..44）。
 *
 * 依赖链：
 *   - 纯状态机在 proc_table.c（pid/父子/退出/回收）；
 *   - spawn：fs_sys_read_file() 从 VFS 读 ELF -> elf64_check/elf64_load 进
 *     **新的** user_as -> 用户栈 4 页 + 映射页 -> argv 块 -> thread_create()；
 *   - 主线程：proc_thread_main() = syscall_run_user() 进 ring3；syscall 1 回到
 *     内核后 proc_exit_self() 收尾（AS/句柄/端口/状态/唤醒父进程）。
 *
 * 多进程的关键（见 docs/design/proc-ng.md）：
 *   1. 每个进程线程在 TCB 里记录自己的 PML4（t->pml4），调度器切换前写 CR3；
 *   2. 每个进程线程有独立的 SYSCALL 入口栈（t->syscall_top），因为 ring3_enter
 *      的恢复点放在 C 栈上，不能和 SYSCALL 入口共用栈顶；
 *   3. ring3/syscall 状态在 TCB（thread_uctx），A 阻塞、B 跑时互不覆盖；
 *   4. 进程自己的句柄表只记录"我拥有的端口/线程"，退出/被 kill 时只回收自己的。
 *
 * 阻塞语义（协作式调度，单核）：
 *   - wait：父线程 thread_sleep() 到被子进程退出时的 thread_wake() 叫醒；
 *   - kill：把目标线程从 ready/blocked 链摘下、置 zombie（栈由别的线程下一
 *     次调度回收）、摘掉它在端口等待链上的条目、关端口、销毁 AS。
 */
#include "proc.h"
#include "proc_str.h"

#include "elf.h"
#include "fbmap.h"
#include "fs_syscall.h"
#include "heap.h"
#include "io.h"
#include "ipc.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "shm.h"
#include "syscall.h"
#include "thread.h"
#include "uaccess.h"
#include "useras.h"
#include "vfs.h"

/* build-ng.ps1 生成的 .rodata 符号。 */
extern const u8 ng_shell_elf_start[];
extern const u8 ng_shell_elf_end[];
extern const u8 ng_proc_elf_start[];
extern const u8 ng_proc_elf_end[];
extern const u8 ng_user_elf_start[]; /* WP1-7 的 hello.S */
extern const u8 ng_user_elf_end[];
extern const u8 ng_displayd_elf_start[]; /* WP3-1 display server（L2a） */
extern const u8 ng_displayd_elf_end[];
extern const u8 ng_demo_elf_start[]; /* WP3-1 demo app（L3） */
extern const u8 ng_demo_elf_end[];
extern const u8 ng_wm_elf_start[]; /* WP3-2 shell/WM（L2b） */
extern const u8 ng_wm_elf_end[];
extern const u8 ng_sessiond_elf_start[]; /* WP3-3 session manager（L2c） */
extern const u8 ng_sessiond_elf_end[];

/* 内嵌 ELF 的落盘路径（ramfs /bin）。 */
#define PROC_BIN_SHELL "/bin/shell"
#define PROC_BIN_PROC "/bin/proc"
#define PROC_BIN_HELLO "/bin/hello"
#define PROC_BIN_DISPLAYD "/bin/displayd"
#define PROC_BIN_DEMO "/bin/demo"
#define PROC_BIN_WM "/bin/wm"
#define PROC_BIN_SESSIOND "/bin/sessiond"

/* freestanding 内存原语（src/ng/mem.c）。 */
void *memcpy(void *dst, const void *src, u64 n);
void *memset(void *dst, int c, u64 n);

/* 等待父进程被唤醒时用的"足够远"的 sleep（与 IPC_FOREVER_SLEEP 同量级）。 */
#define PROC_WAIT_SLEEP (1ULL << 34)

static struct proc_table g_procs;
static int g_proc_ready;

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static u32 proc_strlen(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static void proc_strcopy(char *dst, const char *src, u32 cap)
{
    u32 i = 0;

    if (dst == NULL || cap == 0) {
        return;
    }
    while (src != NULL && src[i] != '\0' && i + 1u < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* 往"目标进程的用户 VA"写字节：走它的页表找到物理页，经恒等映射写。 */
static int as_user_write(struct user_as *as, u64 va, const void *src, u64 len)
{
    const u8 *s = (const u8 *)src;
    u64 done = 0;

    if (as == NULL || src == NULL) {
        return PROC_EINVAL;
    }
    while (done < len) {
        u64 cur = va + done;
        u64 page = cur & ~0xFFFULL;
        u64 off = cur - page;
        u64 chunk = 0x1000ULL - off;
        u64 pte;
        u8 *dst;

        if (chunk > len - done) {
            chunk = len - done;
        }
        pte = as_walk(as, page);
        if ((pte & PG_PRESENT) == 0) {
            return PROC_EFAULT;
        }
        dst = (u8 *)(usize)(pte & PG_ADDR_MASK);
        memcpy(dst + off, s + done, chunk);
        done += chunk;
    }
    return PROC_OK;
}

/* ------------------------------------------------------------------------- */
/* ELF 加载环境（PMM 页 + 目标 user_as）                                      */
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
/* 用户栈：4 页 RW+NX + 一页标记页（固定 VA，AS 隔离证据）                    */
/* ------------------------------------------------------------------------- */

static int proc_map_stack(struct user_as *as, struct proc *p)
{
    u32 i;
    u64 flags = PG_USER | PG_WRITE;

    if (pg_nx_on()) {
        flags |= PG_NX;
    }
    for (i = 0; i < PROC_STACK_PAGES; i++) {
        u64 phys = pmm_alloc_page();

        if (phys == 0) {
            return PROC_ENOMEM;
        }
        memset((void *)(usize)phys, 0, 0x1000);
        if (as_map(as, NG_USER_STACK_BASE + (u64)i * 0x1000ULL, phys, flags) !=
            AS_OK) {
            (void)pmm_free_page(phys);
            return PROC_ENOMEM;
        }
    }
    {
        u64 phys = pmm_alloc_page();

        if (phys == 0) {
            return PROC_ENOMEM;
        }
        memset((void *)(usize)phys, 0, 0x1000);
        if (as_map(as, PROC_MARKER_VA, phys, flags) != AS_OK) {
            (void)pmm_free_page(phys);
            return PROC_ENOMEM;
        }
        p->marker_phys = (u32)phys;
    }
    p->stack_lo = NG_USER_STACK_BASE;
    p->stack_hi = NG_USER_STACK_TOP;
    return PROC_OK;
}

/*
 * 在用户栈顶部构造 argv：字符串从高往低放，然后是 char* 数组 + NULL。
 * 返回初始 RSP（16 字节对齐）与 argc。
 */
static int proc_build_argv(struct user_as *as, const char *const *argv,
                           u64 *out_rsp, u32 *out_argc)
{
    u64 sp = NG_USER_STACK_TOP - 16ULL;
    u64 ptrs[PROC_MAX_ARGS + 1];
    u32 argc = 0;
    u32 i;
    u32 total = 0;

    while (argv != NULL && argv[argc] != NULL && argc < PROC_MAX_ARGS) {
        u32 n = proc_strlen(argv[argc]);

        if (n >= PROC_ARG_MAX) {
            return PROC_EINVAL;
        }
        total += n + 1u;
        argc++;
    }
    if (total > 1024u) {
        return PROC_EINVAL;
    }
    for (i = 0; i < argc; i++) {
        u32 n = proc_strlen(argv[i]) + 1u;

        sp -= (u64)n;
        if (as_user_write(as, sp, argv[i], (u64)n) != PROC_OK) {
            return PROC_EFAULT;
        }
        ptrs[i] = sp;
    }
    sp -= (u64)(argc + 1u) * 8ULL;
    sp &= ~0xFULL;
    ptrs[argc] = 0;
    if (as_user_write(as, sp, ptrs, (u64)(argc + 1u) * 8ULL) != PROC_OK) {
        return PROC_EFAULT;
    }
    *out_rsp = sp;
    *out_argc = argc;
    return PROC_OK;
}

/* ------------------------------------------------------------------------- */
/* 进程生命周期                                                               */
/* ------------------------------------------------------------------------- */

static void proc_release_handles(struct proc *p)
{
    u32 i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < HANDLE_MAX; i++) {
        struct handle_slot *s = &p->handles.slots[i];
        u32 type = s->type;
        void *obj = s->obj;
        u32 handle = handle_make(s->gen, i);

        if (type == HANDLE_TYPE_NONE) {
            continue;
        }
        if (type == HANDLE_TYPE_PORT && obj != NULL) {
            struct ipc_port *port = (struct ipc_port *)obj;
            u32 ticket = ipc_core_port_ticket(port);

            if (ticket != 0) {
                (void)ipc_core_close(ipc_core_global(), ticket, IPC_UID_ROOT);
            }
        }
        (void)handle_close(&p->handles, handle, NULL, NULL);
    }
}

static void proc_wake_parent(u32 parent_pid)
{
    struct proc *pp;

    if (parent_pid == 0) {
        return;
    }
    pp = proc_find(&g_procs, parent_pid);
    if (pp != NULL && pp->tid != 0) {
        (void)thread_wake(pp->tid);
    }
}

static void proc_thread_main(void *arg);

/*
 * 当前进程线程的收尾：保存统计、还句柄/端口、销毁 AS、重挂孩子、置僵尸、
 * 唤醒父进程、退出线程（不返回）。AS 销毁后 CR3 已回内核表，线程继续在自己的
 * 内核栈上跑到 thread_exit()。
 */
static void proc_exit_self(struct proc *p, int status,
                           const struct ring3_result *res)
    __attribute__((noreturn));

static void proc_exit_self(struct proc *p, int status,
                           const struct ring3_result *res)
{
    thread_t *self = thread_self();
    u32 parent_pid = 0;

    if (p == NULL || p->state == PROC_STATE_ZOMBIE) {
        thread_exit();
    }
    if (res != NULL) {
        p->syscalls = res->syscalls;
        p->writes = res->writes;
        p->fs_calls = res->fs_calls;
        p->exit_code = res->exit_code;
        p->user_rsp_ok = (res->user_rsp_ok != 0) ? 1u : 0u;
        p->bad_writes = res->bad_writes;
    }
    proc_release_handles(p);
    /*
     * ng WP3-1：共享内存页和 LFB 页不属于本进程的私有页，必须**先**从页表
     * 摘掉再 as_destroy（as_destroy 会 free 每个 present 叶子页）。
     */
    shm_release_proc(p);
    fbmap_release_proc(p);
    if (p->as != NULL) {
        struct user_as *as = (struct user_as *)p->as;

        as_destroy(as);
        (void)kfree(as);
        p->as = NULL;
    }
    (void)proc_reparent_children(&g_procs, p->pid, 1);
    (void)proc_mark_exit(&g_procs, p->pid, PROC_EXIT_NORMAL, status,
                         &parent_pid);
    if (self != NULL) {
        self->proc = NULL;
    }
    proc_wake_parent(parent_pid);
    thread_exit();
}

static void proc_thread_main(void *arg)
{
    struct proc *p = (struct proc *)arg;
    struct ring3_result res;
    int rc;

    if (p == NULL) {
        thread_exit();
    }
    p->state = PROC_STATE_RUNNING;
    (void)syscall_kernel_stack_sync();
    if (p->exit_kind == PROC_EXIT_KILLED) {
        proc_exit_self(p, p->exit_status, NULL);
    }
    rc = syscall_run_user_args(p->entry, p->user_rsp, p->stack_lo, p->stack_hi,
                               (u64)p->argc, p->argv_va, &res);
    if (rc != 0) {
        /* 没走 syscall 1 就回到内核（异常/入口非法）：按失败退出。 */
        proc_exit_self(p, 255, &res);
    }
    proc_exit_self(p, (int)(res.exit_code & 0xFFULL), &res);
}

/* ------------------------------------------------------------------------- */
/* init / spawn                                                              */
/* ------------------------------------------------------------------------- */

static int proc_seed_bins(void)
{
    const u8 *blobs[7];
    u64 sizes[7];
    const char *paths[7];
    struct vfs_ctx *ctx = fs_sys_ctx();
    u32 i;

    blobs[0] = ng_shell_elf_start;
    sizes[0] = (u64)(usize)ng_shell_elf_end - (u64)(usize)ng_shell_elf_start;
    paths[0] = PROC_BIN_SHELL;
    blobs[1] = ng_proc_elf_start;
    sizes[1] = (u64)(usize)ng_proc_elf_end - (u64)(usize)ng_proc_elf_start;
    paths[1] = PROC_BIN_PROC;
    blobs[2] = ng_user_elf_start;
    sizes[2] = (u64)(usize)ng_user_elf_end - (u64)(usize)ng_user_elf_start;
    paths[2] = PROC_BIN_HELLO;
    blobs[3] = ng_displayd_elf_start;
    sizes[3] =
        (u64)(usize)ng_displayd_elf_end - (u64)(usize)ng_displayd_elf_start;
    paths[3] = PROC_BIN_DISPLAYD;
    blobs[4] = ng_demo_elf_start;
    sizes[4] = (u64)(usize)ng_demo_elf_end - (u64)(usize)ng_demo_elf_start;
    paths[4] = PROC_BIN_DEMO;
    blobs[5] = ng_wm_elf_start;
    sizes[5] = (u64)(usize)ng_wm_elf_end - (u64)(usize)ng_wm_elf_start;
    paths[5] = PROC_BIN_WM;
    blobs[6] = ng_sessiond_elf_start;
    sizes[6] = (u64)(usize)ng_sessiond_elf_end - (u64)(usize)ng_sessiond_elf_start;
    paths[6] = PROC_BIN_SESSIOND;

    /* /bin 目录（ramfs 根只有 /tmp，WP2-1 种的）。 */
    if (ctx == NULL) {
        return 1;
    }
    {
        int rc = vfs_mkdir(ctx, "/bin", 0755);

        if (rc != VFS_OK && rc != VFS_EEXIST) {
            serial_puts("ng: proc bin mkdir rc=");
            serial_putdec((u64)(-rc));
            serial_puts("\r\n");
            return 1;
        }
    }
    for (i = 0; i < 7; i++) {
        int rc = fs_sys_seed_file(paths[i], blobs[i], sizes[i]);

        if (rc != VFS_OK) {
            serial_puts("ng: proc bin seed rc=");
            serial_putdec((u64)(rc < 0 ? -rc : 0));
            serial_puts(" path=");
            serial_puts(paths[i]);
            serial_puts(" size=");
            serial_putdec(sizes[i]);
            serial_puts("\r\n");
            return (int)i + 1; /* 1-based 失败下标 */
        }
    }
    return 0;
}

void proc_init(void)
{
    struct proc *init;
    thread_t *boot;
    int seed;

    proc_table_init(&g_procs);
    init = proc_alloc(&g_procs, 0, 0);
    if (init == NULL) {
        serial_puts("ng: proc init FAIL\r\n");
        return;
    }
    init->state = PROC_STATE_RUNNING;
    init->flags = PROC_F_INIT;
    proc_strcopy(init->path, "<init>", PROC_PATH_MAX);
    boot = thread_self();
    if (boot != NULL) {
        init->tid = boot->tid;
        boot->proc = init;
    }
    g_proc_ready = 1;

    /* 把内嵌的用户 ELF 种进 ramfs（spawn 从 VFS 读，ramfs/FAT16 同一条路径）。 */
    seed = proc_seed_bins();
    if (seed != 0) {
        serial_puts("ng: proc bin FAIL (idx=");
        serial_putdec((u64)seed);
        serial_puts(")\r\n");
        return;
    }
    serial_puts("ng: proc bin ok (shell=");
    serial_puts(PROC_BIN_SHELL);
    serial_puts(" proc=");
    serial_puts(PROC_BIN_PROC);
    serial_puts(" hello=");
    serial_puts(PROC_BIN_HELLO);
    serial_puts(")\r\n");
    serial_puts("ng: desktop bin ok (displayd=");
    serial_puts(PROC_BIN_DISPLAYD);
    serial_puts(" demo=");
    serial_puts(PROC_BIN_DEMO);
    serial_puts(" wm=");
    serial_puts(PROC_BIN_WM);
    serial_puts(" sessiond=");
    serial_puts(PROC_BIN_SESSIOND);
    serial_puts(")\r\n");

    serial_puts("ng: proc table ok (max=");
    serial_putdec(PROC_MAX);
    serial_puts(" init=pid ");
    serial_putdec(init->pid);
    serial_puts(" tid=");
    serial_putdec(init->tid);
    serial_puts(")\r\n");
}

struct proc *proc_current(void)
{
    thread_t *t = thread_self();

    if (t == NULL || t->proc == NULL) {
        return NULL;
    }
    return (struct proc *)t->proc;
}

u32 proc_self_pid(void)
{
    struct proc *p = proc_current();

    return (p != NULL) ? p->pid : 0;
}

int proc_port_install(struct proc *p, u32 ticket, void *port)
{
    u32 handle = 0;

    (void)ticket; /* ticket 是端口的全局身份；表内句柄由 handle_alloc 生成 */
    if (p == NULL || port == NULL) {
        return PROC_EINVAL;
    }
    if (handle_alloc(&p->handles, HANDLE_TYPE_PORT, port, &handle) !=
        HANDLE_OK) {
        return PROC_ENOSPC;
    }
    return PROC_OK;
}

int proc_port_release(struct proc *p, void *port)
{
    u32 handle = 0;

    if (p == NULL || port == NULL) {
        return PROC_EINVAL;
    }
    if (handle_find_obj(&p->handles, HANDLE_TYPE_PORT, port, &handle) !=
        HANDLE_OK) {
        return PROC_ENOENT;
    }
    (void)handle_close(&p->handles, handle, NULL, NULL);
    return PROC_OK;
}

u32 proc_handle_live(const struct proc *p)
{
    return (p == NULL) ? 0 : handle_live(&p->handles);
}

int proc_spawn(const char *path, const char *const *argv, u32 *out_pid)
{
    struct proc *parent;
    struct proc *p = NULL;
    struct user_as *as = NULL;
    struct elf64_image plan;
    struct elf64_load_info info;
    struct elf_env env;
    struct thread *t;
    u8 *blob = NULL;
    u64 size = 0;
    u32 pid = 0;
    u32 thread_handle = 0;
    int rc;

    if (out_pid != NULL) {
        *out_pid = 0;
    }
    if (!g_proc_ready) {
        return PROC_EINVAL;
    }
    if (path == NULL || path[0] == '\0') {
        return PROC_EINVAL;
    }
    parent = proc_current();

    /* 1. 从 VFS 读 ELF（ramfs/FAT16 同一条路径）。 */
    blob = (u8 *)kmalloc((u64)PROC_ELF_MAX);
    if (blob == NULL) {
        return PROC_ENOMEM;
    }
    rc = fs_sys_read_file(path, blob, (u64)PROC_ELF_MAX, &size);
    if (rc != VFS_OK) {
        (void)kfree(blob);
        return (rc < 0) ? rc : PROC_EIO;
    }
    if (size == 0 || size > (u64)PROC_ELF_MAX) {
        (void)kfree(blob);
        return PROC_EINVAL;
    }
    rc = elf64_check(blob, size, NG_USER_VA_MIN, NG_USER_VA_MAX, &plan);
    if (rc != ELF_OK) {
        (void)kfree(blob);
        return PROC_EINVAL;
    }

    /* 2. 进程槽（pid 单调递增）。 */
    p = proc_alloc(&g_procs, (parent != NULL) ? parent->pid : 0,
                   (parent != NULL) ? parent->uid : 0);
    if (p == NULL) {
        (void)kfree(blob);
        return PROC_ENOSPC;
    }
    pid = p->pid;
    proc_strcopy(p->path, path, PROC_PATH_MAX);
    if (parent != NULL) {
        proc_strcopy(p->cwd, parent->cwd, PROC_CWD_MAX);
    }
    p->entry = plan.entry;
    p->load_base = plan.base;
    p->load_end = plan.end;

    /* 3. 新地址空间 + ELF 段。 */
    as = (struct user_as *)kmalloc((u64)sizeof(struct user_as));
    if (as == NULL) {
        rc = PROC_ENOMEM;
        goto fail;
    }
    if (as_create(as) != AS_OK) {
        (void)kfree(as);
        as = NULL;
        rc = PROC_ENOMEM;
        goto fail;
    }
    p->as = as;
    env.ctx = as;
    env.alloc_page = k_alloc_page;
    env.free_page = k_free_page;
    env.page_ptr = k_page_ptr;
    env.map_page = k_map_page;
    env.unmap_page = k_unmap_page;
    rc = elf64_load(blob, size, &env, NG_USER_VA_MIN, NG_USER_VA_MAX, &plan,
                    &info);
    if (rc != ELF_OK) {
        rc = PROC_EINVAL;
        goto fail;
    }
    p->load_base = info.base;
    p->load_end = info.end;
    p->load_pages = info.pages;
    p->load_segs = info.segs;
    p->entry = info.entry;
    (void)kfree(blob);
    blob = NULL;

    /* 4. 用户栈 + 标记页 + argv。 */
    rc = proc_map_stack(as, p);
    if (rc != PROC_OK) {
        goto fail;
    }
    {
        u32 argc = 0;

        rc = proc_build_argv(as, argv, &p->user_rsp, &argc);
        if (rc != PROC_OK) {
            goto fail;
        }
        /* 初始 RSP 指向 char* 数组本身：rdi=argc、rsi=argv。 */
        p->argc = argc;
        p->argv_va = p->user_rsp;
    }

    /* 5. 主线程 + 专用 SYSCALL 栈。 */
    t = thread_create(proc_thread_main, p);
    if (t == NULL) {
        rc = PROC_ENOMEM;
        goto fail;
    }
    /* 先登记线程身份，失败路径才杀得掉它（它还只是 READY，没跑过）。 */
    t->pml4 = as->pml4;
    t->proc = p;
    p->tid = t->tid;
    p->as_pml4 = (u32)as->pml4_phys;
    {
        void *sys = kmalloc(PROC_SYSCALL_STACK);

        if (sys == NULL) {
            rc = PROC_ENOMEM;
            goto fail;
        }
        t->syscall_stack_base = sys;
        t->syscall_stack_size = PROC_SYSCALL_STACK;
        t->syscall_top = (void *)(usize)((u64)(usize)sys + PROC_SYSCALL_STACK);
    }
    if (handle_alloc(&p->handles, HANDLE_TYPE_THREAD, (void *)t,
                     &thread_handle) != HANDLE_OK) {
        rc = PROC_ENOSPC;
        goto fail;
    }

    if (out_pid != NULL) {
        *out_pid = pid;
    }
    return PROC_OK;

fail:
    /*
     * 失败回收：AS（含已映射的 ELF 页/栈页）、进程槽、ELF 缓冲。线程可能已经
     * 建好但还没被调度过，直接杀掉（它永远不该跑）。
     */
    if (p != NULL) {
        if (p->tid != 0) {
            (void)sched_kill_thread(p->tid);
        }
        proc_release_handles(p);
        if (p->as != NULL) {
            as_destroy((struct user_as *)p->as);
            (void)kfree(p->as);
            p->as = NULL;
        }
        (void)proc_mark_exit(&g_procs, p->pid, PROC_EXIT_KILLED,
                             PROC_STATUS_KILLED(9), NULL);
        (void)proc_reap(&g_procs, p->pid);
    }
    if (blob != NULL) {
        (void)kfree(blob);
    }
    return rc;
}

/* ------------------------------------------------------------------------- */
/* wait / kill                                                               */
/* ------------------------------------------------------------------------- */

int proc_wait_pid(u32 pid, int *status, struct proc **out_proc)
{
    struct proc *cur = proc_current();
    struct proc *p;

    if (status != NULL) {
        *status = 0;
    }
    if (out_proc != NULL) {
        *out_proc = NULL;
    }
    if (!g_proc_ready) {
        return PROC_EINVAL;
    }
    p = proc_find(&g_procs, pid);
    if (p == NULL) {
        return PROC_ECHILD;
    }
    if (cur == NULL) {
        return PROC_EPERM; /* 内核上下文不能阻塞 wait */
    }
    if (p->ppid != cur->pid) {
        return PROC_ECHILD; /* 只能等自己的（直接）子进程 */
    }
    g_procs.wait_calls++;
    p->wait_tid = (thread_self() != NULL) ? thread_self()->tid : 0;
    cur->state = PROC_STATE_BLOCKED;
    while (p->state != PROC_STATE_ZOMBIE) {
        (void)thread_sleep(PROC_WAIT_SLEEP);
    }
    cur->state = PROC_STATE_RUNNING;
    p->wait_count++;
    if (status != NULL) {
        *status = p->exit_status;
    }
    if (out_proc != NULL) {
        *out_proc = p;
    }
    return PROC_OK;
}

int proc_reap_pid(u32 pid)
{
    struct proc *p = proc_find(&g_procs, pid);

    if (p == NULL) {
        return PROC_ESRCH;
    }
    if (p->state != PROC_STATE_ZOMBIE) {
        return PROC_EBUSY;
    }
    p->flags |= PROC_F_WAITED;
    return proc_reap(&g_procs, pid);
}

int proc_kill(u32 pid, u32 sig)
{
    struct proc *cur = proc_current();
    struct proc *p;
    u32 parent_pid = 0;

    if (!g_proc_ready) {
        return PROC_EINVAL;
    }
    p = proc_find(&g_procs, pid);
    if (p == NULL) {
        return PROC_ESRCH;
    }
    if (p->state == PROC_STATE_ZOMBIE) {
        return PROC_OK; /* 已经退出，等父进程 wait；kill 幂等 */
    }
    if (cur != NULL && p->uid != cur->uid && cur->uid != 0) {
        return PROC_EPERM;
    }
    if (p == cur) {
        /* 自杀：走正常退出路径（状态 = 0x100|sig），不返回。 */
        proc_exit_self(p, PROC_STATUS_KILLED(sig & 0xFFu), NULL);
    }

    /* 1. 先把目标线程从 IPC 等待链上摘掉（否则端口的"已通知"位会吞唤醒）。 */
    (void)ipc_core_cancel_thread(ipc_core_global(), p->tid);
    /* 2. 摘线程：READY/BLOCKED -> DEAD（栈由别的线程下一次调度回收）。 */
    if (p->tid != 0) {
        (void)sched_kill_thread(p->tid);
    }
    /* 3. 关它自己的端口/句柄、销毁 AS（先摘共享页/LFB，再销毁）。 */
    proc_release_handles(p);
    shm_release_proc(p);
    fbmap_release_proc(p);
    if (p->as != NULL) {
        as_destroy((struct user_as *)p->as);
        (void)kfree(p->as);
        p->as = NULL;
    }
    /* 4. 状态迁移 + 唤醒父进程。 */
    (void)proc_kill_mark(&g_procs, pid, sig, &parent_pid);
    proc_wake_parent(parent_pid);
    return PROC_OK;
}

u32 proc_getpid(void)
{
    struct proc *p = proc_current();

    return (p != NULL) ? p->pid : 0;
}

u32 proc_getppid(void)
{
    struct proc *p = proc_current();

    return (p != NULL) ? p->ppid : 0;
}

const struct proc *proc_slot(u32 pid)
{
    return proc_find(&g_procs, pid);
}

u32 proc_table_live(void)
{
    return proc_live(&g_procs);
}

u64 proc_table_spawned(void)
{
    return g_procs.spawned;
}

/* ------------------------------------------------------------------------- */
/* syscall 40..44                                                            */
/* ------------------------------------------------------------------------- */

/* 用户 argv（char **）-> 内核字符串。返回 argc 或负 errno。 */
static int proc_copy_argv(u64 uargv, char argbuf[][PROC_ARG_MAX], u32 max_args,
                          const char *argv_out[PROC_MAX_ARGS + 1])
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < PROC_MAX_ARGS; i++) {
        argv_out[i] = NULL;
    }
    if (uargv == 0) {
        return 0;
    }
    if (uaccess_range_ok(uargv, 8, 0) == 0) {
        return PROC_EFAULT;
    }
    for (i = 0; i < max_args; i++) {
        u64 up = 0;
        int len;

        if (copyin(&up, uargv + (u64)i * 8ULL, 8) != UACCESS_OK) {
            return PROC_EFAULT;
        }
        if (up == 0) {
            break;
        }
        len = copyin_str(argbuf[i], up, PROC_ARG_MAX);
        if (len < 0) {
            return PROC_EFAULT;
        }
        if (!proc_user_str_len_ok(len, PROC_ARG_MAX)) {
            return PROC_ENAMETOOLONG;
        }
        argv_out[i] = argbuf[i];
        n++;
    }
    return (int)n;
}

u64 proc_sys_spawn(u64 upath, u64 uargv, u64 upid)
{
    char path[PROC_PATH_MAX];
    char argbuf[PROC_MAX_ARGS][PROC_ARG_MAX];
    const char *argv[PROC_MAX_ARGS + 1];
    u32 pid = 0;
    int argc;
    int rc;
    int len;

    if (upath == 0 || upid == 0) {
        return (u64)(i64)PROC_EFAULT;
    }
    len = copyin_str(path, upath, PROC_PATH_MAX);
    if (len < 0) {
        return (u64)(i64)PROC_EFAULT;
    }
    if (!proc_user_str_len_ok(len, PROC_PATH_MAX)) {
        return (u64)(i64)PROC_ENAMETOOLONG;
    }
    argc = proc_copy_argv(uargv, argbuf, PROC_MAX_ARGS, argv);
    if (argc < 0) {
        return (u64)(i64)argc;
    }
    rc = proc_spawn(path, argv, &pid);
    if (rc != PROC_OK) {
        return (u64)(i64)rc;
    }
    if (copyout(upid, &pid, 4) != UACCESS_OK) {
        return (u64)(i64)PROC_EFAULT;
    }
    return 0;
}

u64 proc_sys_wait(u64 pid, u64 ustatus)
{
    int status = 0;
    int rc;

    if (pid == 0 || pid > 0xFFFFFFFFULL) {
        return (u64)(i64)PROC_EINVAL;
    }
    if (ustatus != 0 && uaccess_range_ok(ustatus, 4, 1) == 0) {
        return (u64)(i64)PROC_EFAULT;
    }
    rc = proc_wait_pid((u32)pid, &status, NULL);
    if (rc != PROC_OK) {
        return (u64)(i64)rc;
    }
    if (ustatus != 0 && copyout(ustatus, &status, 4) != UACCESS_OK) {
        return (u64)(i64)PROC_EFAULT;
    }
    (void)proc_reap_pid((u32)pid);
    return 0;
}

u64 proc_sys_getpid(void)
{
    return (u64)proc_getpid();
}

u64 proc_sys_kill(u64 pid, u64 sig)
{
    int rc;

    if (pid == 0 || pid > 0xFFFFFFFFULL || sig > 0xFFULL) {
        return (u64)(i64)PROC_EINVAL;
    }
    rc = proc_kill((u32)pid, (u32)sig);
    return (u64)(i64)rc;
}

u64 proc_sys_getppid(void)
{
    return (u64)proc_getppid();
}
