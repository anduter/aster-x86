/*
 * ng WP1-8 host 单测：syscall.c 的调用号分发与 ring3 执行入口。
 *
 * syscall.c 不链接汇编：syscall_dispatch() 是纯 C 分发（串口/IPC/uaccess 都是
 * 回调），syscall_run_user() 的 ring3 进入点 ring3_enter() 由 fake_ng_env.c
 * 换成“执行测试注册的 hook”。hook 往 syscall_dispatch() 灌 frame，等价于
 * 用户态连发几次 syscall，所以下面这些规则在 host 上是真跑的：
 *
 *   - 0 write：rdi=0 是 ring0/ring3 诊断行；rdi!=0 走 copyin（坏指针 EFAULT、
 *     len=0 / >256 是 EINVAL、256 字节边界成功）；
 *   - 1 exit：ring0 只打行；ring3 记录退出码并让 run_user 返回 0；
 *   - 10..14：参数（含 r10 的 timeout）原样搬给 ipc_sys_*()，返回值透传；
 *   - 未知调用号 = -1；
 *   - run_user：入口/栈区间校验、per-run 统计清零、user_rsp/bad_writes 统计；
 *   - per-thread 内核栈：syscall_set_kernel_stack / set_thread_stack 同步
 *     syscall_kernel_rsp0 与 TSS.RSP0。
 */
#include <string.h>

#include "fake_ng_env.h"
#include "fs_syscall.h"
#include "ipc.h"
#include "paging.h"
#include "serial.h"
#include "syscall.h"
#include "test_util.h"
#include "thread.h"
#include "uaccess.h"
#include "vfs.h"
#include "wp23.h"

/* 与 src/ng/syscall.c 的 struct syscall_frame 字段顺序一致（isr.S push 顺序）。 */
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

extern void syscall_dispatch(struct syscall_frame *f);
extern u64 syscall_kernel_rsp0;
extern u64 syscall_user_rsp;
extern char syscall_stack_top[];

#define PG_SIZE 4096ULL
#define RING3_ENTRY 0x40000000ULL
#define RING3_STACK_LO 0x40100000ULL
#define RING3_STACK_HI 0x40104000ULL
#define RING3_STACK_TOP (RING3_STACK_HI - 16)

static u8 arena[4 * PG_SIZE] __attribute__((aligned(4096)));
static u64 ubuf;

static void reset_env(void)
{
    fake_pg_reset();
    fake_serial_reset();
    fake_ipc_reset();
    fake_gdt_reset();
    fake_ring3_set(NULL, NULL);
    memset(arena, 0, sizeof(arena));
    ubuf = ((u64)(usize)arena + (PG_SIZE - 1)) & ~(PG_SIZE - 1);
    CHECK(fake_pg_map(ubuf, PG_USER | PG_WRITE) == PG_OK);
}

static void frame_init(struct syscall_frame *f, u64 sysno)
{
    memset(f, 0, sizeof(*f));
    f->rax = sysno;
}

/* ------------------------------------------------------------------------- */
/* 1. ring0 分发：未知调用号 / write 诊断模式 / exit                          */
/* ------------------------------------------------------------------------- */

static void t_dispatch_basic(void)
{
    struct syscall_frame f;

    reset_env();

    frame_init(&f, 99);
    syscall_dispatch(&f);
    CHECK(f.rax == ~0ULL);

    frame_init(&f, NG_SYS_WRITE);
    f.rdi = 0;
    syscall_dispatch(&f);
    CHECK(f.rax == 0);
    CHECK(strstr(fake_serial_text(), "ng: syscall write (ring0)\r\n") != NULL);

    frame_init(&f, NG_SYS_EXIT);
    f.rdi = 3;
    syscall_dispatch(&f);
    CHECK(f.rax == 0);
    CHECK(strstr(fake_serial_text(), "ng: syscall exit test\r\n") != NULL);

    /* WP2-3：30 readkey 原样透传 ng_input_readkey() 的返回值。 */
    fake_input_reset();
    CHECK(fake_input_readkey_calls() == 0);
    frame_init(&f, NG_SYS_READKEY);
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)NG_EAGAIN);
    CHECK(fake_input_readkey_calls() == 1);

    fake_input_set_readkey(0x0061001EULL); /* flags=0, ascii='a', scancode=0x1E */
    frame_init(&f, NG_SYS_READKEY);
    syscall_dispatch(&f);
    CHECK(f.rax == 0x0061001EULL);
    CHECK(fake_input_readkey_calls() == 2);
}

/* ------------------------------------------------------------------------- */
/* 2. write(用户缓冲) 的长度/指针边界                                         */
/* ------------------------------------------------------------------------- */

static void t_write_bounds(void)
{
    struct syscall_frame f;
    u32 i;

    reset_env();
    memcpy((void *)(usize)ubuf, "hello", 5);

    frame_init(&f, NG_SYS_WRITE);
    f.rdi = ubuf;
    f.rsi = 5;
    syscall_dispatch(&f);
    CHECK(f.rax == 5);
    CHECK(fake_serial_len() == 5);
    CHECK(memcmp(fake_serial_text(), "hello", 5) == 0);

    /* len=0 / len>256：EINVAL(-22)，不拷贝。 */
    fake_serial_reset();
    frame_init(&f, NG_SYS_WRITE);
    f.rdi = ubuf;
    f.rsi = 0;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)UACCESS_EINVAL);
    CHECK(fake_serial_len() == 0);

    frame_init(&f, NG_SYS_WRITE);
    f.rdi = ubuf;
    f.rsi = 257;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)UACCESS_EINVAL);
    CHECK(fake_serial_len() == 0);

    /* 256 字节正好是上限。 */
    for (i = 0; i < 256; i++) {
        ((u8 *)(usize)ubuf)[i] = (u8)('a' + (i % 26));
    }
    fake_serial_reset();
    frame_init(&f, NG_SYS_WRITE);
    f.rdi = ubuf;
    f.rsi = 256;
    syscall_dispatch(&f);
    CHECK(f.rax == 256);
    CHECK(fake_serial_len() == 256);
    CHECK(fake_serial_text()[0] == 'a');
    CHECK(fake_serial_text()[255] == (char)('a' + (255 % 26)));

    /* 坏指针：未映射（0x1000）与跨页（末 4 字节开始拷 8 字节）。 */
    frame_init(&f, NG_SYS_WRITE);
    f.rdi = 0x1000;
    f.rsi = 1;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)UACCESS_EFAULT);

    frame_init(&f, NG_SYS_WRITE);
    f.rdi = ubuf + PG_SIZE - 4;
    f.rsi = 8;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)UACCESS_EFAULT);
}

/* ------------------------------------------------------------------------- */
/* 3. IPC 调用号：参数搬运 + 返回值透传                                      */
/* ------------------------------------------------------------------------- */

static void t_dispatch_ipc(void)
{
    struct syscall_frame f;
    const struct fake_ipc_call *c;

    reset_env();
    fake_ipc_set_ret(0x00010000ULL);

    frame_init(&f, NG_SYS_PORT_CREATE);
    f.rdi = 4;
    syscall_dispatch(&f);
    CHECK(f.rax == 0x00010000ULL);
    CHECK(fake_ipc_count() == 1);
    c = fake_ipc_get(0);
    CHECK(c != NULL && c->fn == FAKE_IPC_CREATE && c->a == 4);

    fake_ipc_reset();
    fake_ipc_set_ret(0);
    frame_init(&f, NG_SYS_PORT_SEND);
    f.rdi = 0x00010000ULL;
    f.rsi = 0xDEADBEEFULL;
    f.rdx = 32;
    f.r10 = 7;
    syscall_dispatch(&f);
    CHECK(f.rax == 0);
    CHECK(fake_ipc_count() == 1);
    c = fake_ipc_get(0);
    CHECK(c != NULL && c->fn == FAKE_IPC_SEND);
    CHECK(c != NULL && c->a == 0x00010000ULL);
    CHECK(c != NULL && c->b == 0xDEADBEEFULL);
    CHECK(c != NULL && c->c == 32);
    CHECK(c != NULL && c->d == 7); /* timeout 从 r10 来 */

    fake_ipc_reset();
    frame_init(&f, NG_SYS_PORT_RECV);
    f.rdi = 3;
    f.rsi = 0x2000;
    f.rdx = 4096;
    f.r10 = IPC_TIMEOUT_FOREVER;
    syscall_dispatch(&f);
    c = fake_ipc_get(0);
    CHECK(c != NULL && c->fn == FAKE_IPC_RECV && c->a == 3);
    CHECK(c != NULL && c->b == 0x2000 && c->c == 4096);
    CHECK(c != NULL && c->d == IPC_TIMEOUT_FOREVER);

    fake_ipc_reset();
    frame_init(&f, NG_SYS_PORT_REPLY);
    f.rdi = 9;
    f.rsi = 0x3000;
    f.rdx = 16;
    f.r10 = 0;
    syscall_dispatch(&f);
    c = fake_ipc_get(0);
    CHECK(c != NULL && c->fn == FAKE_IPC_REPLY && c->a == 9);
    CHECK(c != NULL && c->b == 0x3000 && c->c == 16 && c->d == 0);

    fake_ipc_reset();
    frame_init(&f, NG_SYS_PORT_CLOSE);
    f.rdi = 0x00020001ULL;
    syscall_dispatch(&f);
    c = fake_ipc_get(0);
    CHECK(c != NULL && c->fn == FAKE_IPC_CLOSE && c->a == 0x00020001ULL);

    /* 负错误码按两补码透传（syscall 返回约定）。 */
    fake_ipc_reset();
    fake_ipc_set_ret((u64)(i64)IPC_ETIMEDOUT);
    frame_init(&f, NG_SYS_PORT_RECV);
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)IPC_ETIMEDOUT);
    CHECK(f.rax == 0xFFFFFFFFFFFFFF92ULL);
}

/* ------------------------------------------------------------------------- */
/* 4. ring3：syscall_run_user 的校验 / 统计 / 退出                           */
/* ------------------------------------------------------------------------- */

struct run_ctx {
    struct syscall_frame f[4];
    int n;
    u64 rsp;
};

static void ring3_hook(void *ctx)
{
    struct run_ctx *c = (struct run_ctx *)ctx;
    int i;

    for (i = 0; i < c->n; i++) {
        syscall_user_rsp = c->rsp;
        syscall_dispatch(&c->f[i]);
    }
}

static void t_run_user_args(void)
{
    struct ring3_result out;
    struct run_ctx ctx;

    reset_env();
    memset(&out, 0, sizeof(out));
    memset(&ctx, 0, sizeof(ctx));

    /* entry=0 / rsp 越界 / 空区间：参数非法，直接 -1。 */
    CHECK(syscall_run_user(0, RING3_STACK_TOP, RING3_STACK_LO, RING3_STACK_HI,
                           &out) == -1);
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_LO - 1, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == -1);
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_HI, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == -1);
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_HI,
                           RING3_STACK_LO, &out) == -1);
}

static void t_run_user_ok(void)
{
    struct ring3_result out;
    struct run_ctx ctx;

    reset_env();
    memcpy((void *)(usize)ubuf, "hello", 5);

    memset(&ctx, 0, sizeof(ctx));
    ctx.n = 2;
    ctx.rsp = RING3_STACK_TOP;
    frame_init(&ctx.f[0], NG_SYS_WRITE);
    ctx.f[0].rdi = ubuf;
    ctx.f[0].rsi = 5;
    frame_init(&ctx.f[1], NG_SYS_EXIT);
    ctx.f[1].rdi = 0x2A;

    fake_ring3_set(ring3_hook, &ctx);
    memset(&out, 0, sizeof(out));
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == 0);
    CHECK(out.syscalls == 2);
    CHECK(out.writes == 1);
    CHECK(out.bytes == 5);
    CHECK(out.exit_code == 0x2A);
    CHECK(out.exited == 1);
    CHECK(out.user_rsp_ok == 1);
    CHECK(out.bad_writes == 0);
    CHECK(strstr(fake_serial_text(), "ng: syscall exit test") != NULL);
    CHECK(memcmp(fake_serial_text(), "hello", 5) == 0);

    /* exit code 全 1 位也要原样保留。 */
    memset(&ctx, 0, sizeof(ctx));
    ctx.n = 1;
    ctx.rsp = RING3_STACK_TOP;
    frame_init(&ctx.f[0], NG_SYS_EXIT);
    ctx.f[0].rdi = ~0ULL;
    fake_ring3_set(ring3_hook, &ctx);
    memset(&out, 0, sizeof(out));
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == 0);
    CHECK(out.exited == 1);
    CHECK(out.exit_code == ~0ULL);
}

static void t_run_user_bad(void)
{
    struct ring3_result out;
    struct run_ctx ctx;

    reset_env();

    /* 有 syscall 但没有 exit：run_user 返回 -1，out.exited=0。 */
    memset(&ctx, 0, sizeof(ctx));
    ctx.n = 1;
    ctx.rsp = RING3_STACK_TOP;
    frame_init(&ctx.f[0], NG_SYS_WRITE);
    ctx.f[0].rdi = ubuf;
    ctx.f[0].rsi = 1;
    fake_ring3_set(ring3_hook, &ctx);
    memset(&out, 0, sizeof(out));
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == -1);
    CHECK(out.exited == 0);
    CHECK(out.syscalls == 1);

    /* 用户 RSP 不在声明的栈区间里：user_rsp_ok=0。 */
    memset(&ctx, 0, sizeof(ctx));
    ctx.n = 1;
    ctx.rsp = 0x2000;
    frame_init(&ctx.f[0], NG_SYS_EXIT);
    fake_ring3_set(ring3_hook, &ctx);
    memset(&out, 0, sizeof(out));
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == 0);
    CHECK(out.user_rsp_ok == 0);
    CHECK(out.exited == 1);

    /* ring3 的坏用户指针：bad_writes++，且诊断行是 ring3 版本。 */
    fake_serial_reset();
    memset(&ctx, 0, sizeof(ctx));
    ctx.n = 2;
    ctx.rsp = RING3_STACK_TOP;
    frame_init(&ctx.f[0], NG_SYS_WRITE);
    ctx.f[0].rdi = 0x1000;
    ctx.f[0].rsi = 4;
    frame_init(&ctx.f[1], NG_SYS_EXIT);
    fake_ring3_set(ring3_hook, &ctx);
    memset(&out, 0, sizeof(out));
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == 0);
    CHECK(out.bad_writes == 1);
    CHECK(out.writes == 0);
    CHECK(out.exited == 1);

    /* ring3 的 write(rdi=0) 诊断行。 */
    fake_serial_reset();
    memset(&ctx, 0, sizeof(ctx));
    ctx.n = 2;
    ctx.rsp = RING3_STACK_TOP;
    frame_init(&ctx.f[0], NG_SYS_WRITE);
    ctx.f[0].rdi = 0;
    frame_init(&ctx.f[1], NG_SYS_EXIT);
    fake_ring3_set(ring3_hook, &ctx);
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_LO,
                           RING3_STACK_HI, &out) == 0);
    CHECK(strstr(fake_serial_text(), "ng: syscall write (ring3)\r\n") != NULL);

    /* out 可为 NULL。 */
    memset(&ctx, 0, sizeof(ctx));
    ctx.n = 1;
    ctx.rsp = RING3_STACK_TOP;
    frame_init(&ctx.f[0], NG_SYS_EXIT);
    fake_ring3_set(ring3_hook, &ctx);
    CHECK(syscall_run_user(RING3_ENTRY, RING3_STACK_TOP, RING3_STACK_LO,
                           RING3_STACK_HI, NULL) == 0);
}

/* ------------------------------------------------------------------------- */
/* 5. per-thread 内核栈                                                       */
/* ------------------------------------------------------------------------- */

static void t_kernel_stack(void)
{
    thread_t t;

    reset_env();
    syscall_kernel_rsp0 = 0;

    /* top=0：不改（避免把 RSP0 清成 0）。 */
    syscall_set_kernel_stack(0);
    CHECK(syscall_kernel_rsp0 == 0);
    CHECK(fake_gdt_calls() == 0);

    syscall_set_kernel_stack(0x12345000ULL);
    CHECK(syscall_kernel_rsp0 == 0x12345000ULL);
    CHECK(fake_gdt_rsp0() == 0x12345000ULL);

    /* NULL 线程（boot）：回退到 isr.S 的静态栈。 */
    syscall_set_thread_stack(NULL);
    CHECK(syscall_kernel_rsp0 == (u64)(usize)syscall_stack_top);

    memset(&t, 0, sizeof(t));
    t.stack_top = (void *)(usize)0x700000ULL;
    syscall_set_thread_stack(&t);
    CHECK(syscall_kernel_rsp0 == 0x700000ULL);
    CHECK(fake_gdt_rsp0() == 0x700000ULL);

    /* TCB 有栈顶时用 TCB；stack_top 为空也回退 boot 栈。 */
    t.stack_top = NULL;
    syscall_set_thread_stack(&t);
    CHECK(syscall_kernel_rsp0 == (u64)(usize)syscall_stack_top);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */
/* 6. WP2-1：20..27 VFS 分发（用户指针 -> uaccess -> vfs）                     */
/* ------------------------------------------------------------------------- */

static void t_dispatch_fs(void)
{
    struct syscall_frame f;
    struct fs_env env;
    struct vfs_stat st;
    char *path = (char *)(usize)ubuf;
    char *buf = (char *)(usize)(ubuf + 128);
    char *entp = (char *)(usize)(ubuf + 768);
    int fd;

    reset_env();
    fake_fs_reset();
    fake_fs_env(&env);
    CHECK(fs_sys_init(&env) == VFS_OK);
    CHECK(strcmp(fs_sys_ctx()->cwd, "/") == 0);

    /* 20 open("/tmp/a", O_CREAT|O_RDWR, 0644) */
    strcpy(path, "/tmp/a");
    frame_init(&f, NG_SYS_OPEN);
    f.rdi = ubuf;
    f.rsi = VFS_O_CREAT | VFS_O_RDWR;
    f.rdx = 0644;
    syscall_dispatch(&f);
    fd = (int)(i64)f.rax;
    CHECK(fd > 0);

    /* 22 write(fd, "hello", 5) */
    memcpy(buf, "hello", 5);
    frame_init(&f, NG_SYS_WRITE_FD);
    f.rdi = (u64)fd;
    f.rsi = ubuf + 128;
    f.rdx = 5;
    syscall_dispatch(&f);
    CHECK(f.rax == 5);

    /* 23 close(fd) 后重开只读：新 fd 从 offset 0 读回 "hello"。 */
    frame_init(&f, NG_SYS_CLOSE);
    f.rdi = (u64)fd;
    syscall_dispatch(&f);
    CHECK(f.rax == 0);
    strcpy(path, "/tmp/a");
    frame_init(&f, NG_SYS_OPEN);
    f.rdi = ubuf;
    f.rsi = VFS_O_RDONLY;
    f.rdx = 0;
    syscall_dispatch(&f);
    fd = (int)(i64)f.rax;
    CHECK(fd > 0);

    /* 21 read(fd, buf, 8) == 5 + 内容 */
    memset(buf, 0, 16);
    frame_init(&f, NG_SYS_READ);
    f.rdi = (u64)fd;
    f.rsi = ubuf + 128;
    f.rdx = 8;
    syscall_dispatch(&f);
    CHECK(f.rax == 5);
    CHECK(memcmp(buf, "hello", 5) == 0);

    /* 24 stat("/tmp/a", &st) */
    frame_init(&f, NG_SYS_STAT);
    f.rdi = ubuf;
    f.rsi = ubuf + 256;
    syscall_dispatch(&f);
    CHECK(f.rax == 0);
    memcpy(&st, (void *)(usize)(ubuf + 256), sizeof(st));
    CHECK(st.type == VFS_NODE_FILE);
    CHECK(st.size == 5);

    /* 25 listdir("/tmp", ents, 4) == 1 */
    strcpy(path, "/tmp");
    frame_init(&f, NG_SYS_LISTDIR);
    f.rdi = ubuf;
    f.rsi = ubuf + 768;
    f.rdx = 4;
    syscall_dispatch(&f);
    CHECK(f.rax == 1);
    {
        struct vfs_dirent e;

        memcpy(&e, entp, sizeof(e));
        CHECK(e.name_len == 1);
        CHECK(e.name[0] == 'a');
        CHECK(e.type == VFS_NODE_FILE);
    }

    /* 23 close(fd) 后 21 read 旧 fd -> EBADF */
    frame_init(&f, NG_SYS_CLOSE);
    f.rdi = (u64)fd;
    syscall_dispatch(&f);
    CHECK(f.rax == 0);
    frame_init(&f, NG_SYS_READ);
    f.rdi = (u64)fd;
    f.rsi = ubuf + 128;
    f.rdx = 1;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)VFS_EBADF);

    /* 坏路径指针 -> EFAULT（不会 #PF）；坏 stat 指针 -> EFAULT。 */
    frame_init(&f, NG_SYS_OPEN);
    f.rdi = 0x1000;
    f.rsi = VFS_O_RDONLY;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)VFS_EFAULT);
    frame_init(&f, NG_SYS_STAT);
    f.rdi = ubuf;
    f.rsi = 0;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)VFS_EFAULT);

    /* 26 mkdir / 27 unlink 成功 / 失败码。 */
    strcpy(path, "/d");
    frame_init(&f, NG_SYS_MKDIR);
    f.rdi = ubuf;
    f.rsi = 0755;
    syscall_dispatch(&f);
    CHECK(f.rax == 0);
    frame_init(&f, NG_SYS_MKDIR);
    f.rdi = ubuf;
    f.rsi = 0755;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)VFS_EEXIST);
    frame_init(&f, NG_SYS_UNLINK);
    f.rdi = ubuf;
    syscall_dispatch(&f);
    CHECK(f.rax == (u64)(i64)VFS_EISDIR);
}

/* ------------------------------------------------------------------------- */

int test_syscall(void)
{
    t_dispatch_basic();
    t_write_bounds();
    t_dispatch_ipc();
    t_dispatch_fs();
    t_run_user_args();
    t_run_user_ok();
    t_run_user_bad();
    t_kernel_stack();
    return 0;
}
