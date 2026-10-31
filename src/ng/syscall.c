/*
 * ng WP1-3/1-6/1-7: syscall（STAR/LSTAR/FMASK + 调用号分发）。
 *
 * 调用约定（ABI 1.0 草案，WP4-1 冻结）：
 *   rax = 调用号，rdi/rsi/rdx/r10 = 参数，返回值放 rax（负 = 错误码）。
 *     0  write：rdi=用户缓冲区、rsi=长度 -> 拷进内核后写串口（返回长度）；
 *                rdi=0 时保留 WP1-3 的"write serial 诊断行"语义（ring0/ring3
 *                往返自检用它）；
 *     1  exit：ring3 下置退出标志、记录 rdi 退出码并跳回内核恢复点；
 *     10..14 IPC（WP1-6）。
 *   ABI 1.1（WP5-0a）：48..95 是 1.1 新号段（已冻结、未实现）；未分配号与
 *     未实现号在 default 统一返回 -ENOSYS(-38)，不再返回 -1(EPERM)。
 *
 * 入口/返回路径在 isr.S：
 *   - SYSCALL 只改 CS/SS，不换栈 => 入口把用户 RSP 存到全局，再从
 *     `syscall_kernel_rsp0` 装载**当前线程**的内核栈（WP1-7：调度器在每次
 *     switch_context 前用 syscall_set_thread_stack() 同步它和 TSS.RSP0）；
 *   - ring3 返回用 SYSRETQ（STAR 高位 0x13 => CS=0x23, SS=0x1b）；
 *   - ring0 往返测试用 IRETQ（SYSRET 架构上只能回 CPL3）。
 *
 * WP1-7：ring3 执行统一走 syscall_run_user()：置 ring3 状态、iretq 进 CPL3、
 * syscall 1 时由入口跳回 kernel_resume（不依赖用户态再执行）。WP1-3 的静态
 * 用户页往返和 WP1-7 的 ELF64 加载程序都用这一个入口。
 */
#include "syscall.h"

#include "fbmap.h"
#include "fs_syscall.h"
#include "gdt.h"
#include "input.h"
#include "io.h"
#include "ipc.h"
#include "paging.h"
#include "proc.h"
#include "serial.h"
#include "shm.h"
#include "svc_syscall.h"
#include "thread.h"
#include "uaccess.h"
#include "wp23.h"

#define MSR_EFER 0xC0000080u
#define MSR_STAR 0xC0000081u
#define MSR_LSTAR 0xC0000082u
#define MSR_FMASK 0xC0000084u

#define EFER_SCE (1ULL << 0)

/*
 * STAR：高 16 位 = 0x13（用户段基址，带 RPL3），低 32 位高 16 位 = 0x08。
 *   SYSCALL: CS = 0x08, SS = 0x10（内核代码/数据）
 *   SYSRET : CS = 0x13 + 16 = 0x23（用户代码），SS = 0x13 + 8 = 0x1b（用户数据）
 */
#define STAR_VALUE ((0x13ULL << 48) | (0x08ULL << 32))

/* 清 TF/IF/DF/AC：入口不在中断窗口里跑，AC=0 满足 SMAP 约定。 */
#define FMASK_VALUE ((1ULL << 8) | (1ULL << 9) | (1ULL << 10) | (1ULL << 18))

#define USER_CODE_VA 0x40000000ULL
#define USER_STACK_VA 0x40100000ULL
#define USER_PAGE_SIZE 4096ULL

/* syscall 0 的用户缓冲区上限（内核栈上的一份小缓冲）。 */
#define USER_WRITE_MAX 256ULL

/* 与 isr.S 的 push 顺序一致（没有 vector/error 两个槽）。 */
struct syscall_frame {
    u64 rax;
    u64 rcx;
    u64 rdx;
    u64 rbx;
    u64 rbp;
    u64 rsi;
    u64 rdi;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
};

/* ---- 汇编侧符号 ----------------------------------------------------------- */
extern char syscall_entry[];
extern char syscall_stack_top[];
extern void syscall_ring0_stub(void);
extern void ring3_enter(u64 user_rip, u64 user_rsp);
extern void ring3_enter_args(u64 user_rip, u64 user_rsp, u64 argc, u64 argv);
extern char user_prog_start[];
extern char user_prog_end[];

/* ---- 汇编入口读写的全局 --------------------------------------------------- */
u64 syscall_user_rsp;
u64 syscall_saved_rip;
u64 syscall_saved_flags;
u64 syscall_return_mode; /* 0 = SYSRETQ（ring3），1 = IRETQ（ring0 测试） */
u64 syscall_want_exit;   /* ring3 的 syscall 1：回内核而不是回用户态 */
/* WP1-7：SYSCALL 入口装载的内核栈顶（== TSS.RSP0，随线程切换更新）。 */
u64 syscall_kernel_rsp0;
/* WP3-0：SYSCALL 入口的 CS（0x23 = CPL3）。只在入口窗口有效。 */
u64 syscall_entry_cs;

u64 kernel_resume_rsp;
u64 kernel_resume_rip;

/*
 * WP3-0：per-thread 执行上下文。没有 TCB 时（WP1-3 的 ring0/ring3 自检发生在
 * sched_init 之前、host 单测直接调 dispatch）回退到一份静态上下文。
 */
static struct thread_uctx g_boot_uctx;

static struct thread_uctx *uctx_cur(void)
{
    thread_t *t = thread_self();

    return (t != NULL) ? &t->uctx : &g_boot_uctx;
}

/* ---- 状态 ----------------------------------------------------------------- */
static volatile u64 syscall_count;

static u8 user_code_page[USER_PAGE_SIZE] __attribute__((aligned(4096)));
static u8 user_stack[USER_PAGE_SIZE] __attribute__((aligned(4096)));

/* ------------------------------------------------------------------------- */
/* 初始化                                                                     */
/* ------------------------------------------------------------------------- */

static void wrmsr_checked(u32 msr, u64 value, const char *what)
{
    wrmsr(msr, value);
    if (rdmsr(msr) != value) {
        serial_puts("ng: CHECK FAIL msr ");
        serial_puts(what);
        serial_puts("\r\n");
    }
}

void syscall_init(void)
{
    u64 efer;

    efer = rdmsr(MSR_EFER);
    wrmsr_checked(MSR_EFER, efer | EFER_SCE, "efer.sce");
    wrmsr_checked(MSR_STAR, STAR_VALUE, "star");
    wrmsr_checked(MSR_LSTAR, (u64)(usize)syscall_entry, "lstar");
    wrmsr_checked(MSR_FMASK, FMASK_VALUE, "fmask");

    /*
     * 引导线程的内核栈是 isr.S 的静态栈（sched_init 之前还没有 TCB）。
     * 这里把 syscall 入口用的栈顶和 TSS.RSP0 一起对齐给它；之后每次
     * 上下文切换由 sched_arch.c 调 syscall_set_thread_stack() 更新。
     */
    syscall_set_kernel_stack((u64)(usize)syscall_stack_top);

    serial_puts("ng: syscall msr star=");
    serial_puthex(rdmsr(MSR_STAR));
    serial_puts(" lstar=");
    serial_puthex(rdmsr(MSR_LSTAR));
    serial_puts(" fmask=");
    serial_puthex(rdmsr(MSR_FMASK));
    serial_puts(" efer.sce=");
    serial_putdec(rdmsr(MSR_EFER) & EFER_SCE);
    serial_puts("\r\n");
}

/* ------------------------------------------------------------------------- */
/* per-thread 内核栈（syscall 入口 + TSS.RSP0 同步）                          */
/* ------------------------------------------------------------------------- */

static u64 stack_top_of(const struct thread *t)
{
    if (t != NULL && t->syscall_top != NULL) {
        return (u64)(usize)t->syscall_top;
    }
    if (t != NULL && t->stack_top != NULL) {
        return (u64)(usize)t->stack_top;
    }
    /* boot 线程（sched_init 的 0 号槽）没有 TCB 栈，用 isr.S 的静态栈。 */
    return (u64)(usize)syscall_stack_top;
}

void syscall_set_thread_stack(struct thread *t)
{
    syscall_set_kernel_stack(stack_top_of(t));
}

u64 syscall_kernel_stack_sync(void)
{
    syscall_set_thread_stack(thread_self());
    return syscall_kernel_rsp0;
}

/* ------------------------------------------------------------------------- */
/* 分发（isr.S 调用）                                                         */
/* ------------------------------------------------------------------------- */

static int syscall_user_write(struct thread_uctx *u, u64 uva, u64 len)
{
    char buf[USER_WRITE_MAX];
    u64 i;
    int rc;

    if (len == 0 || len > USER_WRITE_MAX) {
        return UACCESS_EINVAL;
    }
    rc = copyin(buf, uva, len);
    if (rc != UACCESS_OK) {
        return UACCESS_EFAULT;
    }
    for (i = 0; i < len; i++) {
        serial_putc(buf[i]);
    }
    u->writes++;
    u->bytes += len;
    return (int)len;
}

/*
 * syscall 入口的"收集"：汇编在换栈之前把用户 RSP/RCX/R11/CS 写进全局，这里
 * 一次性把它们存进当前线程的上下文。入口到此处之间没有调度点，所以全局是
 * 可靠的；dispatch 一返回，这些状态就只存在于 TCB 里。
 */
static void syscall_enter_ctx(struct thread_uctx *u)
{
    if (syscall_user_rsp != 0) {
        u->user_rsp = syscall_user_rsp;
    }
    u->saved_rip = syscall_saved_rip;
    u->saved_flags = syscall_saved_flags;
    u->from_ring3 = ((syscall_entry_cs & 3u) == 3u || u->run_user) ? 1u : 0u;
}

/*
 * dispatch 返回前的"回写"：把本线程的状态写进汇编返回路径读的全局。这里到
 * SYSRET/IRETQ 之间同样没有调度点，所以安全。
 */
static void syscall_prepare_return(struct thread_uctx *u)
{
    syscall_user_rsp = u->user_rsp;
    syscall_saved_rip = u->saved_rip;
    syscall_saved_flags = u->saved_flags;
    syscall_return_mode = u->return_mode;
    syscall_want_exit = u->want_exit;
    kernel_resume_rsp = u->resume_rsp;
    kernel_resume_rip = u->resume_rip;
}

void syscall_dispatch(struct syscall_frame *f)
{
    struct thread_uctx *u = uctx_cur();

    syscall_count++;
    syscall_enter_ctx(u);

    if (u->from_ring3) {
        u->syscalls++;
        if (u->user_rsp >= u->stack_lo && u->user_rsp < u->stack_hi) {
            u->user_rsp_ok = 1;
        }
    }

    switch (f->rax) {
    case NG_SYS_WRITE:
        if (f->rdi != 0) {
            int rc = syscall_user_write(u, f->rdi, f->rsi);

            if (rc < 0 && u->from_ring3) {
                u->bad_writes++;
            }
            f->rax = (u64)(i64)rc;
        } else {
            /* WP1-3 的往返自检：没有用户缓冲区，只打诊断行。 */
            serial_puts(u->from_ring3 ? "ng: syscall write (ring3)\r\n"
                                      : "ng: syscall write (ring0)\r\n");
            f->rax = 0;
        }
        break;
    case NG_SYS_EXIT:
        serial_puts("ng: syscall exit test\r\n");
        f->rax = 0;
        if (u->from_ring3) {
            u->want_exit = 1;
            u->exit_code = f->rdi;
        }
        break;
    /* WP1-6：IPC / 句柄。用户指针与长度在 ipc_sys_*() 里校验。 */
    case NG_SYS_PORT_CREATE:
        f->rax = ipc_sys_create(f->rdi);
        break;
    case NG_SYS_PORT_SEND:
        f->rax = ipc_sys_send(f->rdi, f->rsi, f->rdx, f->r10);
        break;
    case NG_SYS_PORT_RECV:
        f->rax = ipc_sys_recv(f->rdi, f->rsi, f->rdx, f->r10);
        break;
    case NG_SYS_PORT_REPLY:
        f->rax = ipc_sys_reply(f->rdi, f->rsi, f->rdx, f->r10);
        break;
    case NG_SYS_PORT_CLOSE:
        f->rax = ipc_sys_close(f->rdi);
        break;
    /* WP2-1：VFS/ramfs。用户指针在 fs_syscall.c 里经 uaccess 校验。 */
    case NG_SYS_OPEN:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_open(f->rdi, (u32)f->rsi, (u32)f->rdx);
        break;
    case NG_SYS_READ:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_read((u32)f->rdi, f->rsi, f->rdx);
        break;
    case NG_SYS_WRITE_FD:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_write((u32)f->rdi, f->rsi, f->rdx);
        break;
    case NG_SYS_CLOSE:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_close((u32)f->rdi);
        break;
    case NG_SYS_STAT:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_stat(f->rdi, f->rsi);
        break;
    case NG_SYS_LISTDIR:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_listdir(f->rdi, f->rsi, (u32)f->rdx);
        break;
    case NG_SYS_MKDIR:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_mkdir(f->rdi, (u32)f->rsi);
        break;
    case NG_SYS_UNLINK:
        if (u->from_ring3) {
            u->fs_calls++;
        }
        f->rax = (u64)(i64)fs_sys_unlink(f->rdi);
        break;
    /* WP2-3：非阻塞 readkey。无键返回 -EAGAIN，用户态自己决定重试/超时。 */
    case NG_SYS_READKEY:
        f->rax = ng_input_readkey();
        break;
    /* WP2-5：auth/log/pkg。用户指针在 svc_syscall.c 里经 uaccess 校验。 */
    case NG_SYS_LOGIN:
        f->rax = (u64)(i64)ng_sys_login(f->rdi, f->rsi);
        break;
    case NG_SYS_DMESG:
        f->rax = (u64)(i64)ng_sys_dmesg(f->rdi, f->rsi);
        break;
    case NG_SYS_LOG_WRITE:
        f->rax = (u64)(i64)ng_sys_log_write(f->rdi, f->rsi);
        break;
    case NG_SYS_PKG_LIST:
        f->rax = (u64)(i64)ng_sys_pkg_list(f->rdi, f->rsi);
        break;
    case NG_SYS_PKG_VERIFY:
        f->rax = (u64)(i64)ng_sys_pkg_verify(f->rdi);
        break;
    /* WP3-0：进程 / 跨进程 IPC。用户指针在 proc_sys_*() 里经 uaccess 校验。 */
    case NG_SYS_SPAWN:
        f->rax = proc_sys_spawn(f->rdi, f->rsi, f->rdx);
        break;
    case NG_SYS_WAIT:
        f->rax = proc_sys_wait(f->rdi, f->rsi);
        break;
    case NG_SYS_GETPID:
        f->rax = proc_sys_getpid();
        break;
    case NG_SYS_KILL:
        f->rax = proc_sys_kill(f->rdi, f->rsi);
        break;
    case NG_SYS_GETPPID:
        f->rax = proc_sys_getppid();
        break;
    /* WP3-1：显示/共享内存机制。用户指针在 shm.c/fbmap.c 里经 uaccess 校验。 */
    case NG_SYS_SHM_ALLOC:
        f->rax = shm_sys_alloc(f->rdi, f->rsi, f->rdx);
        break;
    case NG_SYS_SHM_MAP:
        f->rax = shm_sys_map(f->rdi, f->rsi);
        break;
    case NG_SYS_FB_MAP:
        f->rax = fbmap_sys_map(f->rdi, f->rsi);
        break;
    /*
     * WP5-0h（ABI 1.1 DRAFT）：输入服务。
     *   96 input_event(&ev)：非阻塞；用户指针在 dispatch 里 copyout 校验；
     *   97 input_claim(pid, flags)：pid=0 当前进程，flags bit0 = 释放。
     */
    case NG_SYS_INPUT_EVENT: {
        struct input_packet ev;
        int rc = ng_input_event(&ev);

        if (rc == 0 && copyout(f->rdi, &ev, (u64)sizeof(ev)) != UACCESS_OK) {
            rc = UACCESS_EFAULT;
        }
        f->rax = (u64)(i64)rc;
        break;
    }
    case NG_SYS_INPUT_CLAIM:
        f->rax = (u64)ng_input_claim(f->rdi, f->rsi);
        break;
    default:
        /*
         * ABI 1.1（WP5-0a）：未分配号（2..9、15..19、28..29、36..39、98..）与
         * 1.1 已分配但内核尚未实现的号（48..95）统一 -ENOSYS(-38)。
         * 1.0 §10 第 1 项已声明未分配号行为不属于 1.0 契约，故不破坏 1.0。
         */
        f->rax = (u64)(i64)NG_SYS_ENOSYS;
        break;
    }

    syscall_prepare_return(u);
}

/* ------------------------------------------------------------------------- */
/* ring3 执行入口（WP1-3 静态页 / WP1-7 ELF64 程序共用）                       */
/* ------------------------------------------------------------------------- */

int syscall_run_user(u64 entry, u64 user_rsp, u64 stack_lo, u64 stack_hi,
                     struct ring3_result *out)
{
    return syscall_run_user_args(entry, user_rsp, stack_lo, stack_hi, 0, 0, out);
}

int syscall_run_user_args(u64 entry, u64 user_rsp, u64 stack_lo, u64 stack_hi,
                          u64 argc, u64 argv, struct ring3_result *out)
{
    struct thread_uctx *u = uctx_cur();

    if (entry == 0 || user_rsp < stack_lo || user_rsp >= stack_hi ||
        stack_hi <= stack_lo) {
        return -1;
    }

    u->stack_lo = stack_lo;
    u->stack_hi = stack_hi;
    u->run_user = 1;
    u->from_ring3 = 1;
    u->return_mode = 0;
    u->want_exit = 0;
    u->syscalls = 0;
    u->writes = 0;
    u->bytes = 0;
    u->exit_code = 0;
    u->bad_writes = 0;
    u->fs_calls = 0;
    u->user_rsp_ok = 0;
    u->user_rsp = user_rsp;
    u->resume_rsp = 0;
    u->resume_rip = 0;
    /* 汇编返回路径在 dispatch 之前读的全局也同步一份（host 单测直接看它们）。 */
    syscall_return_mode = 0;
    syscall_want_exit = 0;

    if (argc != 0 || argv != 0) {
        ring3_enter_args(entry, user_rsp, argc, argv);
    } else {
        ring3_enter(entry, user_rsp);
    }
    /* 走到这里说明 syscall 1 把控制权还给了内核恢复点。 */
    u->run_user = 0;
    u->from_ring3 = 0;

    if (out != NULL) {
        out->syscalls = u->syscalls;
        out->writes = u->writes;
        out->bytes = u->bytes;
        out->fs_calls = u->fs_calls;
        out->exit_code = u->exit_code;
        out->exited = u->want_exit ? 1 : 0;
        out->user_rsp_ok = u->user_rsp_ok;
        out->bad_writes = u->bad_writes;
    }
    return u->want_exit ? 0 : -1;
}

/*
 * WP3-0：ring3_enter 在 iretq 之前调用，把"回到 C 的恢复点"存进当前线程的
 * 上下文（而不是全局）。多进程交替执行时，每个线程有各自的恢复点。
 */
void ring3_save_resume(u64 rsp, u64 rip)
{
    struct thread_uctx *u = uctx_cur();

    u->resume_rsp = rsp;
    u->resume_rip = rip;
}

/* ------------------------------------------------------------------------- */
/* ring0 往返                                                                 */
/* ------------------------------------------------------------------------- */

static int syscall_ring0_test(void)
{
    struct thread_uctx *u = uctx_cur();
    int fails = 0;

    u->from_ring3 = 0;
    u->run_user = 0;
    u->return_mode = 1;
    u->want_exit = 0;
    syscall_entry_cs = 0;
    syscall_count = 0;

    syscall_ring0_stub();

    if (syscall_count == 1 && u->want_exit == 0 && syscall_return_mode == 1) {
        serial_puts("ng: syscall ring0 ok\r\n");
    } else {
        fails++;
        serial_puts("ng: syscall ring0 FAIL\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* ABI 1.1（WP5-0a）：未分配号 / 未实现号 = -ENOSYS(-38)                       */
/* ------------------------------------------------------------------------- */

static int syscall_enosys_test(void)
{
    /* 1.0 未分配号 + 1.1 已分配但未实现的号：都必须落 default。 */
    static const u64 nos[] = {
        2u, 9u, 15u, 19u, 28u, 29u, 36u, 39u, 48u, 95u, 98u
    };
    struct syscall_frame f = {0};
    u64 bad = 0;
    u64 i;

    for (i = 0; i < (u64)(sizeof(nos) / sizeof(nos[0])); i++) {
        f.rax = nos[i];
        syscall_dispatch(&f);
        if (f.rax != (u64)(i64)NG_SYS_ENOSYS) {
            bad++;
        }
    }
    if (bad == 0 && (u64)(i64)NG_SYS_ENOSYS == 0xFFFFFFFFFFFFFFDAULL) {
        serial_puts("ng: syscall ENOSYS ok (-38)\r\n");
        return 0;
    }
    serial_puts("ng: syscall ENOSYS FAIL bad=");
    serial_putdec(bad);
    serial_puts("\r\n");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* ring3 静态页往返（WP1-3 回归，保持原有证据行）                              */
/* ------------------------------------------------------------------------- */

static int map_user_pages(void)
{
    u64 code_pte;
    u64 stack_pte;
    u64 n;
    u64 i;

    n = (u64)(user_prog_end - user_prog_start);
    if (n == 0 || n > USER_PAGE_SIZE) {
        return 1;
    }
    for (i = 0; i < USER_PAGE_SIZE; i++) {
        user_code_page[i] = (i < n) ? (u8)user_prog_start[i] : 0;
    }
    for (i = 0; i < USER_PAGE_SIZE; i++) {
        user_stack[i] = 0;
    }

    /* 代码页：U/S=1、只读、可执行（W=0, NX=0）。 */
    if (pg_map(USER_CODE_VA, (u64)(usize)user_code_page, PG_USER) != PG_OK) {
        return 1;
    }
    /* 栈页：U/S=1、可写、不可执行。 */
    if (pg_map(USER_STACK_VA, (u64)(usize)user_stack,
               PG_USER | PG_WRITE | PG_NX) != PG_OK) {
        return 1;
    }

    code_pte = pg_walk(USER_CODE_VA);
    stack_pte = pg_walk(USER_STACK_VA);
    if ((code_pte & (PG_PRESENT | PG_USER | PG_NX)) != (PG_PRESENT | PG_USER)) {
        return 1;
    }
    if ((stack_pte & (PG_PRESENT | PG_USER | PG_WRITE | PG_NX)) !=
        (PG_PRESENT | PG_USER | PG_WRITE | PG_NX)) {
        return 1;
    }

    serial_puts("ng: ring3 map code=");
    serial_puthex(USER_CODE_VA);
    serial_puts(" pte=");
    serial_puthex(code_pte);
    serial_puts(" stack=");
    serial_puthex(USER_STACK_VA);
    serial_puts(" pte=");
    serial_puthex(stack_pte);
    serial_puts("\r\n");
    return 0;
}

static int syscall_ring3_test(void)
{
    int fails = 0;
    struct ring3_result res;

    if (map_user_pages() != 0) {
        serial_puts("ng: ring3 map FAIL\r\n");
        return 1;
    }
    /* 当前线程（boot）的内核栈顶：per-thread RSP0 的第一份证据。 */
    (void)syscall_kernel_stack_sync();

    if (syscall_run_user(USER_CODE_VA, USER_STACK_VA + USER_PAGE_SIZE - 16,
                         USER_STACK_VA, USER_STACK_VA + USER_PAGE_SIZE,
                         &res) != 0) {
        serial_puts("ng: ring3 run FAIL\r\n");
        return 1;
    }

    serial_puts("ng: ring3 syscalls=");
    serial_putdec(res.syscalls);
    serial_puts(" user_rsp=");
    serial_putdec((u64)res.user_rsp_ok);
    serial_puts(" exit=");
    serial_putdec(res.exited);
    serial_puts("\r\n");

    if (res.exited && res.syscalls == 2 && res.user_rsp_ok) {
        serial_puts("ng: ring3 syscall ok\r\n");
    } else {
        fails++;
        serial_puts("ng: ring3 syscall FAIL\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int syscall_selftest(void)
{
    int fails = 0;

    fails += syscall_ring0_test();
    fails += syscall_enosys_test();
    fails += syscall_ring3_test();
    return fails;
}

/* ------------------------------------------------------------------------- */
/* WP1-7：内核栈同步的小出口（elf_selftest / 调度器共用）                      */
/* ------------------------------------------------------------------------- */

void syscall_set_kernel_stack(u64 top)
{
    if (top == 0) {
        return;
    }
    syscall_kernel_rsp0 = top;
    gdt_set_rsp0(top);
}
