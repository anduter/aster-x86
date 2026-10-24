/* =============================================================================
 * core/supervisor.c - 内建 supervisor 服务
 *
 * 服务循环：accept 一个 client channel，读取服务名，查询 registry，
 * 回复 "yes"/"no"。这是一个最小可运行的 supervisor，不负责重启策略。
 * ============================================================================= */
#include "supervisor.h"
#include "service.h"
#include "../ipc/port.h"
#include "../ipc/channel.h"
#include "../proc/proc.h"
#include "../proc/proc_internal.h"
#include "../apps/echosvc.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

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

}

void supervisor_entry(void)
{
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
        } else if (strcmp(name, "restart-echo") == 0) {
            echosvc_request_stop();
            proc_sleep_ticks(20U);
            if (!service_exists("echo")) {
                proc_t *p = proc_spawn("echosvc");
                if (p != 0) {
                    kprintf("supervisor: restart echo -> pid %u\n", p->pid);
                    for (uint32_t w = 0; w < 100U && !service_exists("echo"); w++) {
                        proc_sleep_ticks(1U);
                    }
                    (void)chan_send(server, service_exists("echo") ? "ok" : "fail",
                                    service_exists("echo") ? 2U : 4U);
                } else {
                    (void)chan_send(server, "fail", 4U);
                }
            } else {
                (void)chan_send(server, "fail", 4U);
            }
        } else {
            kprintf("supervisor: query %s -> %s\n", name, ok ? "yes" : "no");
            (void)chan_send(server, ok ? "yes" : "no", ok ? 3U : 2U);
        }
        chan_release(server);
    }
}
