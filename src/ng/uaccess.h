/*
 * ng WP1-7: copyin/copyout（用户指针 -> 内核缓冲 / 反向）。
 *
 * 约定（与 WP1-3 的 SMAP 约定、WP1-6 的 IPC 用户指针一致）：
 *   1. 指针非 0、长度非 0、无 64 位溢出、末地址 <= 0x00007FFF_FFFF_FFFF；
 *   2. 范围里的**每一页**都要在"当前 CR3 的地址空间"里 P=1 且 U/S=1
 *      （copyout 还要求 W=1）——用 pg_current_pml4() + pg_walk_in()；
 *   3. SMAP=on 时用 stac/clac 包住实际拷贝；坏指针返回 EFAULT，绝不触发
 *      #PF（这正是一开始做 copyin/copyout 的原因）。
 *
 * 当前 CR3 是内核表时，内核地址（U=0）自然被拒；ring3 程序在内核里调
 * syscall 时 CR3 是它自己的 AS，用户页 U=1 才能通过。
 */
#ifndef NG_UACCESS_H
#define NG_UACCESS_H

#include "types.h"

/* 与 Linux/IPC 错误码同值（负值）。 */
#define UACCESS_OK 0
#define UACCESS_EFAULT (-14)
#define UACCESS_EINVAL (-22)

/* 只做页级校验，不拷贝（IPC 预校验用）。 */
int uaccess_range_ok(u64 uva, u64 len, int write);

/* 内核 <- 用户：成功 0，坏指针 UACCESS_EFAULT。 */
int copyin(void *dst, u64 uva, u64 len);

/* 内核 -> 用户：成功 0，坏指针/只读页 UACCESS_EFAULT。 */
int copyout(u64 uva, const void *src, u64 len);

/*
 * 拷一个 NUL 结尾的用户字符串到 dst（最多 max 字节，总是 NUL 结尾）。
 * 返回长度（不含 NUL）或 UACCESS_EFAULT；截断时返回 max-1。
 */
int copyin_str(char *dst, u64 uva, u64 max);

#endif /* NG_UACCESS_H */
