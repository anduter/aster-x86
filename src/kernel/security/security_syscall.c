/* =============================================================================
 * security/security_syscall.c - 身份/权限相关系统调用（M7 + ABI v1.3）
 *
 * 从 proc/trap/syscall.c 拆出的 security 组：getuid / getgid / setuid，
 * ABI v1.3 追加账号系统的一组调用（64..72）：
 *   SETEUID/SETEGID/SETGID/GETEUID/GETEGID/GETGROUPS/SETGROUPS/SETUMASK/LOGIN
 *
 * 语义按 POSIX 收窄（教学实现，够用且可断言）：
 *   - root（euid == 0）可以任意设置；
 *   - 普通用户只能 seteuid 到 {ruid, suid} / setegid 到 {rgid, sgid}；
 *   - setuid/setgid（"永久"改身份）只有 root 能调用——提权一律走 sudo/su，
 *     它们通过 acct_* 在内核里落地凭据，不在用户态拼 uid/fd 组合。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "include/fs.h"
#include "../proc/trap/syscall_internal.h"
#include "security_syscall.h"
#include "accounts.h"

/* 用户态字符串参数的最大长度（用户名/口令都远小于此） */
#define SEC_STR_MAX 64

bool security_syscall_owns(uint32_t nr)
{
    if (nr == SYS_GETUID || nr == SYS_GETGID || nr == SYS_SETUID) return true;
    return (nr >= SYS_SETEUID && nr <= SYS_LOGIN);
}

/* 取用户态字符串；失败返回 -1（调用方统一映射成 syscall 的 -1） */
static int arg_string(char *dst, uint32_t max, uint32_t uptr)
{
    if (uptr == 0U) {
        if (max > 0U) dst[0] = '\0';
        return 0;
    }
    return (copy_user_string(dst, max, uptr) == E_OK) ? 0 : -1;
}

static int sys_seteuid(uint32_t euid)
{
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (p->euid == 0U) {
        p->euid = euid;
        return 0;
    }
    /* 非 root：只能在 ruid / suid 之间切换 */
    if (euid == p->uid || euid == p->suid) {
        p->euid = euid;
        return 0;
    }
    return -1;
}

static int sys_setegid(uint32_t egid)
{
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (p->euid == 0U) {
        p->egid = egid;
        return 0;
    }
    if (egid == p->gid || egid == p->sgid) {
        p->egid = egid;
        return 0;
    }
    return -1;
}

static int sys_setgid(uint32_t gid)
{
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (p->euid != 0U) return -1;           /* 只有 root 能永久改组 */
    p->gid = gid;
    p->egid = gid;
    p->sgid = gid;
    return 0;
}

static int sys_getgroups(uint32_t ubuf, uint32_t max)
{
    proc_t *p = proc_current();
    uint32_t n;
    uint32_t tmp[ABI_NGROUPS_MAX];

    if (p == 0) return -1;

    n = (uint32_t)p->ngroups;
    if (n > ABI_NGROUPS_MAX) n = ABI_NGROUPS_MAX;
    for (uint32_t i = 0; i < n; i++) tmp[i] = p->groups[i];

    if (ubuf != 0U && max > 0U) {
        uint32_t copy = (n < max) ? n : max;

        if (copy > 0U &&
            copy_to_user((void *)(uintptr_t)ubuf, tmp, copy * sizeof(uint32_t)) != E_OK) {
            return -1;
        }
    }
    return (int)n;
}

static int sys_setgroups(uint32_t ubuf, uint32_t n)
{
    proc_t *p = proc_current();
    uint32_t tmp[ABI_NGROUPS_MAX];

    if (p == 0) return -1;
    if (p->euid != 0U) return -1;           /* 只有 root 能改附加组 */
    if (n > ABI_NGROUPS_MAX) return -1;

    if (n > 0U) {
        if (ubuf == 0U) return -1;
        if (copy_from_user(tmp, (const void *)(uintptr_t)ubuf,
                           n * sizeof(uint32_t)) != E_OK) {
            return -1;
        }
    }
    proc_set_groups(p, tmp, n);
    return 0;
}

static int sys_setumask(uint32_t mask)
{
    proc_t *p = proc_current();
    uint32_t old;

    if (p == 0) return -1;
    old = p->umask;
    p->umask = mask & 0777U;                /* umask 只影响低 9 位 */
    return (int)old;
}

/* ---- SYS_LOGIN：PAM-lite 内核钩子 ---- */
static int sys_login(uint32_t cmd, uint32_t a, uint32_t b)
{
    proc_t *p = proc_current();
    char user[SEC_STR_MAX];
    char pass[SEC_STR_MAX];
    char out[192];

    switch (cmd) {
    case LOGIN_CHECK:
        return (p != 0 && p->login_required) ? 1 : 0;

    case LOGIN_AUTH:
        if (arg_string(user, sizeof(user), a) != 0) return -1;
        if (arg_string(pass, sizeof(pass), b) != 0) return -1;
        return acct_login(user, pass);

    case LOGIN_SU:
        if (arg_string(user, sizeof(user), a) != 0) return -1;
        if (arg_string(pass, sizeof(pass), b) != 0) return -1;
        return acct_su(user, pass);

    case LOGIN_PASSWD:
        if (arg_string(pass, sizeof(pass), a) != 0) return -1;
        if (arg_string(user, sizeof(user), b) != 0) return -1;
        {
            /* 改自己的口令：必须提供旧口令（root 免旧口令） */
            acct_user_t me;

            if (p == 0) return -1;
            if (acct_find_uid(p->uid, &me) != 0) return -1;
            return acct_passwd(me.name, pass, user, p->euid == 0U);
        }

    case LOGIN_PASSWD_USER:
        /* root 给任意账户设口令（useradd 之后用） */
        if (p == 0 || p->euid != 0U) return -1;
        if (arg_string(user, sizeof(user), a) != 0) return -1;
        if (arg_string(pass, sizeof(pass), b) != 0) return -1;
        return acct_passwd(user, 0, pass, true);

    case LOGIN_WHO:
        if (a == 0U || b == 0U) return -1;
        {
            int n = acct_who(out, sizeof(out));

            if (n < 0) return -1;
            if (copy_to_user((void *)(uintptr_t)a, out,
                             (uint32_t)strlen(out) + 1U) != E_OK) {
                return -1;
            }
            return n;
        }

    case LOGIN_LAST:
        if (a == 0U || b == 0U) return -1;
        {
            int n = acct_last(out, sizeof(out));

            if (n < 0) return -1;
            if (copy_to_user((void *)(uintptr_t)a, out,
                             (uint32_t)strlen(out) + 1U) != E_OK) {
                return -1;
            }
            return n;
        }

    case LOGIN_LOGOUT:
        acct_logout();
        if (p != 0) p->is_login = false;
        acct_audit("LOGOUT", "-");
        return 0;

    case LOGIN_SUDO:
        if (arg_string(pass, sizeof(pass), a) != 0) return -1;
        return acct_sudo((a == 0U) ? 0 : pass);

    case LOGIN_SUDO_END:
        return acct_sudo_end();

    case LOGIN_INFO:
        if (a == 0U) return -1;
        {
            acct_user_t me;
            const char *name = "?";

            if (p == 0) return -1;
            if (acct_find_uid(p->uid, &me) == 0) name = me.name;
            ksnprintf(out, sizeof(out),
                      "uid=%u euid=%u gid=%u egid=%u groups=%u name=%s",
                      (unsigned int)p->uid, (unsigned int)p->euid,
                      (unsigned int)p->gid, (unsigned int)p->egid,
                      (unsigned int)p->ngroups, name);
            if (copy_to_user((void *)(uintptr_t)a, out,
                             (uint32_t)strlen(out) + 1U) != E_OK) {
                return -1;
            }
            return 0;
        }

    default:
        return -1;
    }
}

int security_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {
    case SYS_GETUID:
        ret = (proc_current() != 0) ? (int)proc_current()->uid : 0;
        break;
    case SYS_GETGID:
        ret = (proc_current() != 0) ? (int)proc_current()->gid : 0;
        break;
    case SYS_SETUID: {
        /* 兼容旧语义：只有 root 能改身份，四个字段一起落（setuid(2) 语义） */
        proc_t *p = proc_current();

        if (p == 0 || p->euid != 0U) { ret = -1; break; }
        proc_set_cred(p, f->ebx, (f->ecx != 0U) ? f->ecx : f->ebx);
        if (p->uid == 0U) p->umask = 022U;
        ret = 0;
        break;
    }
    case SYS_SETEUID:
        ret = sys_seteuid(f->ebx);
        break;
    case SYS_SETEGID:
        ret = sys_setegid(f->ebx);
        break;
    case SYS_SETGID:
        ret = sys_setgid(f->ebx);
        break;
    case SYS_GETEUID:
        ret = (proc_current() != 0) ? (int)proc_current()->euid : 0;
        break;
    case SYS_GETEGID:
        ret = (proc_current() != 0) ? (int)proc_current()->egid : 0;
        break;
    case SYS_GETGROUPS:
        ret = sys_getgroups(f->ebx, f->ecx);
        break;
    case SYS_SETGROUPS:
        ret = sys_setgroups(f->ebx, f->ecx);
        break;
    case SYS_SETUMASK:
        ret = sys_setumask(f->ebx);
        break;
    case SYS_LOGIN:
        ret = sys_login(f->ebx, f->ecx, f->edx);
        break;
    default:
        return -1000;
    }
    return ret;
}
