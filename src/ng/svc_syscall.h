/*
 * ng WP2-5: auth/log/pkg 的 syscall 胶水（调用号 31..35）。
 *
 * 这一层只做：
 *   1. 用户指针 -> 内核缓冲（copyin_str/copyin/copyout，全部经 uaccess）；
 *   2. 长度上限校验（服务自身的长度限制之外再加一层 syscall 上限）；
 *   3. 把参数转给 L1 的 auth / log / pkg 函数，返回值原样透传（负 = errno）。
 *
 * 调用号（syscall.h）：
 *   31 login(name, pass)        -> uid / -ENOENT / -EACCES / -EINVAL / -EFAULT
 *   32 dmesg(buf, max)          -> 字节数 / -EINVAL / -EFAULT
 *   33 log_write(tag, msg)      -> 0 / -EINVAL / -EFAULT
 *   34 pkg_list(buf, max)       -> 包数 / -ENOSPC / -EINVAL / -EFAULT
 *   35 pkg_verify(name)         -> 0 / -ENOENT / -EINVAL / -EIO / -EFAULT
 */
#ifndef NG_SVC_SYSCALL_H
#define NG_SVC_SYSCALL_H

#include "types.h"

/* 单次 syscall 的参数/缓冲上限（L1 自身还会再校验一次）。 */
#define SVC_LOG_SYS_READ_MAX 4096u
#define SVC_PKG_LIST_MAX 512u

int ng_sys_login(u64 uname, u64 upass);
int ng_sys_dmesg(u64 ubuf, u64 max);
int ng_sys_log_write(u64 utag, u64 umsg);
int ng_sys_pkg_list(u64 ubuf, u64 max);
int ng_sys_pkg_verify(u64 uname);

#endif /* NG_SVC_SYSCALL_H */
