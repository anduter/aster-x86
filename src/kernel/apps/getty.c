/* =============================================================================
 * apps/getty.c - 控制台 getty（内核内建程序，tty0）
 *
 * 生产启动路径：bootseq -> proc_spawn("getty") -> 本文件的 supervisor 循环。
 *
 * 职责：
 *   1. 每个登录会话跑一个子 getty：把本 tty 标成"需要登录"，然后
 *      exec /bin/shell；之后由用户态 shell 的 login 前端提示
 *      login:/Password:，内核通过 SYS_LOGIN 校验 /etc/shadow 并落地凭据。
 *   2. shell 退出（exit）后子 getty 进程结束，**父 getty 立即重新拉起**一个
 *      新的 getty/shell（tty0 回到 login: 提示），而不是让控制台转 idle。
 *
 * 为什么父 getty 不自己 exec shell：exec 会替换当前进程映像，原进程就没有
 * "等待 + 重启"的实体了。这里父进程只做 spawn/wait，子进程负责会话，
 * 重启语义就自然成立（这也是 init/getty 的经典分工）。
 *
 * 测试模式（-Test，内核参数 autologin=root）不经过这里：bootseq 直接
 * proc_spawn("shell")，行为与引入账号系统之前完全一致。
 * ============================================================================= */
#include "getty.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "include/dev.h"
#include "include/userland.h"
#include "include/apps.h"
#include "include/security.h"

#define GETTY_HOSTNAME "osteachos"

/* 第一个 getty 进程当 supervisor。它在 spawn 子 getty **之前**置位，
 * 子进程启动时看到 true 就进入会话角色——不需要在子进程里读写别的进程
 * 字段，也不存在"子进程先跑、标志还没设"的竞态。 */
static bool getty_supervisor_claimed;

/* 一个登录会话：置 login_required -> exec shell（失败退回内核 shell） */
static void getty_session(void)
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

/* 父进程：spawn 子 getty -> wait 子进程退出 -> 重新 spawn（tty0 重启） */
static void getty_supervise(void)
{
    proc_t *self = proc_current();
    uint32_t self_pid = (self != 0) ? self->pid : 0U;

    kprintf("getty: tty0 supervisor pid=%u (respawn login after exit)\n",
            (unsigned int)self_pid);

    for (;;) {
        proc_t *child = proc_spawn("getty");
        uint32_t child_pid;
        int status = 0;
        int r;
        uint32_t spins = 0;

        if (child == 0) {
            kprintln("getty: respawn failed, falling back to kernel shell");
            getty_session();
            return;
        }

        child_pid = child->pid;
        child->parent_pid = self_pid;       /* proc_wait 只认自己的孩子 */

        /* 子进程可能瞬间退出（例如 /bin/shell 加载失败），但僵尸会留在表里，
         * 所以 wait 一定能回收；万一还没挂上父子关系就短暂让出 CPU 重试。 */
        for (;;) {
            r = proc_wait((int)child_pid, &status);
            if (r >= 0) break;
            if (++spins > 1000U) {
                kprintln("getty: wait failed, respawning anyway");
                break;
            }
            proc_yield();
        }

        kprintf("getty: tty0 session pid=%u ended status=%d -> respawn login\n",
                (unsigned int)child_pid, status);

        /* shell 已退出：清掉它的会话槽，避免连续 exit/login 后 who 显示旧会话、
         * 8 个槽位被历史 pid 占满（getty 是唯一知道旧 shell pid 的地方）。 */
        acct_session_drop(child_pid);
        console_set_status("login required");
    }
}

void getty_entry(void)
{
    if (!getty_supervisor_claimed) {
        getty_supervisor_claimed = true;
        getty_supervise();
        return;
    }
    getty_session();
}
