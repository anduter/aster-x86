/* =============================================================================
 * pkg/pkg_syscall.c - SYS_PKG(77)：ring3 组件 CLI 的内核入口
 *
 * 参数约定（legacy int 0x81）：ebx=op, ecx=a, edx=b。
 * op 编码与语义见 include/pkg.h 和 docs/claims/pkg.md：
 *
 *   0 LIST      a=user buf, b=max           -> 组件条数（>=0）/ -1
 *   1 INSTALL   a=包名或路径, b=reply buf    -> 0 / -1
 *   2 REMOVE    a=组件名, b=reply buf        -> 0 / -1
 *   3 VERIFY    a=组件名, b=reply buf        -> 0 / -1
 *   4 COMPONENT_PORT      a=port object id  -> 仅 _pkgd 启动的组件进程
 *   5 COMPONENT_UNREG     a=port object id  -> 仅 _pkgd 启动的组件进程
 *
 * 身份：一律取 proc_current() 的 euid，不由命令行自述。普通用户只能安装/
 * 卸载自己 <home>/Apps 下的 app；service 需要 root（= CAP_PKG_ADMIN）。
 * 回复文本："ok ..." / "err <code> <detail>"，写进 b 指向的 ≤128 字节缓冲。
 * ============================================================================= */
#include "include/pkg.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/proc.h"
#include "include/fs.h"
#include "../proc/trap/syscall_internal.h"

static uint32_t pkg_caller_euid(void)
{
    proc_t *p = proc_current();

    return (p != 0) ? proc_euid_of(p) : 0U;
}

/* 把状态码/细节格式化成 CLI 回复，写回用户缓冲。返回 0/-1。 */
static int pkg_cli_reply(uint32_t uptr, int status, const char *detail)
{
    char reply[PKG_CLI_REPLY_MAX];
    int n;

    if (detail == 0) detail = pkg_status_text(status);
    if (status == PKG_OK) {
        n = ksnprintf(reply, sizeof(reply), "ok %s", detail);
    } else {
        n = ksnprintf(reply, sizeof(reply), "err %d %s", status, detail);
    }
    if (n < 0) n = 0;
    if ((uint32_t)n >= sizeof(reply)) n = (int)sizeof(reply) - 1;
    reply[n] = '\0';

    if (uptr != 0U &&
        copy_to_user((void *)(uintptr_t)uptr, reply,
                     (uint32_t)strlen(reply) + 1U) != E_OK) {
        return -1;
    }
    return (status == PKG_OK) ? 0 : -1;
}

static int pkg_arg_name(char *dst, uint32_t max, uint32_t uptr)
{
    if (uptr == 0U) return -1;
    if (copy_user_string(dst, max, uptr) != E_OK) return -1;
    return (dst[0] != '\0') ? 0 : -1;
}

int pkg_syscall(uint32_t op, uint32_t a, uint32_t b)
{
    uint32_t euid = pkg_caller_euid();

    /* P4：6..17 是 ring3 pkgd 专用的验签/落盘原语（只接受已注册的 pkgd）。 */
    if (op >= PKG_OP_SESSION_BEGIN) {
        return pkgd_primitive(op, a, b);
    }

    switch (op) {
    case PKG_OP_LIST: {
        char *buf;
        uint32_t count;
        int ok;

        if (a == 0U || b == 0U) return -1;
        if (b > 4096U) b = 4096U;
        buf = (char *)malloc(b);
        if (buf == 0) return -1;
        memset(buf, 0, b);
        /* 路由：ring3 pkgd 在线 -> IPC；auto 失败回退 ring0 的 pkg_list。 */
        count = pkgd_cli_list(euid, buf, b);
        ok = (copy_to_user((void *)(uintptr_t)a, buf, b) == E_OK);
        free(buf);
        return ok ? (int)count : -1;
    }
    case PKG_OP_INSTALL: {
        char path[FS_PATH_MAX];
        char detail[64];
        int r;

        if (pkg_arg_name(path, sizeof(path), a) != 0) return -1;
        detail[0] = '\0';
        r = pkgd_cli_install(path, euid, detail, sizeof(detail));
        return pkg_cli_reply(b, r, (detail[0] != '\0') ? detail : 0);
    }
    case PKG_OP_REMOVE: {
        char name[PKG_NAME_MAX];
        char detail[64];
        int r;

        if (pkg_arg_name(name, sizeof(name), a) != 0) return -1;
        detail[0] = '\0';
        r = pkgd_cli_remove(name, euid, detail, sizeof(detail));
        return pkg_cli_reply(b, r, (detail[0] != '\0') ? detail : 0);
    }
    case PKG_OP_VERIFY: {
        char name[PKG_NAME_MAX];
        char detail[64];
        int r;

        if (pkg_arg_name(name, sizeof(name), a) != 0) return -1;
        detail[0] = '\0';
        r = pkgd_cli_verify(name, euid, detail, sizeof(detail));
        return pkg_cli_reply(b, r, (detail[0] != '\0') ? detail : 0);
    }
    case PKG_OP_COMPONENT_PORT:
        return pkg_component_report_port(a);
    case PKG_OP_COMPONENT_UNREG:
        return pkg_component_unregister_port(a);
    default:
        return -1;
    }
}
