/* =============================================================================
 * apps/getty.c - 控制台 getty（内核内建程序）
 *
 * 职责只有一个：把本 tty 标成"需要登录"，然后 exec /bin/shell。
 * 之后由用户态 shell 的 login 前端提示 login:/Password:，内核通过 SYS_LOGIN
 * 校验 /etc/shadow 并把凭据落到进程上（详见 security/accounts.c）。
 * ============================================================================= */
#include "getty.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "include/dev.h"
#include "include/userland.h"
#include "include/apps.h"

#define GETTY_HOSTNAME "osteachos"

void getty_entry(void)
{
    proc_t *p = proc_current();

    if (p != 0) {
        /* exec 不会重置 proc_t：这个标志会带到 /bin/shell 里，
         * shell 用 SYS_LOGIN(LOGIN_CHECK) 读它决定是否先跑登录前端。 */
        p->login_required = true;
        p->is_login = false;
    }

    kprintf("getty: tty0 %s login required\n", GETTY_HOSTNAME);
    console_set_status("login required");

    if (user_exec("system:/bin/shell") != 0) {
        kprintln("getty: cannot load /bin/shell, falling back to kernel shell");
        if (p != 0) p->login_required = false;
        shell_entry();
    }
}
