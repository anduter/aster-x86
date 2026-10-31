/*
 * ng WP1-6: IPC / 句柄 / 端口（控制面消息，payload ≤ 4 KiB）。
 *
 * 分两层（和 WP1-5 的 sched 一样，为了 host 单测）：
 *
 *   1. `struct ipc_core` + `ipc_core_*()`：纯状态机 —— 句柄表、端口消息队列、
 *      等待队列/阻塞/超时/close 唤醒。只通过 `struct ipc_env` 的 6 个钩子
 *      依赖环境（时间、睡眠、显式唤醒、分配），所以 host 单测可以给一套
 *      fake 环境直接驱动（tests/unit-ng/test_ipc.c）。
 *   2. `ipc_*()`：内核胶水 —— env 用 TSC/thread_sleep/thread_wake/kmalloc，
 *      并提供 `ipc_sys_*()` 给 syscall 分发（用户指针校验 + SMAP copyin/out）。
 *
 * 语义（详见 docs/design/ipc-ng.md）：
 *
 *   - 端口是有界消息队列（深度由 port_create 指定，默认 16，上限 64）；
 *     队列满 send 阻塞，队列空 recv 阻塞；close 唤醒两侧等待者。
 *   - 消息头固定 16 字节 {type,len,from,flags}，payload ≤ 4096；
 *     send/recv 的 `len` 都是“含头部的总长度”，`hdr.len` 是 payload 长度。
 *   - `from` 由发送方填自己的端口 ticket（0 = 单向消息）；内核校验它必须是
 *     本表里存活的 PORT 句柄。服务端用 `hdr.from` 调 reply。
 *   - ticket 与端口句柄同构：`(port_generation << 16) | slot`。WP1-6 只有
 *     单进程，ticket 就是创建端口时返回的句柄；close 后 generation 递增，
 *     旧 ticket 一律 HANDLE_EBADF（跨进程 ticket 需要 cap grant，后续 WP）。
 *   - 权限：端口记录 owner_uid；发送/接收/回复/关闭要求
 *     `caller_uid == owner_uid || caller_uid == IPC_UID_ROOT`。当前没有
 *     uid，内核路径一律 root（TODO：进程 uid 在 WP1-7/进程模型里接）。
 *   - 超时单位是 TSC tick（没有频率校准，WP1-5 同样处理）：
 *     timeout=0 非阻塞（EAGAIN），timeout=~0 永久等待，其余 = tick 数。
 *
 * 错误码复用 handle.h 的一套（数值同 Linux errno 的负值形式）。
 */
#ifndef NG_IPC_H
#define NG_IPC_H

#include "types.h"

#include "handle.h"

/* ---- 尺寸/常量 ----------------------------------------------------------- */
#define IPC_HDR_SIZE 16u
#define IPC_PAYLOAD_MAX 4096u
#define IPC_MSG_MAX (IPC_HDR_SIZE + IPC_PAYLOAD_MAX)
#define IPC_DEPTH_DEFAULT 16u
#define IPC_DEPTH_MAX 64u
#define IPC_PORT_MAX 16u
#define IPC_WAITER_MAX 12u

/* 等待/超时约定 */
#define IPC_TIMEOUT_NOW 0ULL
#define IPC_TIMEOUT_FOREVER (~0ULL)
/* 永久等待时给 thread_sleep 的“足够远”的 deadline（约 1<<40 TSC tick）。 */
#define IPC_FOREVER_SLEEP (1ULL << 40)

/* 等待方向 */
#define IPC_WAIT_RECV 1u
#define IPC_WAIT_SEND 2u

/* 消息头 flags：本 WP 没有定义位，必须为 0（给 ABI 1.0 留位）。 */
#define IPC_FLAG_NONE 0u

/* uid：现在没有进程/uid，内核路径一律 root。 */
#define IPC_UID_ROOT 0u

/* 错误码（与 handle.h 同值；额外的是 IPC 自己的） */
#define IPC_OK HANDLE_OK
#define IPC_EPERM HANDLE_EPERM
#define IPC_ENOENT HANDLE_ENOENT
#define IPC_EBADF HANDLE_EBADF
#define IPC_ENOMEM HANDLE_ENOMEM
#define IPC_EINVAL HANDLE_EINVAL
#define IPC_ENOSPC HANDLE_ENOSPC
#define IPC_EAGAIN (-11)
#define IPC_EFAULT (-14)
#define IPC_EMSGSIZE (-90)
#define IPC_ETIMEDOUT (-110)
#define IPC_ECANCELED (-125)

/* ---- 消息 ---------------------------------------------------------------- */
struct ipc_msg_hdr {
    u32 type;  /* 业务类型（调用方定义） */
    u32 len;   /* payload 字节数（0..IPC_PAYLOAD_MAX） */
    u32 from;  /* 发送方端口 ticket（0 = 无回复地址） */
    u32 flags; /* 保留，必须 0 */
};

_Static_assert(sizeof(struct ipc_msg_hdr) == IPC_HDR_SIZE,
               "ipc_msg_hdr must be 16 bytes");

struct ipc_msg {
    struct ipc_msg_hdr hdr;
    u8 payload[]; /* hdr.len 字节 */
};

/* ---- 等待者（每个阻塞中的线程一条；静态池，避免阻塞路径分配）------------- */
struct ipc_waiter {
    u32 tid;
    u32 kind; /* IPC_WAIT_RECV / IPC_WAIT_SEND */
    u32 notified;
    u32 in_use;
    struct ipc_waiter *next;
};

/* ---- 端口 ---------------------------------------------------------------- */
struct ipc_port {
    u32 used;  /* 槽位占用（close 后仍 used，等等待者退出再回收） */
    u32 closed;
    u32 depth;
    u32 head;
    u32 tail;
    u32 count;
    u32 slot; /* 创建它的句柄槽位（ticket 交叉校验） */
    u32 gen;  /* 创建它的句柄 generation（ticket 交叉校验） */
    u32 owner_uid;
    u32 owner_pid; /* ng WP3-0：属主进程（0 = 内核）。recv/close 只允许属主 */
    u32 flags; /* 预留：private 等 */
    struct ipc_msg **ring;
    struct ipc_waiter *recv_waiters;
    struct ipc_waiter *send_waiters;
    /* 统计 */
    u64 sent;
    u64 recvd;
    u64 sent_blocked;
    u64 recv_blocked;
    u64 closure_wakes;
};

/* ---- 环境钩子 ------------------------------------------------------------ */
struct ipc_env {
    void *ctx;
    u32 (*self_tid)(void *ctx); /* 当前线程 tid（等待者身份） */
    u64 (*now)(void *ctx);      /* 单调 tick（TSC） */
    /* 阻塞当前线程至多 ticks；被显式唤醒可提前返回。 */
    void (*sleep)(u64 ticks, void *ctx);
    int (*wake)(u32 tid, void *ctx); /* 显式唤醒；非阻塞线程返回 EBAD */
    void *(*alloc)(u64 size, void *ctx);
    void (*free)(void *p, void *ctx);
};

/* ---- 系统（单实例；host 单测每个用例一个）------------------------------- */
struct ipc_core {
    struct handle_table handles;
    struct ipc_port ports[IPC_PORT_MAX];
    struct ipc_waiter waiters[IPC_WAITER_MAX];
    struct ipc_waiter *free_waiters;
    const struct ipc_env *env;
    u32 port_live;
    /* 统计 */
    u64 sends;
    u64 recvs;
    u64 replies;
    u64 closes;
    u64 timeouts;
    u64 eagains;
    u64 bad_handles;
    u64 perms;
    u64 waiter_wakes;
};

void ipc_core_init(struct ipc_core *c, const struct ipc_env *env);

/* 创建端口，*out_handle = (gen<<16)|slot；depth=0 → 默认 16。 */
int ipc_core_port_create(struct ipc_core *c, u32 depth, u32 owner_uid,
                         u32 *out_handle);

/*
 * ng WP3-0：带属主 pid 的端口创建。返回的 *out_handle 就是 **ticket**
 * （全局端口身份，(gen<<16)|slot），可以交给别的进程；进程自己的句柄表由
 * proc.c 另记一份。
 */
int ipc_core_port_create_pid(struct ipc_core *c, u32 depth, u32 owner_uid,
                             u32 owner_pid, u32 *out_ticket);

/* ticket = 端口的全局身份；ticket == 0 表示"无来源"（单向消息）。 */
u32 ipc_core_port_ticket(const struct ipc_port *p);
/* 按 ticket 解析端口（校验槽位/generation）；无效返回 NULL。 */
struct ipc_port *ipc_core_port_by_ticket(struct ipc_core *c, u32 ticket);
u32 ipc_core_port_owner_pid(const struct ipc_port *p);

/*
 * ng WP3-0：把 tid 从所有端口的等待链上摘掉（kill 一个阻塞在 recv/send 的
 * 进程时必须做，否则端口上的"已通知"等待者会吞掉后续唤醒）。返回摘除数。
 */
u32 ipc_core_cancel_thread(struct ipc_core *c, u32 tid);
/* 诊断：正在使用中的等待者条数（自检里验证 kill 没有泄漏等待者）。 */
u32 ipc_core_waiter_live(const struct ipc_core *c);

/*
 * 发送/回复。msg 指向“头部 + payload”的完整消息，len 含头部。
 * timeout 语义见文件头；会阻塞并在超时/close 时返回负错误码。
 */
int ipc_core_send(struct ipc_core *c, u32 port_handle, const void *msg, u32 len,
                  u32 caller_uid, u64 timeout);
/* ticket 是请求方端口句柄（与 send 的目标同构，但语义上校验来源）。 */
int ipc_core_reply(struct ipc_core *c, u32 ticket, const void *msg, u32 len,
                   u32 caller_uid, u64 timeout);

/*
 * 接收：buf/max 是调用方缓冲区；返回消息真实总长度（可能 > max，此时只
 * 拷贝 max 字节，调用方据返回值判断截断）。*out_len 同返回值。
 */
int ipc_core_recv(struct ipc_core *c, u32 port_handle, void *buf, u32 max,
                  u32 caller_uid, u64 timeout, u32 *out_len);

/* 关闭端口：句柄失效、队列清空、唤醒所有等待者（recv/send 返回 ECANCELED）。 */
int ipc_core_close(struct ipc_core *c, u32 port_handle, u32 caller_uid);

/* ---- 内核胶水 ------------------------------------------------------------ */
void ipc_init(void);
struct ipc_core *ipc_core_global(void);

/* 内核线程直接用的端口 API（uid = root；msg 是内核缓冲区）。 */
int ipc_port_create(u32 depth, u32 *out_handle);
int ipc_port_send(u32 port, const void *msg, u32 len, u64 timeout);
int ipc_port_recv(u32 port, void *buf, u32 max, u64 timeout, u32 *out_len);
int ipc_port_reply(u32 ticket, const void *msg, u32 len, u64 timeout);
int ipc_port_close(u32 port);

/*
 * syscall 入口（isr.S → syscall_dispatch → 这里）。参数与寄存器一一对应；
 * 用户指针先做页级 U/S 校验，再在 SMAP 窗口（stac/clac）里拷贝。
 * 返回值是 u64：非负 = 句柄/长度，负 = 错误码（两补码，低 32 位可当 int）。
 */
u64 ipc_sys_create(u64 depth);
u64 ipc_sys_send(u64 handle, u64 user_msg, u64 len, u64 timeout);
u64 ipc_sys_recv(u64 handle, u64 user_buf, u64 max, u64 timeout);
u64 ipc_sys_reply(u64 ticket, u64 user_msg, u64 len, u64 timeout);
u64 ipc_sys_close(u64 handle);

#endif /* NG_IPC_H */
