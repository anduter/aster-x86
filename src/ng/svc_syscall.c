/*
 * ng WP2-5: auth/log/pkg syscall 胶水实现（31..35）。
 *
 * 与 fs_syscall.c 的分工相同：L1 服务不 include 任何内核内部头；用户指针
 * 只在这里经 uaccess 处理。所有成功路径都返回 >= 0，失败返回负 errno。
 */
#include "svc_syscall.h"

#include "auth.h"
#include "log.h"
#include "pkg.h"
#include "uaccess.h"
#include "vfs.h"

/* 用户 NUL 字符串 -> 内核缓冲；返回长度，坏指针/超长返回负 errno。 */
static int read_user_string(u64 uva, char *buf, u32 cap)
{
    int n;

    if (uva == 0 || buf == NULL || cap < 2) {
        return VFS_EFAULT;
    }
    n = copyin_str(buf, uva, (u64)cap);
    if (n < 0) {
        return VFS_EFAULT;
    }
    if ((u32)n >= cap - 1u) {
        return VFS_ENAMETOOLONG;
    }
    return n;
}

int ng_sys_login(u64 uname, u64 upass)
{
    char name[AUTH_NAME_MAX];
    char pass[AUTH_PASS_MAX];
    int rc;

    rc = read_user_string(uname, name, (u32)sizeof(name));
    if (rc < 0) {
        return rc;
    }
    rc = read_user_string(upass, pass, (u32)sizeof(pass));
    if (rc < 0) {
        return rc;
    }
    return auth_login(name, pass);
}

int ng_sys_dmesg(u64 ubuf, u64 max)
{
    char buf[SVC_LOG_SYS_READ_MAX];
    u32 n;

    if (max == 0 || max > (u64)SVC_LOG_SYS_READ_MAX) {
        return VFS_EINVAL;
    }
    if (ubuf == 0) {
        return VFS_EFAULT;
    }
    n = log_read(buf, (u32)max);
    if (n > 0 && copyout(ubuf, buf, (u64)n) != UACCESS_OK) {
        return VFS_EFAULT;
    }
    return (int)n;
}

int ng_sys_log_write(u64 utag, u64 umsg)
{
    char tag[LOG_TAG_MAX];
    char msg[LOG_MSG_MAX];
    int rc;

    rc = read_user_string(utag, tag, (u32)sizeof(tag));
    if (rc < 0) {
        return rc;
    }
    rc = read_user_string(umsg, msg, (u32)sizeof(msg));
    if (rc < 0) {
        return rc;
    }
    rc = log_write(tag, msg);
    return (rc < 0) ? rc : 0;
}

int ng_sys_pkg_list(u64 ubuf, u64 max)
{
    char buf[SVC_PKG_LIST_MAX];
    int n;

    if (max == 0 || max > (u64)SVC_PKG_LIST_MAX) {
        return VFS_EINVAL;
    }
    if (ubuf == 0) {
        return VFS_EFAULT;
    }
    n = pkg_list(buf, (u32)max);
    if (n < 0) {
        return n;
    }
    {
        u32 len = 0;
        u32 i;

        for (i = 0; buf[i] != '\0'; i++) {
            len++;
        }
        len++; /* 含 NUL */
        if (len > (u32)max) {
            return VFS_ENOSPC;
        }
        if (copyout(ubuf, buf, (u64)len) != UACCESS_OK) {
            return VFS_EFAULT;
        }
    }
    return n;
}

int ng_sys_pkg_verify(u64 uname)
{
    char name[PKG_NAME_MAX];
    int rc;

    rc = read_user_string(uname, name, (u32)sizeof(name));
    if (rc < 0) {
        return rc;
    }
    return pkg_verify(name);
}
