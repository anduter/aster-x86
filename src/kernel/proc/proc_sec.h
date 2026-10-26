#ifndef PROC_SEC_H
#define PROC_SEC_H

#include "proc.h"

struct interrupt_frame;

/* ---- 信号帧（SYS_SIGRETURN）合法性 ----
 *
 * sigreturn 会用用户栈上的 sigcontext 直接 iret 回用户态。用户可以在
 * handler 里改写这块内存，所以 cs/ss/eip/eflags 全部不可信：
 *   - cs 必须是用户代码段 0x1B（RPL=3），ss 必须是用户数据段 0x23；
 *   - eip 必须 < 0xC0000000（用户地址上界）；
 *   - eflags 只允许用户可写位（CF/PF/AF/ZF/SF/TF/IF/DF/OF/AC/ID），
 *     显式拒绝 IOPL=3、NT、VM86（V86 模式是 ring0 提权面）、VIF/VIP、RF。
 * 旧的实现直接 memcpy + iret，用户可构造 cs=0x08/eip=内核地址 提权到 ring0。 */
bool proc_sigframe_valid(const struct interrupt_frame *f);

/* ---- SYS_KILL 权限 ----
 * root、发给自己、或目标是同一 uid 的进程（真实/生效/保存 uid 任一匹配）才允许。
 * 旧实现只按 pid 查表，alice 可以 kill root/其它用户进程。from==0 表示内核。
 */
bool proc_can_signal(const proc_t *from, const proc_t *to);

/* ---- copy_*_user 的用户地址窗口 ----
 * addr 必须落在 [用户基址, 0xC0000000) 且 addr+len 不回绕。user_ctx 为 false
 * 时（内核线程/启动自检）跳过下界检查，只保留 3GB 上界与回绕检查。
 */
bool uaccess_addr_ok(uintptr_t addr, uint32_t len, bool user_ctx);

/* 启动自检：sigreturn 帧校验 / kill uid 策略 / uaccess 窗口。 */
void audit_priv_selftest(void);

#endif
