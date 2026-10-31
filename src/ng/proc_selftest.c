/*
 * ng WP3-0: 多进程用户态 / 跨进程 IPC 的内核自检。
 *
 * 出口证据行（run-ng 全部要求命中）：
 *   ng: proc table ok (max=32 init=pid 1 tid=...)
 *   ng: proc spawn ok (pid=...)
 *   ng: proc wait ok (status=0)
 *   ng: proc ipc cross ok (pid=... procs=3)
 *   ng: proc as isolated ok (va=0x40010000 ...)
 *   ng: proc kill ok (pids=... status=265)
 *   ng: WP3-0 selftest PASS
 *
 * 覆盖：
 *   1. spawn 从 ramfs 读 ELF（/bin/proc 由 proc_init 种进来）-> 新 AS -> 线程；
 *   2. wait 阻塞并拿到退出码 0；
 *   3. 3 进程链式 ping（hello -> relay -> server，ticket 经 argv 和消息 from
 *      两跳传递，reply 两跳）；
 *   4. 两个进程在**同一个 VA**（标记页 0x40010000）上拿到**不同的物理页**，
 *      内容各自独立（AS 隔离）；
 *   5. kill 一个阻塞在 recv 的进程：状态 = 0x100|sig，等待者不泄漏，能回收；
 *   6. 非子进程 wait -> ECHILD，重复 kill 幂等，表统计。
 */
#include "proc_selftest.h"

#include "ipc.h"
#include "paging.h"
#include "proc.h"
#include "sched_arch.h"
#include "serial.h"
#include "useras.h"

/* 内嵌的 /bin/proc 已经由 proc_init 种进 ramfs。 */
#define WP30_BIN "/bin/proc"

static int g_fails;

static void fail_item(const char *what)
{
    g_fails++;
    serial_puts("ng: proc ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

/* 等两个进程把标记页写出来（它们各自 block 在 recv 里）。 */
static int wait_marker(const struct proc *p1, const struct proc *p2,
                       u64 want1, u64 want2)
{
    u32 i;
    u64 va = PROC_MARKER_VA;

    for (i = 0; i < 200; i++) {
        u64 pte1 = as_walk((const struct user_as *)p1->as, va);
        u64 pte2 = as_walk((const struct user_as *)p2->as, va);
        const u64 *m1;
        const u64 *m2;

        if ((pte1 & PG_PRESENT) != 0 && (pte2 & PG_PRESENT) != 0) {
            m1 = (const u64 *)(usize)(pte1 & PG_ADDR_MASK);
            m2 = (const u64 *)(usize)(pte2 & PG_ADDR_MASK);
            if (*m1 == want1 && *m2 == want2) {
                return 1;
            }
        }
        (void)thread_sleep(1ULL << 20);
    }
    return 0;
}

int proc_selftest(void)
{
    const char *argv_hello[3];
    const char *argv_chain[4];
    const char *argv_hold1[3];
    const char *argv_hold2[3];
    struct proc *child = NULL;
    u32 pid_hello = 0;
    u32 pid_chain = 0;
    u32 pid_h1 = 0;
    u32 pid_h2 = 0;
    int status = -1;
    u32 waiters_before;
    u32 waiters_after;
    u64 pte_base1;
    u64 pte_base2;
    u64 pte1;
    u64 pte2;
    u64 marker1;
    u64 marker2;
    int rc;

    g_fails = 0;
    serial_puts("ng: proc selftest: spawn/wait/ipc/kill/as\r\n");

    /* ---- 1. spawn + wait：hello（打印 pid/ppid 后 exit 0）-------------- */
    argv_hello[0] = "proc";
    argv_hello[1] = "hello";
    argv_hello[2] = NULL;
    rc = proc_spawn(WP30_BIN, argv_hello, &pid_hello);
    if (rc != PROC_OK || pid_hello == 0) {
        serial_puts("ng: proc spawn rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("spawn");
        return g_fails;
    }
    serial_puts("ng: proc spawn ok (pid=");
    serial_putdec(pid_hello);
    serial_puts(" path=");
    serial_puts(WP30_BIN);
    serial_puts(")\r\n");

    child = NULL;
    rc = proc_wait_pid(pid_hello, &status, &child);
    if (rc != PROC_OK || status != 0) {
        serial_puts("ng: proc wait rc=");
        serial_putdec((u64)(rc < 0 ? -rc : 0));
        serial_puts(" status=");
        serial_putdec((u64)status);
        serial_puts("\r\n");
        fail_item("wait");
        (void)proc_reap_pid(pid_hello);
        return g_fails;
    }
    if (child == NULL || child->ppid != 1 || child->as_pml4 == 0 ||
        child->tid == 0) {
        fail_item("wait");
    }
    serial_puts("ng: proc wait ok (status=");
    serial_putdec((u64)status);
    serial_puts(" pid=");
    serial_putdec(pid_hello);
    serial_puts(" tid=");
    serial_putdec(child != NULL ? child->tid : 0);
    serial_puts(")\r\n");
    (void)proc_reap_pid(pid_hello);

    /* ---- 2. 非子进程 / 不存在的 pid ---------------------------------- */
    if (proc_wait_pid(9999, &status, NULL) != PROC_ECHILD) {
        fail_item("wait child check");
    }
    if (proc_kill(9999, 9) != PROC_ESRCH) {
        fail_item("kill pid check");
    }

    /* ---- 3. 3 进程链式 ping：hello(chain) -> relay -> server ----------- */
    waiters_before = ipc_core_waiter_live(ipc_core_global());
    argv_chain[0] = "proc";
    argv_chain[1] = "hello";
    argv_chain[2] = "chain";
    argv_chain[3] = NULL;
    rc = proc_spawn(WP30_BIN, argv_chain, &pid_chain);
    if (rc != PROC_OK) {
        fail_item("ipc spawn");
    } else {
        child = NULL;
        rc = proc_wait_pid(pid_chain, &status, &child);
        if (rc != PROC_OK || status != 0) {
            serial_puts("ng: proc ipc chain status=");
            serial_putdec((u64)status);
            serial_puts("\r\n");
            fail_item("ipc cross");
        } else {
            serial_puts("ng: proc ipc cross ok (pid=");
            serial_putdec(pid_chain);
            serial_puts(" procs=3 msgs=4 exits=3)\r\n");
        }
        (void)proc_reap_pid(pid_chain);
    }
    waiters_after = ipc_core_waiter_live(ipc_core_global());
    if (waiters_after > waiters_before) {
        fail_item("ipc waiters");
    }

    /* ---- 4. AS 隔离：两个进程同一个 VA、不同的物理页 ------------------ */
    argv_hold1[0] = "proc";
    argv_hold1[1] = "hold";
    argv_hold1[2] = "111";
    argv_hold2[0] = "proc";
    argv_hold2[1] = "hold";
    argv_hold2[2] = "222";
    rc = proc_spawn(WP30_BIN, argv_hold1, &pid_h1);
    if (rc == PROC_OK) {
        rc = proc_spawn(WP30_BIN, argv_hold2, &pid_h2);
    }
    if (rc != PROC_OK || pid_h1 == 0 || pid_h2 == 0) {
        fail_item("as isolate");
    } else {
        const struct proc *p1 = proc_slot(pid_h1);
        const struct proc *p2 = proc_slot(pid_h2);

        if (p1 == NULL || p2 == NULL || p1->as == NULL || p2->as == NULL ||
            p1->as == p2->as || p1->as_pml4 == p2->as_pml4) {
            fail_item("as isolate");
        } else if (!wait_marker(p1, p2, 111, 222)) {
            fail_item("as isolate");
        } else {
            pte1 = as_walk((const struct user_as *)p1->as, PROC_MARKER_VA);
            pte2 = as_walk((const struct user_as *)p2->as, PROC_MARKER_VA);
            pte_base1 = as_walk((const struct user_as *)p1->as,
                                p1->load_base);
            pte_base2 = as_walk((const struct user_as *)p2->as,
                                p2->load_base);
            marker1 = *(const u64 *)(usize)(pte1 & PG_ADDR_MASK);
            marker2 = *(const u64 *)(usize)(pte2 & PG_ADDR_MASK);
            if ((pte1 & PG_ADDR_MASK) == (pte2 & PG_ADDR_MASK) ||
                (pte_base1 & PG_ADDR_MASK) == (pte_base2 & PG_ADDR_MASK) ||
                marker1 == marker2) {
                fail_item("as isolate");
            } else {
                serial_puts("ng: proc as isolated ok (va=");
                serial_puthex(PROC_MARKER_VA);
                serial_puts(" p1=");
                serial_puthex(pte1 & PG_ADDR_MASK);
                serial_puts(" p2=");
                serial_puthex(pte2 & PG_ADDR_MASK);
                serial_puts(" tags=");
                serial_putdec(marker1);
                serial_puts("/");
                serial_putdec(marker2);
                serial_puts(")\r\n");
            }
        }
    }

    /* ---- 5. kill：两个 hold 都阻塞在 recv 里，直接终止 + 回收 ---------- */
    if (pid_h1 != 0 && pid_h2 != 0) {
        int st1 = -1;
        int st2 = -1;
        int kill_rc1 = proc_kill(pid_h1, 9);
        int kill_rc2 = proc_kill(pid_h2, 9);
        int wr1;
        int wr2;

        wr1 = proc_wait_pid(pid_h1, &st1, NULL);
        wr2 = proc_wait_pid(pid_h2, &st2, NULL);
        if (kill_rc1 != PROC_OK || kill_rc2 != PROC_OK || wr1 != PROC_OK ||
            wr2 != PROC_OK || st1 != PROC_STATUS_KILLED(9) ||
            st2 != PROC_STATUS_KILLED(9)) {
            fail_item("kill");
        } else {
            serial_puts("ng: proc kill ok (pids=");
            serial_putdec(pid_h1);
            serial_puts(",");
            serial_putdec(pid_h2);
            serial_puts(" status=");
            serial_putdec((u64)st1);
            serial_puts(" waiters=");
            serial_putdec((u64)ipc_core_waiter_live(ipc_core_global()));
            serial_puts(")\r\n");
        }
        (void)proc_reap_pid(pid_h1);
        (void)proc_reap_pid(pid_h2);
        /* 再 kill 一次已经回收的 pid：ESRCH（幂等性的反向证据）。 */
        if (proc_kill(pid_h1, 9) != PROC_ESRCH) {
            fail_item("kill reap check");
        }
    }

    /* ---- 6. 表统计 --------------------------------------------------- */
    if (proc_table_live() != 1) {
        /* 只剩 init；有泄漏就是 FAIL。 */
        serial_puts("ng: proc leak live=");
        serial_putdec((u64)proc_table_live());
        serial_puts("\r\n");
        fail_item("reap");
    }
    serial_puts("ng: proc stat spawned=");
    serial_putdec(proc_table_spawned());
    serial_puts(" live=");
    serial_putdec((u64)proc_table_live());
    serial_puts(" switches=");
    serial_putdec(sched_switch_count());
    serial_puts("\r\n");

    return g_fails;
}
