/* =============================================================================
 * apps/heartbeat.c - 心跳线程（一个程序）
 *
 * 作用：
 *   1. 证明调度器在多个内核线程之间切换；
 *   2. 在**屏幕右下角**画一个心跳小部件（状态栏右侧），每秒跳动一次；
 *   3. 同时在串口留一行纯文本日志，供自动测试断言。
 * ============================================================================= */
#include "heartbeat.h"
#include "../proc/proc.h"
#include "../core/percpu.h"
#include "../intr/interrupt.h"
#include "../console.h"
#include "../io.h"
#include "../lib/kprintf.h"

#define HEARTBEAT_PERIOD 100U   /* tick 数，100 Hz 下约 1 秒 */

void heartbeat_entry(void)
{
    uint32_t n = 0;

    for (;;) {
        proc_t *me = proc_current();
        char widget[24];

        /* 串口日志（屏幕不滚动输出，只在右下角显示小部件） */
        serial_puts("thread: heartbeat ");
        serial_putdec(n);
        serial_puts(" pid=");
        serial_putdec((me != 0) ? me->pid : 0U);
        serial_puts(" cpu=");
        serial_putdec(smp_cpu_id());
        serial_puts("\r\n");

        /* 右下角心跳：计数 + 交替的跳动符号 */
        ksnprintf(widget, sizeof(widget), "hb %u %s", n, ((n & 1U) != 0U) ? "*" : ".");
        console_set_status_right(widget);

        n++;

        /* 等到下一个周期：期间不断让出 CPU，避免忙等占满时间片 */
        unsigned int start = timer_ticks();
        while ((timer_ticks() - start) < HEARTBEAT_PERIOD) {
            proc_yield();
            __asm__ volatile ("hlt");   /* 让 CPU 睡到下一个 tick */
        }
    }
}
