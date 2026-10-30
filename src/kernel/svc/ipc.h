#ifndef SVC_IPC_H
#define SVC_IPC_H

/* =============================================================================
 * svc/ipc.h - 消息端口（ABI 1.5 的 SYS_PORT_* / SYS_CAP_GRANT 内核机制）
 *
 * 契约来源：docs/design/three-layer.md §3（消息头/端口语义/capability）。
 * 与 svc/port.c 的“连接端点”（native ABI 的 PORT_CONNECT/ACCEPT）不同：
 *   - port.c 管理 accept 队列（channel 对），给 service/supervisor 用；
 *   - 本模块管理带 16 条消息队列的消息端口，给 SYS_PORT_SEND/RECV 用。
 * 两套 id 命名空间当前独立（见 docs/claims/ipc.md“设计问题 #1”），P3 再统一。
 *
 * 端口 id 是 16 位不透明句柄：`(generation << 4) | slot`。SYS_PORT_CREATE
 * 返回它；所有 SEND/RECV/REPLY/CAP_GRANT 都校验 generation，避免端口槽位
 * 复用后旧 id 误投。
 *
 * ticket = `(pid << 16) | port_handle`。REPLY 时内核校验句柄有效、端口属于
 * 该 pid；请求方把自己的 ticket 放进业务 payload 交给服务（ABI 没有独立
 * ticket 字段，见 claims“设计问题 #2”）。
 *
 * 阻塞语义：proc_block() 没有超时参数、proc 目录不在本会话可改范围，所以
 * SEND/RECV 的“阻塞有界”用 1 tick 粒度的 sleep 轮询实现（10ms 唤醒延迟）。
 * ============================================================================= */

#include "../include/types.h"

#define IPC_MAGIC          0x49504331U     /* "IPC1" */
#define IPC_HDR_SIZE       16U
#define IPC_PAYLOAD_MAX    4096U
#define IPC_NCAPS_MAX      4U
#define IPC_CAP_SIZE       8U              /* u32 type; u32 id; */
#define IPC_MSG_MAX        (IPC_HDR_SIZE + IPC_PAYLOAD_MAX + \
                            IPC_NCAPS_MAX * IPC_CAP_SIZE)

#define IPC_QUEUE_DEPTH    16U
#define IPC_PORT_MAX       16U
#define IPC_GRANT_MAX      8U

/* 端口句柄编码：低 4 位槽位，其余是 generation（12 位） */
#define IPC_SLOT_MASK      0xFU
#define IPC_GEN_SHIFT      4

/* SEND 满队列 / RECV 空队列的等待上限（tick = 10ms） */
#define IPC_SEND_TIMEOUT_TICKS 20U
#define IPC_RECV_TIMEOUT_TICKS 30U

/* 消息头 flags（设计 §3.1） */
#define IPC_F_NEED_REPLY   0x1U
#define IPC_F_NO_REPLY     0x2U
#define IPC_F_ALL          (IPC_F_NEED_REPLY | IPC_F_NO_REPLY)

/* SYS_PORT_CREATE(flags)：bit0 = 私有（仅同 uid 可收发） */
#define IPC_FLAG_PRIVATE   0x1U

/* capability 类型（设计 §3.3） */
#define IPC_CAP_PORT       1U
#define IPC_CAP_MMIO       2U
#define IPC_CAP_IRQ        3U
#define IPC_CAP_DMA        4U
#define IPC_CAP_FILE       5U
#define IPC_CAP_SOCKET     6U

/* cap rights 位（CAP_GRANT 的第 4 个参数走 esi，0 = 默认 RW） */
#define IPC_RIGHT_READ     0x1U
#define IPC_RIGHT_WRITE    0x2U
#define IPC_RIGHT_GRANT    0x4U
#define IPC_RIGHT_ADMIN    0x8U
#define IPC_RIGHTS_ALL     (IPC_RIGHT_READ | IPC_RIGHT_WRITE | \
                            IPC_RIGHT_GRANT | IPC_RIGHT_ADMIN)
#define IPC_RIGHTS_DEFAULT (IPC_RIGHT_READ | IPC_RIGHT_WRITE)

/* 错误码（与 Linux errno 一致；syscall 返回负值） */
#define IPC_EAGAIN         11              /* 超时 */
#define IPC_EINTR          4               /* 被信号打断 */

struct process;

static inline uint32_t ipc_ticket_make(uint32_t pid, uint32_t handle)
{
    return (pid << 16) | (handle & 0xFFFFU);
}

/* ---- 内核内部 API（syscall 封装与自检共用） ----
 * caller 是发起操作的进程；syscall 路径传 proc_current()，自检可以传当前
 * 内核线程（supervisor）。caller 的 euid 决定权限，caller->pid 记录归属。 */
int  ipc_port_create(uint32_t flags, struct process *caller);
int  ipc_port_send(uint32_t port, const void *msg, uint32_t len,
                   struct process *caller);
/* 返回消息的真实长度（可能 > maxlen，此时 buf 只收到 maxlen 字节）。 */
int  ipc_port_recv(uint32_t port, void *buf, uint32_t maxlen,
                   struct process *caller);
/* ticket 是请求方的 `(pid<<16)|port_handle`（见文件头）。 */
int  ipc_port_reply(uint32_t ticket, const void *msg, uint32_t len,
                    struct process *caller);
int  ipc_cap_grant(uint32_t port, uint32_t type, uint32_t id, uint32_t rights,
                   struct process *caller);
/* P5 硬件 cap 扩展：len_pages 只对 CAP_MMIO 有意义（0 -> 1 页）。
 * SYS_CAP_GRANT 把长度放在 rights 的高 16 位（低 4 位是 rights），
 * 老的三参调用 rights 只有低 4 位 -> 默认 1 页，兼容。 */
int  ipc_cap_grant_ex(uint32_t port, uint32_t type, uint32_t id,
                      uint32_t rights, uint32_t len_pages,
                      struct process *caller);
/* 查“某进程拥有的端口上”的 grant。id==0 表示同类型任意 id（取最大 id，
 * 供 CAP_DMA 配额用）；id!=0 精确匹配（MMIO 基址）。成功返回 0。 */
int  ipc_cap_lookup(uint32_t owner_pid, uint32_t type, uint32_t id,
                    uint32_t *out_id, uint32_t *out_rights,
                    uint32_t *out_len_pages);
/* 撤销一条 grant（SYS_CAP_GRANT 的 IRQ 绑定失败回滚用）。 */
int  ipc_cap_revoke(uint32_t port, uint32_t type, uint32_t id,
                    struct process *caller);
/* 释放端口（自检/未来的 SYS_PORT_CLOSE 用；同 uid 或 root 可释放）。 */
void ipc_port_release(uint32_t port, struct process *caller);

/* 内核事件投递（IRQ -> 端口）。sender_pid 记 0（内核），不做发送权限检查；
 * 队列满返回 -IPC_EAGAIN（中断路径不能阻塞）。 */
int  ipc_port_post(uint32_t port, uint32_t type, const void *payload,
                   uint32_t len);

/* 诊断/自检：端口句柄是否有效、是否已记录某个 grant。 */
bool ipc_port_exists(uint32_t port);
bool ipc_cap_granted(uint32_t port, uint32_t type, uint32_t id);
/* 端口归属（SYS_SVC_REGISTER 校验“只能登记自己的端口”）。不存在返回 0。 */
uint32_t ipc_port_owner_pid(uint32_t port);
uint32_t ipc_port_owner_uid(uint32_t port);

/* 内核侧自检（-Test 构建由 supervisor 线程在调度器启动后调用一次）。 */
void ipc_selftest(void);

#endif /* SVC_IPC_H */
