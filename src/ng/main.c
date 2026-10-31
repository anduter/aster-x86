/*
 * ng WP1-2: 64 位 C 入口。
 *
 * 到这里时 entry.S 已完成：长模式、分页（引导用 1 GiB 2 MiB 恒等映射）、
 * 64 位段与启动栈。
 *
 * 本 WP 做的事：
 *   1. 串口先就位（无显示环境下唯一的证据通道）；
 *   2. pg_init() 建 4 级页表（4 KiB 内核段 + 2 MiB 大页混合）、切 CR3、
 *      置 EFER.NXE / CR4.SMEP/SMAP / CR0.WP，并做不依赖 trap 的自检；
 *   3. 打印最终 PASS/FAIL 后 hlt 停住（trap/IDT 留给 WP1-3）。
 */
#include "paging.h"

#include "serial.h"
#include "types.h"

void kmain(u64 mb_info)
{
    int fails;

    (void)mb_info; /* multiboot2 / PVH 信息留给 WP1-4（PMM 需要内存图） */

    serial_init();
    serial_puts("ng: hello 64-bit\r\n");

    fails = pg_init();

    serial_puts("ng: WP1-2 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    for (;;) {
        __asm__ volatile("hlt");
    }
}
