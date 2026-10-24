#include "svcwatch.h"
#include "../core/service.h"
#include "../proc/proc.h"
#include "../lib/kprintf.h"

void svcwatch_entry(void)
{
    for (;;) {
        if (!service_exists("echo")) {
            proc_t *p = proc_spawn("echosvc");
            if (p != 0) {
                kprintf("svcwatch: restart echo -> pid %u\n", p->pid);
            } else {
                kprintln("svcwatch: restart echo failed");
            }
        }
        proc_sleep_ticks(20U);
    }
}
