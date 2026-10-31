/*
 * ng WP1-3: syscall（STAR/LSTAR/FMASK）与 ring0/ring3 往返测试。
 * ng WP1-6: 调用号表（0/1 已用；10..14 为 IPC/句柄）。
 */
#ifndef NG_SYSCALL_H
#define NG_SYSCALL_H

/*
 * 调用号（ABI 1.0 草案，WP4-1 冻结）：
 *   rax=调用号，rdi/rsi/rdx/r10 = 参数，返回值放 rax（负 = 错误码）。
 *   0  write(serial)                       参数未用
 *   1  exit test                           参数未用
 *  10  port_create(depth)                  -> handle / -err
 *  11  port_send(handle, msg, len, tmo)    -> 0 / -err
 *  12  port_recv(handle, buf, max, tmo)    -> 消息总长 / -err
 *  13  port_reply(ticket, msg, len, tmo)   -> 0 / -err
 *  14  port_close(handle)                  -> 0 / -err
 * 其中 msg/buf 是用户指针（内核做 U/S 页校验 + SMAP copyin/copyout）；
 * timeout 单位是 TSC tick（0 = 非阻塞，~0 = 永久）。
 */
#define NG_SYS_WRITE 0u
#define NG_SYS_EXIT 1u
#define NG_SYS_PORT_CREATE 10u
#define NG_SYS_PORT_SEND 11u
#define NG_SYS_PORT_RECV 12u
#define NG_SYS_PORT_REPLY 13u
#define NG_SYS_PORT_CLOSE 14u

/* 置 EFER.SCE 与 STAR/LSTAR/FMASK，打印读回值。 */
void syscall_init(void);

/* ring0 syscall 往返 + ring3 syscall 往返；返回失败项数。 */
int syscall_selftest(void);

#endif /* NG_SYSCALL_H */
