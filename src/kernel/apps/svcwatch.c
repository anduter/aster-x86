/* =============================================================================
 * apps/svcwatch.c - 服务监督者（P3：注册表生命周期引擎的驱动线程）
 *
 * 两个职责：
 *   1. 新注册表（SYS_SVC_REGISTER 登记的服务）：每次 tick 驱动
 *      svc_supervise_tick()，处理心跳超时/unhealthy、进程消失、退避重启
 *      （1s/2s/4s，超过 restart_limit 隔离）。“重启后 lookup 返回新 port、
 *      旧 client 必须重新 lookup”的语义在 svc/registry.c 里。
 *   2. 旧注册表回退：按名字盯着 echo（组件包安装/卸载流程依赖它），
 *      保持 0.74.2 的 svcwatch: restart echo -> pid N 行为不变。
 * ============================================================================= */
#include "svcwatch.h"
#include "include/svc.h"
#include "include/proc.h"
#include "include/kernel.h"

/* 轮询周期（tick=10ms）：200ms 足够让退避粒度（1s/2s/4s）不抖动。 */
#define SVCWATCH_PERIOD_TICKS 20U
#define SVCWATCH_START_GRACE  50U

void svcwatch_entry(void)
{
    /* 启动 grace：给 boot 时的 echosvc 时间注册。 */
    proc_sleep_ticks(SVCWATCH_START_GRACE);

    for (;;) {
        /* 注册表生命周期：心跳超时标记 / 退避重启 / 隔离。 */
        svc_supervise_tick(timer_ticks());

        /* 旧注册表回退：echo 组件（不经过 SYS_SVC_* 表）。 */
        if (!service_exists("echo")) {
            /* 二次确认，避免停止/重建窗口里的误判。 */
            proc_sleep_ticks(SVCWATCH_PERIOD_TICKS);
            if (!service_exists("echo")) {
                proc_t *p = proc_spawn("echosvc");
                if (p != 0) {
                    kprintf("svcwatch: restart echo -> pid %u\n", p->pid);
                } else {
                    kprintln("svcwatch: restart echo failed");
                }
            }
        }
        proc_sleep_ticks(SVCWATCH_PERIOD_TICKS);
    }
}
