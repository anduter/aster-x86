/* =============================================================================
 * core/kernel.c - 内核主入口（薄层）
 *
 * 只做最早期的输出初始化（串口/framebuffer/console），然后把控制权交给
 * bootseq.c 的步骤注册表；最后启动调度器进入用户态。
 * 具体子系统的初始化顺序见 core/bootseq.c 的 boot_steps[]。
 * ============================================================================= */
#include "../types.h"
#include "arch/x86/io.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/proc.h"

void kernel_main(void)
{
    __asm__ volatile("cli");

    /* ---------- 0/1. 最早期的输出：串口 -> framebuffer -> 控制台 ---------- */
    serial_init();
    fb_init();
    boot_splash();
    console_init();
    console_set_title(OS_NAME " v" OS_VERSION "  -  32-bit protected mode");
    console_set_status("booting ...");
    kprintln("boot: graphical splash shown");

    kprintln("kernel: " OS_NAME " v" OS_VERSION " (build " OS_BUILD ")");
    kprintf("fb: %ux%u %ubpp pitch=%u lfb=%p\n",
            (unsigned int)fb_w, (unsigned int)fb_h, (unsigned int)fb_bpp,
            fb_pitch, (void *)(uintptr_t)fb_addr);
    kprintf("console: %ux%u cells\n",
            (unsigned int)console_cols(), (unsigned int)console_rows());

    /* ---------- 2. 子系统初始化（注册表驱动） ---------- */
    kernel_boot();

    /* ---------- 3. 进入 ring3 ---------- */
    console_set_status("ready - ring3 shell");
    sched_start();

    panic("scheduler returned unexpectedly");
}
