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
 *      切换、copyin/copyout、per-thread 内核栈；排在 WP1-6/2-1 之后）；
 *  13. 用户态 shell（WP2-6：wp26_selftest 加载嵌入的 shell ELF，切 CR3 进
 *      ring3，交互到 exit/Ctrl-D；`build-ng.ps1 -Smoke` 跳过它）。
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
#include "log.h"
#include "mem_selftest.h"
#include "paging.h"
#include "parity_selftest.h"
#include "proc.h"
#include "proc_selftest.h"
#include "sched_selftest.h"
#include "serial.h"
#include "shm.h"
#include "syscall.h"
#include "trap.h"
#include "types.h"
#include "wp23.h"
#include "wp24.h"
#include "wp25.h"
#include "wp26.h"
#include "wp31.h"

/* WP2-5：把每个 WP 的 PASS/FAIL 写进 64 KiB log 环（启动关键路径证据）。 */
static void log_wp(const char *wp, int fails)
{
    char msg[32];
    const char *tail = (fails == 0) ? "PASS" : "FAIL";
    u32 i = 0;
    u32 j = 0;

    while (wp[i] != '\0' && i < 20u) {
        msg[i] = wp[i];
        i++;
    }
    msg[i++] = ' ';
    while (tail[j] != '\0' && i + 1u < (u32)sizeof(msg)) {
        msg[i++] = tail[j++];
    }
    msg[i] = '\0';
    (void)log_write("selftest", msg);
}

void kmain(u64 mb_info)
{
    int mem_fails;
    int late_fails;
    int fails;

    serial_init();
    (void)log_init();
    serial_puts("ng: hello 64-bit\r\n");
    (void)log_write("boot", "ng: hello 64-bit");
    ipc_init();
    /*
     * WP3-1：共享内存对象表（数据面）。只清表 + 绑 PMM 环境钩子，不分配页；
     * 真正分配在 ring3 的 SYS_SHM_ALLOC（displayd 建 surface 时）。
     */
    shm_init();

    /* WP1-4 前半：内存图 + PMM。必须在建页表之前（页表页从 PMM 来）。 */
    mem_fails = mem_boot_init(mb_info);

    fails = mem_fails;
    fails += pg_init();
    (void)log_write("boot", "paging 4-level ok");

    serial_puts("ng: WP1-2 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    log_wp("WP1-2", fails);

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
    log_wp("WP1-3", fails);

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
    log_wp("WP1-4", fails);

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
    log_wp("WP1-5", fails);

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
    log_wp("WP1-6", fails);

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
    log_wp("WP2-1", fails);

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
    log_wp("WP2-2", fails);

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
    log_wp("WP2-4", fails);

    /* WP2-5：auth/log/pkg（L1）。auth_init 往 ramfs 种 /etc/passwd，pkg_init
     * 种 /System/Packages/hello/1.0.0；31..35 的成功路径用一页临时 U=1 映射
     * 走真实 uaccess。它不依赖显示/输入，但排在 ring3 前（ring3 svc demo 用）。 */
    if (mem_fails == 0 && fails == 0) {
        fails += wp25_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP2-5 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP2-5 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    log_wp("WP2-5", fails);

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
    log_wp("WP2-3", fails);

    /* WP2-7：parity 100 项（E2 出口）。排在所有服务/驱动自检之后、ring3
     * demo 之前：用例会重建 auth/pkg/log 状态（幂等；auth 重新 init 也会
     * 解锁用户，保证后面的 ring3 svc demo 仍能登录）。失败不 panic。 */
    if (mem_fails == 0 && fails == 0) {
        fails += parity_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP2-7 parity skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP2-7 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    log_wp("WP2-7", fails);

    /* WP1-7：ELF64 加载器 + ring3 hello。需要 PMM/heap/调度器（per-thread
     * syscall 内核栈用一个真实线程验证），所以排在最后。 */
    if (mem_fails == 0 && fails == 0) {
        /* WP3-0：进程表初始化（pid 1 = 内核 init；把 /bin/shell、/bin/proc、
         * /bin/hello 种进 ramfs）。必须在 WP1-7/WP2-6 之前，后两者要用它。 */
        proc_init();
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
    log_wp("WP1-7", fails);

    /* WP3-0：多进程用户态 + 跨进程 IPC（spawn/wait/getpid/kill/getppid、
     * 每进程句柄表、ticket 跨进程、AS 隔离）。依赖 WP1-7 的 ELF/用户 AS、
     * WP2-1 的 ramfs（/bin）、WP1-5 的调度器（wait 阻塞）。 */
    if (mem_fails == 0 && fails == 0) {
        fails += proc_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP3-0 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP3-0 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    log_wp("WP3-0", fails);

    /* WP3-1：display server（L2a，ring3 进程 /bin/displayd）+ demo（L3）。
     * 依赖 WP2-3 的 VBE/LFB/PS2、WP3-0 的进程/spawn/IPC、WP1-7 的用户 AS，
     * 以及本 WP 新加的 syscall 45/46/47（共享内存 + LFB 用户映射）。
     * 没有 LFB（-vga none 降级）时打印 skipped 并跳过，不算失败。 */
    if (mem_fails == 0 && fails == 0) {
        fails += wp31_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP3-1 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP3-1 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    log_wp("WP3-1", fails);

    /* WP2-6：用户态 shell（L3）。默认启动顺序：所有自检 -> ring3 hello ->
     * shell（交互，直到 exit/Ctrl-D）。`build-ng.ps1 -Smoke` 编出的内核跳过
     * shell，只保留 ring3 hello 冒烟路径（run-ng -Smoke）。 */
#if defined(NG_SMOKE_MODE)
    serial_puts("ng: shell skipped (smoke mode)\r\n");
    (void)log_write("selftest", "WP2-6 skipped (smoke mode)");
#else
    if (mem_fails == 0 && fails == 0) {
        fails += wp26_selftest();
    } else {
        fails += 1;
        serial_puts("ng: WP2-6 selftest skipped (earlier failure)\r\n");
    }

    serial_puts("ng: WP2-6 selftest ");
    if (fails == 0) {
        serial_puts("PASS\r\n");
    } else {
        serial_puts("FAIL (");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    log_wp("WP2-6", fails);
#endif

    for (;;) {
        /* idle 点轮询 PS/2：没有 IRQ 时也不会把键盘字节丢在控制器里。 */
        ng_input_poll();
        __asm__ volatile("hlt");
    }
}
