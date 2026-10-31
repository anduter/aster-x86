/*
 * ng WP1-3: 64 位 C 入口（页表 -> GDT/TSS -> IDT -> syscall -> 出口自检）。
 *
 * 顺序是有依赖的：
 *   1. pg_init() 先建 4 级页表/W^X/NX/SMEP/SMAP（WP1-2）；
 *   2. gdt_init() 换掉 entry.S 的引导 GDT，装上用户段与 TSS/IST；
 *   3. trap_init() 填 256 项 IDT 并 lidt（#DF 用 IST1）；
 *   4. syscall_init() 置 EFER.SCE、STAR/LSTAR/FMASK；
 *   5. 受控异常（写只读页/ud2/坏段）与 syscall（ring0、ring3）自检。
 *
 * 想验证 panic 路径：build-ng.ps1 -PanicTest 会在所有正常检查后触发一条
 * 不可恢复的 #UD（普通 build 不定义 NG_PANIC_TEST，行为不变）。
 */
#include "gdt.h"
#include "paging.h"
#include "serial.h"
#include "syscall.h"
#include "trap.h"
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

    if (fails == 0) {
        gdt_init();
        fails += trap_init();
        syscall_init();
        fails += trap_selftest();
        fails += syscall_selftest();
    }

#ifdef NG_PANIC_TEST
    serial_puts("ng: panic selftest: triggering unrecoverable #UD\r\n");
    __asm__ volatile("ud2" : : : "memory");
#endif

    serial_puts("ng: WP1-3 selftest ");
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
