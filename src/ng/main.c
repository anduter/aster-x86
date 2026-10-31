/*
 * ng WP1-3/1-4/1-5/1-6/1-7: 64 位 C 入口（内存图/PMM -> 页表 -> GDT/TSS ->
 * IDT -> syscall -> 内存/heap 自检 -> 线程/调度自检 -> IPC/句柄自检 ->
 * ELF64 加载器 + ring3 hello）。
 *
 * 顺序是有依赖的：
 *   1. mem_boot_init()：解析 PVH/multiboot2 内存图、初始化 PMM、保留内核
 *      镜像/低 1 MiB/引导结构，并跑 PMM 自检（WP1-4）；
 *   2. pg_init() 建 4 级页表/W^X/NX/SMEP/SMAP，页表页从 PMM 申请（WP1-2/1-4）；
 *   3. gdt_init() 换掉 entry.S 的引导 GDT，装上用户段与 TSS/IST；
 *   4. trap_init() 填 256 项 IDT；syscall_init() 置 EFER.SCE、STAR/LSTAR/FMASK；
 *   5. 受控异常与 syscall（ring0、ring3）自检（WP1-3）；
 *   6. heap 与“页表池来自 PMM”自检（WP1-4）；
 *   7. 线程/上下文切换/round-robin/exit 回收/sleep-wake 自检（WP1-5，栈从
 *      heap 分配，所以必须排在 WP1-4 之后）；
 *   8. IPC/句柄/端口自检（WP1-6，需要调度器的阻塞/唤醒，排在 WP1-5 之后）；
 *   9. VFS/ramfs 自检（WP2-1：根 ramfs + /tmp，数据页来自 PMM；ring3 demo
 *      要调 20..27，所以排在 WP1-7 之前）；
 *  10. FAT16 自检（WP2-2：嵌入镜像 -> PMM RAM 盘 -> /mnt/fat；ring3 demo
 *      读 HELLO.TXT / 建 NEW.TXT，所以必须排在 WP1-7 之前）；
 *  11. 块设备自检（WP2-4：PCI PIIX3 + ATA PIO -> 真实 disk.img 上的 FAT16
 *      挂到 /mnt/disk；ring3 demo 读 HELLO.TXT / 写 DISK.TXT，所以排在
 *      WP1-7 之前）；
 *  12. ELF64 加载器 + ring3 hello（WP1-7：PMM 页、用户地址空间、CR3
 *      切换、copyin/copyout、per-thread 内核栈；排在 WP1-6/2-1 之后）。
 *
 * ipc_init() 不分配内存，只把端口表/句柄表清零并绑定环境钩子，所以放在
 * 最前面；真正创建端口在自检里（heap 已就绪）。
 *
 * 想验证 panic 路径：build-ng.ps1 -PanicTest 会在所有正常检查后触发一条
 * 不可恢复的 #UD（普通 build 不定义 NG_PANIC_TEST，行为不变）。
 */
#include "elf_selftest.h"
#include "fat16_selftest.h"
#include "fs_selftest.h"
#include "gdt.h"
#include "ipc.h"
#include "ipc_selftest.h"
#include "mem_selftest.h"
#include "paging.h"
#include "sched_selftest.h"
#include "serial.h"
#include "syscall.h"
#include "trap.h"
#include "types.h"
#include "wp23.h"
#include "wp24.h"

void kmain(u64 mb_info)
{
    int mem_fails;
    int late_fails;
    int fails;

    serial_init();
    serial_puts("ng: hello 64-bit\r\n");
    ipc_init();

    /* WP1-4 前半：内存图 + PMM。必须在建页表之前（页表页从 PMM 来）。 */
    mem_fails = mem_boot_init(mb_info);

    fails = mem_fails;
    fails += pg_init();

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

    /* WP1-4 后半：heap 与页表池。PMM 没起来就跳过（避免用 0 地址扩堆）。 */
    late_fails = 0;
    if (mem_fails == 0) {
        late_fails += mem_heap_selftest();
        late_fails += mem_pgtbl_selftest();
    } else {
        late_fails = 1;
        serial_puts("ng: WP1-4 heap/pgtbl skipped (memory init failed)\r\n");
    }
    fails += late_fails;

    serial_puts("ng: WP1-4 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    /* WP1-5：线程模型 / 上下文切换 / 调度器。依赖 heap（栈）与 PMM，且
     * 调度点全是显式调用（协作式），前面有失败就不冒险进调度。 */
    if (mem_fails == 0 && fails == 0) {
        fails += sched_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP1-5 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP1-5 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    /* WP1-6：句柄表 + 端口 + 阻塞/超时/close 唤醒 + 用户指针校验。
     * 需要 WP1-5 的调度器（阻塞用 thread_sleep，唤醒用 thread_wake）。 */
    if (mem_fails == 0 && fails == 0) {
        fails += ipc_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP1-6 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP1-6 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    /* WP2-1：VFS 核心 + ramfs。数据页走 PMM（fs_env 注入），不需要 heap；
     * 排在 ELF64/ring3 之前，因为 ring3 demo 要调 20..27 的 fs syscall。 */
    if (mem_fails == 0 && fails == 0) {
        fails += fs_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP2-1 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP2-1 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    /* WP2-2：嵌入的 1.44MB FAT16 镜像 -> PMM RAM 盘 -> /mnt/fat。ring3 demo
     * 要读 /mnt/fat/HELLO.TXT、建 NEW.TXT，所以排在 ELF64/ring3 之前。 */
    if (mem_fails == 0 && fails == 0) {
        fails += fat16_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP2-2 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP2-2 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    /* WP2-4：ATA PIO + 真实磁盘镜像上的 FAT16（/mnt/disk）。ring3 demo 要读
     * HELLO.TXT、写 DISK.TXT，所以排在 ELF64/ring3 之前。 */
    if (mem_fails == 0 && fails == 0) {
        fails += wp24_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP2-4 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP2-4 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    /* WP2-3：PCI/VBE/帧缓冲控制台 + PS/2 键盘。显示不可用时回退 VGA 文本；
     * 键盘/输入自检独立于显示。ring3 demo 的 readkey 依赖它，所以排在
     * WP1-7 之前。 */
    if (mem_fails == 0 && fails == 0) {
        fails += wp23_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP2-3 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP2-3 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    /* WP1-7：ELF64 加载器 + ring3 hello。需要 PMM/heap/调度器（per-thread
     * syscall 内核栈用一个真实线程验证），所以排在最后。 */
    if (mem_fails == 0 && fails == 0) {
        fails += elf_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP1-7 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP1-7 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }

    for (;;) {
        /* idle 点轮询 PS/2：没有 IRQ 时也不会把键盘字节丢在控制器里。 */
        ng_input_poll();
        __asm__ volatile("hlt");
    }
}
