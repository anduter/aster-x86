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
}

void supervisor_entry(void)
{
    uint32_t cooldown = 0;

    for (;;) {
        uint32_t server = 0;
        char name[32];
        int n;
        int ok;

        /* 非阻塞 accept：服务请求和 watchdog 同一循环，避免服务线程永远阻塞。 */
        if (port_accept((uint32_t)sup_port, &server) == 0) {
            n = chan_recv_block(server, name, sizeof(name) - 1U);
            if (n <= 0) {
                chan_release(server);
            } else {
                name[n] = '\0';
                ok = service_exists(name);
                if (strcmp(name, "manifest") == 0) {
                    ok = service_exists("supervisor") && service_exists("echo");
                    kprintf("supervisor: manifest -> %s\n", ok ? "ok" : "missing");
                    (void)chan_send(server, ok ? "ok" : "missing", ok ? 2U : 7U);
                } else {
                    kprintf("supervisor: query %s -> %s\n", name, ok ? "yes" : "no");
                    (void)chan_send(server, ok ? "yes" : "no", ok ? 3U : 2U);
                }
                chan_release(server);
            }
        }

        /* 自动恢复策略：echo 服务缺失就拉起来，冷却窗口避免重复 spawn。 */
        if (cooldown > 0U) cooldown--;
        if (!service_exists("echo") && cooldown == 0U) {
            proc_t *p = proc_spawn("echosvc");
            cooldown = 20U;
            if (p != 0) {
                kprintf("supervisor: restart echo -> pid %u\n", p->pid);
            } else {
                kprintln("supervisor: restart echo failed");
            }
        }

        proc_sleep_ticks(10U);
    }
}
