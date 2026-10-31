/*
 * ng WP1-6: IPC 内核胶水 —— 环境钩子 + syscall 入口（用户指针校验）。
 *
 * 纯状态机在 ipc_core.c（host 单测直接驱动）。这里只做三件事：
 *   1. 把 `struct ipc_env` 绑到 ng 内核：TSC 时间源、thread_sleep/thread_wake、
 *      kmalloc/kfree、当前线程 tid；
 *   2. 提供内核线程直接用的 `ipc_port_*()`（uid = root）；
 *   3. 提供 `ipc_sys_*()`：syscall 参数校验 + 用户页 U/S 检查 + SMAP 窗口
 *      （stac/clac）内的 copyin/copyout，然后调状态机。
 *
 * 用户指针校验是页级的：地址必须落在 U/S=1 的已映射页里（recv 还要求
 * 可写），越界/内核地址/未映射一律 -EFAULT。SMAP=off（qemu64）时跳过
 * stac/clac（在无 SMAP 的 CPU 上执行会 #UD），与 isr.S 的 clac 判定一致。
 *
 * 阻塞注意：recv/send 可能在 syscall 里阻塞并切走线程，而 syscall 入口用的
 * 是 WP1-3 的**单个静态内核栈**。当前只有内核线程用 IPC、ring3 也没有
 * 并发用户线程，所以不会重入；per-thread syscall 栈属于用户态多线程/进程
 * WP（见 claims 遗留）。
 */
#include "ipc.h"

#include "heap.h"
#include "io.h"
#include "paging.h"
#include "thread.h"
#include "uaccess.h"

/* x86-64 低半区用户地址上限（canonical lower half）。 */
#define IPC_USER_VA_MAX 0x00007FFFFFFFFFFFULL

static struct ipc_core g_ipc;

/* ------------------------------------------------------------------------- */
/* 内核环境钩子                                                               */
/* ------------------------------------------------------------------------- */

static u32 kenv_self_tid(void *ctx)
{
    thread_t *t;

    (void)ctx;
    t = thread_self();
    return (t != NULL) ? t->tid : 0;
}

static u64 kenv_now(void *ctx)
{
    (void)ctx;
    return sched_now(); /* RDTSC */
}

static void kenv_sleep(u64 ticks, void *ctx)
{
    (void)ctx;
    (void)thread_sleep(ticks);
}

static int kenv_wake(u32 tid, void *ctx)
{
    (void)ctx;
    return thread_wake(tid);
}

static void *kenv_alloc(u64 size, void *ctx)
{
    (void)ctx;
    return kmalloc(size);
}

static void kenv_free(void *p, void *ctx)
{
    (void)ctx;
    (void)kfree(p);
}

static const struct ipc_env kernel_env = {
    NULL, kenv_self_tid, kenv_now, kenv_sleep, kenv_wake, kenv_alloc, kenv_free};

void ipc_init(void)
{
    ipc_core_init(&g_ipc, &kernel_env);
}

struct ipc_core *ipc_core_global(void)
{
    return &g_ipc;
}

/* ------------------------------------------------------------------------- */
/* 内核线程 API（uid = root）                                                 */
/* ------------------------------------------------------------------------- */

int ipc_port_create(u32 depth, u32 *out_handle)
{
    return ipc_core_port_create(&g_ipc, depth, IPC_UID_ROOT, out_handle);
}

int ipc_port_send(u32 port, const void *msg, u32 len, u64 timeout)
{
    return ipc_core_send(&g_ipc, port, msg, len, IPC_UID_ROOT, timeout);
}

int ipc_port_recv(u32 port, void *buf, u32 max, u64 timeout, u32 *out_len)
{
    return ipc_core_recv(&g_ipc, port, buf, max, IPC_UID_ROOT, timeout, out_len);
}

int ipc_port_reply(u32 ticket, const void *msg, u32 len, u64 timeout)
{
    return ipc_core_reply(&g_ipc, ticket, msg, len, IPC_UID_ROOT, timeout);
}

int ipc_port_close(u32 port)
{
    return ipc_core_close(&g_ipc, port, IPC_UID_ROOT);
}

/* ------------------------------------------------------------------------- */
/* 用户指针校验 + copyin/copyout                                              */
/* ------------------------------------------------------------------------- */
/*
 * 实现收敛到 WP1-7 的 uaccess.c（同一套页级 U/S 校验 + SMAP 窗口），IPC 这里
 * 只做错误码翻译：用户指针失败一律 IPC_EFAULT。
 */
static int user_copy_in(void *dst, u64 uva, u64 len)
{
    return (copyin(dst, uva, len) == UACCESS_OK) ? IPC_OK : IPC_EFAULT;
}

static int user_copy_out(u64 uva, const void *src, u64 len)
{
    return (copyout(uva, src, len) == UACCESS_OK) ? IPC_OK : IPC_EFAULT;
}

/* 句柄/长度参数的高位必须为 0（避免 64 位参数截断成 32 位别名）。 */
static int ipc_arg32(u64 v)
{
    return v <= 0xFFFFFFFFULL;
}

/* ------------------------------------------------------------------------- */
/* syscall 入口                                                               */
/* ------------------------------------------------------------------------- */

u64 ipc_sys_create(u64 depth)
{
    u32 handle = 0;
    int rc;

    if (depth > (u64)IPC_DEPTH_MAX) {
        return (u64)(i64)IPC_EINVAL;
    }
    rc = ipc_port_create((u32)depth, &handle);
    if (rc != IPC_OK) {
        return (u64)(i64)rc;
    }
    return (u64)handle;
}

u64 ipc_sys_send(u64 handle, u64 user_msg, u64 len, u64 timeout)
{
    void *kbuf;
    u32 n;
    int rc;

    if (!ipc_arg32(handle)) {
        return (u64)(i64)IPC_EBADF;
    }
    if (!ipc_arg32(len)) {
        return (u64)(i64)IPC_EINVAL;
    }
    if (len < (u64)IPC_HDR_SIZE || len > (u64)IPC_MSG_MAX) {
        return (u64)(i64)((len > (u64)IPC_MSG_MAX) ? IPC_EMSGSIZE
                                                   : IPC_EINVAL);
    }
    n = (u32)len;
    kbuf = kmalloc((u64)n);
    if (kbuf == NULL) {
        return (u64)(i64)IPC_ENOMEM;
    }
    rc = user_copy_in(kbuf, user_msg, (u64)n);
    if (rc == IPC_OK) {
        rc = ipc_port_send((u32)handle, kbuf, n, timeout);
    }
    (void)kfree(kbuf);
    return (u64)(i64)rc;
}

u64 ipc_sys_recv(u64 handle, u64 user_buf, u64 max, u64 timeout)
{
    void *kbuf;
    u32 cap;
    u32 got = 0;
    int rc;

    if (!ipc_arg32(handle)) {
        return (u64)(i64)IPC_EBADF;
    }
    if (!ipc_arg32(max)) {
        return (u64)(i64)IPC_EINVAL;
    }
    if (max == 0) {
        return (u64)(i64)IPC_EINVAL;
    }
    cap = (max > (u64)IPC_MSG_MAX) ? IPC_MSG_MAX : (u32)max;
    if (!uaccess_range_ok(user_buf, (u64)cap, 1)) {
        return (u64)(i64)IPC_EFAULT;
    }
    kbuf = kmalloc((u64)cap);
    if (kbuf == NULL) {
        return (u64)(i64)IPC_ENOMEM;
    }
    rc = ipc_port_recv((u32)handle, kbuf, cap, timeout, &got);
    if (rc >= 0) {
        u32 n = ((u32)rc < cap) ? (u32)rc : cap;

        if (n > 0 && user_copy_out(user_buf, kbuf, (u64)n) != IPC_OK) {
            rc = IPC_EFAULT;
        }
    }
    (void)kfree(kbuf);
    if (rc < 0) {
        return (u64)(i64)rc;
    }
    return (u64)(u32)rc;
}

u64 ipc_sys_reply(u64 ticket, u64 user_msg, u64 len, u64 timeout)
{
    void *kbuf;
    u32 n;
    int rc;

    if (!ipc_arg32(ticket)) {
        return (u64)(i64)IPC_EBADF;
    }
    if (!ipc_arg32(len)) {
        return (u64)(i64)IPC_EINVAL;
    }
    if (len < (u64)IPC_HDR_SIZE || len > (u64)IPC_MSG_MAX) {
        return (u64)(i64)((len > (u64)IPC_MSG_MAX) ? IPC_EMSGSIZE
                                                   : IPC_EINVAL);
    }
    n = (u32)len;
    kbuf = kmalloc((u64)n);
    if (kbuf == NULL) {
        return (u64)(i64)IPC_ENOMEM;
    }
    rc = user_copy_in(kbuf, user_msg, (u64)n);
    if (rc == IPC_OK) {
        rc = ipc_port_reply((u32)ticket, kbuf, n, timeout);
    }
    (void)kfree(kbuf);
    return (u64)(i64)rc;
}

u64 ipc_sys_close(u64 handle)
{
    int rc;

    if (!ipc_arg32(handle)) {
        return (u64)(i64)IPC_EBADF;
    }
    rc = ipc_port_close((u32)handle);
    return (u64)(i64)rc;
}
