#include "svcwatch.h"
#include "../core/service.h"
#include "../proc/proc.h"
#include "../lib/kprintf.h"

void svcwatch_entry(void)
{
    /* 启动 grace：给 boot 时的 echosvc 时间注册。 */
    proc_sleep_ticks(50U);

    for (;;) {
        if (!service_exists("echo")) {
            /* 二次确认，避免停止/重建窗口里的误判。 */
            proc_sleep_ticks(20U);
            if (!service_exists("echo")) {
                proc_t *p = proc_spawn("echosvc");
                if (p != 0) {
                    kprintf("svcwatch: restart echo -> pid %u\n", p->pid);
                } else {
                    kprintln("svcwatch: restart echo failed");
                }
            }
        }
        proc_sleep_ticks(20U);
    }
}
