/*
 * ng WP1-3: syscall（STAR/LSTAR/FMASK + EFER.SCE）。
 *
 * 调用约定（WP1-3 最小版，ABI 1.0 草案只定到能用）：
 *   rax = 调用号，rdi/rsi/rdx = 参数（本 WP 还没用），返回值放 rax。
 *     0 = write serial：内核串口打一行，返回 0；
 *     1 = exit test：打印并返回；ring3 下额外请求“返回内核”（不回用户态）。
 *
 * 入口/返回路径在 isr.S：
 *   - SYSCALL 只改 CS/SS，不换栈 => 入口先把用户 RSP 存全局，再切静态内核栈；
 *   - ring3 返回用 SYSRETQ（STAR 高位 0x13 => CS=0x23, SS=0x1b）；
 *   - ring0 往返测试用 IRETQ（SYSRET 架构上只能回 CPL3）。
 *
 * ring3 测试（stretch）：把 user_prog_start..user_prog_end 的机器码拷进一个
 * .bss 页，映射到 0x40000000（U/S=1, R-X），4 KiB 栈映射到 0x40100000
 * （U/S=1, RW+NX），构造 iretq 帧进 CPL3。用户程序两次 syscall 后由 handler
 * 直接跳回内核恢复点（kernel_resume_*），不依赖用户态再跑别的代码。
 */
#include "syscall.h"

#include "io.h"
#include "paging.h"
#include "serial.h"

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
extern char user_prog_start[];
extern char user_prog_end[];

/* ---- 汇编入口读写的全局 --------------------------------------------------- */
u64 syscall_user_rsp;
u64 syscall_saved_rip;
u64 syscall_saved_flags;
u64 syscall_return_mode; /* 0 = SYSRETQ（ring3），1 = IRETQ（ring0 测试） */
u64 syscall_want_exit;   /* ring3 的 syscall 1：回内核而不是回用户态 */

u64 kernel_resume_rsp;
u64 kernel_resume_rip;

/* ---- 状态 ----------------------------------------------------------------- */
static volatile u64 syscall_count;
static volatile u64 ring3_syscalls;
static volatile int syscall_from_ring3;
static volatile int ring3_user_rsp_ok;

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
/* 分发（isr.S 调用）                                                         */
/* ------------------------------------------------------------------------- */

void syscall_dispatch(struct syscall_frame *f)
{
    syscall_count++;

    if (syscall_from_ring3) {
        ring3_syscalls++;
        if (syscall_user_rsp >= USER_STACK_VA &&
            syscall_user_rsp < USER_STACK_VA + USER_PAGE_SIZE) {
            ring3_user_rsp_ok = 1;
        }
    }

    switch (f->rax) {
    case 0:
        serial_puts(syscall_from_ring3 ? "ng: syscall write (ring3)\r\n"
                                       : "ng: syscall write (ring0)\r\n");
        f->rax = 0;
        break;
    case 1:
        serial_puts("ng: syscall exit test\r\n");
        f->rax = 0;
        if (syscall_from_ring3) {
            syscall_want_exit = 1;
        }
        break;
    default:
        f->rax = (u64)-1;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* ring0 往返                                                                 */
/* ------------------------------------------------------------------------- */

static int syscall_ring0_test(void)
{
    int fails = 0;

    syscall_from_ring3 = 0;
    syscall_return_mode = 1;
    syscall_want_exit = 0;
    syscall_count = 0;

    syscall_ring0_stub();

    if (syscall_count == 1 && syscall_want_exit == 0) {
        serial_puts("ng: syscall ring0 ok\r\n");
    } else {
        fails++;
        serial_puts("ng: syscall ring0 FAIL\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* ring3 往返                                                                 */
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

    if (map_user_pages() != 0) {
        serial_puts("ng: ring3 map FAIL\r\n");
        return 1;
    }

    syscall_from_ring3 = 1;
    syscall_return_mode = 0;
    syscall_want_exit = 0;
    syscall_count = 0;
    ring3_syscalls = 0;
    ring3_user_rsp_ok = 0;

    ring3_enter(USER_CODE_VA, USER_STACK_VA + USER_PAGE_SIZE - 16);
    /* 到这里说明 syscall 1 走 kernel_resume 回到了内核。 */
    syscall_from_ring3 = 0;

    serial_puts("ng: ring3 syscalls=");
    serial_putdec(ring3_syscalls);
    serial_puts(" user_rsp=");
    serial_putdec((u64)ring3_user_rsp_ok);
    serial_puts(" exit=");
    serial_putdec(syscall_want_exit);
    serial_puts("\r\n");

    if (syscall_want_exit && ring3_syscalls == 2 && ring3_user_rsp_ok) {
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
    fails += syscall_ring3_test();
    return fails;
}
