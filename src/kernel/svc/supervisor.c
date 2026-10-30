/* =============================================================================
 * core/supervisor.c - 内建 supervisor 服务
 *
 * 服务循环：accept 一个 client channel，读取服务名，查询 registry，
 * 回复 "yes"/"no"。这是一个最小可运行的 supervisor，不负责重启策略。
 * ============================================================================= */
#include "supervisor.h"
#include "service.h"
#include "include/svc.h"
#include "include/svc.h"
#include "include/proc.h"
#include "include/proc.h"
#include "include/apps.h"
#include "include/kernel.h"
#include "include/kernel.h"

static int sup_port = -1;

void supervisor_init(void)
{
    sup_port = port_create();
    if (sup_port < 0) {
        kprintln("supervisor: no port");
        return;
    }
    (void)service_register("supervisor", (uint32_t)sup_port);
    if (proc_spawn("supervisor") == 0) {
        kprintln("supervisor: spawn failed");
    }
    if (proc_spawn("echosvc") == 0) {
        kprintln("supervisor: echosvc spawn failed");
    }
    if (proc_spawn("svcwatch") == 0) {
        kprintln("supervisor: svcwatch spawn failed");
    }

}

void supervisor_entry(void)
{
    /* P1 IPC 自检：必须在调度器启动后的进程上下文里跑（要阻塞/超时/被信号
     * 打断）；service_lifecycle_selftest 在 sched_start 之前，做不到这些。
     * 只在 -Test 构建打印，避免生产启动多花 ~500ms。 */
    if (boot_test_build()) {
        ipc_selftest();
    }

    for (;;) {
        uint32_t server = 0;
        char name[32];
        int n;
        int ok;

        if (port_accept_block((uint32_t)sup_port, &server) != 0) {
            continue;
        }
        n = chan_recv_block(server, name, sizeof(name) - 1U);
        if (n <= 0) {
            chan_release(server);
            continue;
        }
        name[n] = '\0';
        ok = service_exists(name);
        if (strcmp(name, "manifest") == 0) {
            ok = service_exists("supervisor") && service_exists("echo");
            kprintf("supervisor: manifest -> %s\n", ok ? "ok" : "missing");
            (void)chan_send(server, ok ? "ok" : "missing", ok ? 2U : 7U);
        } else if (strncmp(name, "start:", 6) == 0) {
            /* 组件安装后由 _pkgd 通知启动：按程序名拉起（程序表里的名字）。
             * echosvc 已经在线时按"已在运行"处理，不重复拉起。 */
            const char *prog = name + 6;
            int started = 1;

            if (strcmp(prog, "echosvc") == 0 && service_exists("echo")) {
                kprintf("supervisor: start %s already running\n", prog);
            } else {
                proc_t *p = proc_spawn(prog);
                started = (p != 0) ? 1 : 0;
                kprintf("supervisor: start %s -> %s\n", prog,
                        started ? "ok" : "failed");
            }
            (void)chan_send(server, started ? "ok" : "no", started ? 2U : 2U);
        } else if (strncmp(name, "stop:", 5) == 0) {
            /* 卸载运行中的组件：先停服务，等注册表里消失再让 _pkgd 删文件。 */
            const char *prog = name + 5;

            if (strcmp(prog, "echosvc") == 0) {
                echosvc_request_stop();
                for (uint32_t w = 0; w < 100U && service_exists("echo"); w++) {
                    proc_sleep_ticks(1U);
                }
            }
            ok = service_exists("echo") ? 0 : 1;
            kprintf("supervisor: stop %s -> %s\n", prog, ok ? "ok" : "busy");
            (void)chan_send(server, ok ? "ok" : "no", ok ? 2U : 2U);
        } else if (strcmp(name, "restart-echo") == 0) {
            /* 只负责停；先等旧服务注销，再让客户端去等 svcwatch 拉起的新服务。 */
            echosvc_request_stop();
            for (uint32_t w = 0; w < 100U && service_exists("echo"); w++) {
                proc_sleep_ticks(1U);
            }
            (void)chan_send(server, "ok", 2U);
        } else {
            kprintf("supervisor: query %s -> %s\n", name, ok ? "yes" : "no");
            (void)chan_send(server, ok ? "yes" : "no", ok ? 3U : 2U);
        }
        chan_release(server);
    }
}
