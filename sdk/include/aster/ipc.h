/*
 * Aster SDK alpha —— IPC / 端口（ABI 1.0 未冻结）
 *
 * 对应内核调用号 10..14（见 src/ng/ipc.h）。控制面消息 = **16 字节头**
 * {type,len,from,flags} + payload（≤ 4096 字节）；数据面（像素）走共享内存，
 * 不要用 IPC 传大块像素。
 *
 * ticket 是端口的全局身份 `(generation << 16) | slot`，不是进程私有句柄：
 *   - 可以放进消息或 argv 交给别的进程；
 *   - 但 recv/close 只允许端口属主进程（内核按 pid 校验）；
 *   - ticket == 0 表示"无回复地址"（单向消息）。
 *
 * timeout 单位是 TSC tick（0 = 非阻塞 / -EAGAIN，~0 = 永久）。
 * 端口深度：create 时给（0 = 默认 16），上限 64。
 */
#ifndef ASTER_IPC_H
#define ASTER_IPC_H

#include <aster/types.h>
#include <aster/syscall.h>

/* ---- 尺寸 / 常量 --------------------------------------------------------- */
#define ASTER_IPC_HDR_SIZE 16u
#define ASTER_IPC_PAYLOAD_MAX 4096u
#define ASTER_IPC_MSG_MAX (ASTER_IPC_HDR_SIZE + ASTER_IPC_PAYLOAD_MAX)
#define ASTER_PORT_DEPTH_DEFAULT 16u
#define ASTER_PORT_DEPTH_MAX 64u

/* ---- 消息头（与 src/ng/ipc.h 的 struct ipc_msg_hdr 逐字节一致） --------- */
struct aster_ipc_hdr {
    u32 type;  /* 业务类型（SDK 的 display 协议用 ASTER_DISPLAY_MSG_*） */
    u32 len;   /* payload 字节数，不含本头 */
    u32 from;  /* 发送方端口 ticket（0 = 单向） */
    u32 flags; /* 保留，必须 0 */
};
_Static_assert(sizeof(struct aster_ipc_hdr) == ASTER_IPC_HDR_SIZE,
               "aster_ipc_hdr must be 16 bytes");

/* ---- 原始端口 syscall ---------------------------------------------------- */
static inline i64 aster_port_create(u32 depth)
{
    return aster_syscall1(ASTER_SYS_PORT_CREATE, (u64)depth);
}

static inline i64 aster_port_send(u32 ticket, const void *msg, u32 len,
                                  u64 timeout)
{
    if (msg == NULL || len < ASTER_IPC_HDR_SIZE) {
        return ASTER_EINVAL;
    }
    return aster_syscall4(ASTER_SYS_PORT_SEND, (u64)ticket, (u64)(usize)msg,
                          (u64)len, timeout);
}

static inline i64 aster_port_recv(u32 ticket, void *buf, u32 max, u64 timeout)
{
    if (buf == NULL || max == 0) {
        return ASTER_EINVAL;
    }
    return aster_syscall4(ASTER_SYS_PORT_RECV, (u64)ticket, (u64)(usize)buf,
                          (u64)max, timeout);
}

static inline i64 aster_port_reply(u32 ticket, const void *msg, u32 len,
                                   u64 timeout)
{
    if (msg == NULL || len < ASTER_IPC_HDR_SIZE) {
        return ASTER_EINVAL;
    }
    return aster_syscall4(ASTER_SYS_PORT_REPLY, (u64)ticket, (u64)(usize)msg,
                          (u64)len, timeout);
}

static inline i64 aster_port_close(u32 ticket)
{
    return aster_syscall1(ASTER_SYS_PORT_CLOSE, (u64)ticket);
}

/* ---- 高层：带头部的消息收发 --------------------------------------------- */
static inline u32 aster_ipc__get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static inline void aster_ipc__put32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

/*
 * 组一条 {头 + payload} 并 send/reply。payload 可以为 NULL（len=0）。
 * 内部用 4 KiB 栈缓冲；在 16 KiB 用户栈上调用没问题，但不要递归套多层。
 */
static inline i64 aster_ipc__send_to(u32 ticket, u32 from, u32 type,
                                     const void *payload, u32 len, u64 timeout,
                                     int is_reply)
{
    u8 frame[ASTER_IPC_MSG_MAX];
    i64 rc;

    if (len > ASTER_IPC_PAYLOAD_MAX) {
        return ASTER_EMSGSIZE;
    }
    if (len != 0 && payload == NULL) {
        return ASTER_EINVAL;
    }
    aster_ipc__put32(frame + 0, type);
    aster_ipc__put32(frame + 4, len);
    aster_ipc__put32(frame + 8, from);
    aster_ipc__put32(frame + 12, 0);
    if (len != 0) {
        aster_memcopy(frame + ASTER_IPC_HDR_SIZE, payload, (u64)len);
    }
    if (is_reply != 0) {
        rc = aster_port_reply(ticket, frame, ASTER_IPC_HDR_SIZE + len, timeout);
    } else {
        rc = aster_port_send(ticket, frame, ASTER_IPC_HDR_SIZE + len, timeout);
    }
    return rc;
}

static inline i64 aster_ipc_send(u32 to_ticket, u32 from_ticket, u32 type,
                                 const void *payload, u32 len, u64 timeout)
{
    return aster_ipc__send_to(to_ticket, from_ticket, type, payload, len,
                              timeout, 0);
}

static inline i64 aster_ipc_reply(u32 to_ticket, u32 from_ticket, u32 type,
                                  const void *payload, u32 len, u64 timeout)
{
    return aster_ipc__send_to(to_ticket, from_ticket, type, payload, len,
                              timeout, 1);
}

/*
 * 收一条消息：成功返回 payload 长度（≥0），out_type / out_from 填头部；
 * payload 拷进 buf（cap 字节），payload 比 cap 大返回 -EMSGSIZE 且不拷贝。
 * 超时返回 -ETIMEDOUT，端口关闭返回 -ECANCELED。
 */
static inline i64 aster_ipc_recv(u32 ticket, u32 *out_type, void *buf, u32 cap,
                                 u32 *out_from, u64 timeout)
{
    u8 frame[ASTER_IPC_MSG_MAX];
    u32 hlen;
    i64 rc;

    if (buf == NULL && cap != 0) {
        return ASTER_EINVAL;
    }
    rc = aster_port_recv(ticket, frame, (u32)sizeof(frame), timeout);
    if (rc < 0) {
        return rc;
    }
    if (rc < (i64)ASTER_IPC_HDR_SIZE || rc > (i64)sizeof(frame)) {
        return ASTER_EINVAL;
    }
    hlen = aster_ipc__get32(frame + 4);
    if ((u64)hlen + (u64)ASTER_IPC_HDR_SIZE != (u64)rc) {
        return ASTER_EINVAL;
    }
    if (hlen > ASTER_IPC_PAYLOAD_MAX) {
        return ASTER_EINVAL;
    }
    if (hlen > cap) {
        return ASTER_EMSGSIZE;
    }
    if (out_type != NULL) {
        *out_type = aster_ipc__get32(frame + 0);
    }
    if (out_from != NULL) {
        *out_from = aster_ipc__get32(frame + 8);
    }
    if (hlen != 0) {
        aster_memcopy(buf, frame + ASTER_IPC_HDR_SIZE, (u64)hlen);
    }
    return (i64)hlen;
}

#endif /* ASTER_IPC_H */
