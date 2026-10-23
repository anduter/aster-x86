/* =============================================================================
 * kernel.c - 内核主入口
 *
 * 启动顺序：
 *   0. 串口先起来，保证任何阶段的日志都能抓到
 *   1. framebuffer + 文字控制台（此时未开分页，写的是物理地址）
 *   2. PMM：物理页位图，标记内核镜像与堆区
 *   3. VMM：建页表 -> 映射显存 -> 再开分页（顺序很重要，见下）
 *   4. Heap
 *   5. 内存子系统自检
 *   6. 中断：IDT + 8259 + PIT + 键盘，然后开中断
 *   7. 显示运行状态，进入 idle：hlt 等中断，按秒刷新运行时间，回显键盘
 *
 * 关于第 3 步的顺序：如果先 vmm_enable_paging() 再映射显存，中间这段时间里
 * 任何 kprintf 都会写到未映射的物理地址上（没有 IDT，直接三重故障）。所以
 * 必须"先建映射、后开分页"。
 * ============================================================================= */
#include "types.h"
#include "io.h"
#include "fb.h"
#include "console.h"
#include "panic.h"
#include "version.h"
#include "intr/interrupt.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/memtest.h"
#include "heap.h"
#include "fs/ramfs.h"
#include "fs/fat16.h"
#include "core/pci.h"
#include "core/e1000.h"
#include "core/net.h"
#include "core/acpi.h"
#include "core/apic.h"
#include "core/percpu.h"
#include "core/smp.h"
#include "core/madt.h"
#include "block/block.h"
#include "core/error.h"
#include "core/program.h"
#include "core/device.h"
#include "core/demo.h"
#include "core/cpu.h"
#include "core/trust.h"
#include "core/splash.h"
#include "proc/proc.h"
#include "intr/syscall.h"
#include "intr/native.h"
#include "user/gdt.h"
#include "lib/string.h"

/* E820 内存映射表由加载器写在 0x5000（见 loader/sysinfo.asm） */
#define E820_COUNT_ADDR  0x5000U
#define E820_TABLE_ADDR  0x5002U
/* 每条 24 字节，而 MEM_TOTAL 位于 0x5200，所以最多只能放 21 条 */
#define E820_MAX_ENTRIES 21U

/* 已安装的用户程序大小（持久化自检用来比对） */
static uint32_t installed_shell_size;

/* 持久化后端设备名（ata0 优先，没有硬盘就退回 ram0） */
static const char *backing_disk;

/* 选择持久化磁盘：有有效文件系统就挂载，否则等安装完再格式化写盘 */
static void select_backing(void)
{
    block_device_t *d = blk_lookup("ata0");

    if (d == 0) d = blk_lookup("ram0");
    if (d == 0) {
        kprintln("fs: no block device, in-memory only");
        return;
    }

    backing_disk = d->name;
    fs_set_backing(d->name);

    if (fs_mount_disk(d->name) == E_OK) {
        kprintf("fs: mounted %s (%u sectors)\n", d->name,
                (unsigned int)d->sector_count);
    } else {
        kprintf("fs: %s has no valid filesystem, will format\n", d->name);
    }
}

/* 权限模型自检：root 写一个 0600 文件，验证其他人被拒绝、root 可读 */
static void auth_selftest(void)
{
    uint32_t h;
    int denied, allowed;

    h = fs_open("system:/auth.txt", FS_OPEN_WRITE | FS_OPEN_CREAT);
    if (h != 0U) {
        (void)fs_write(h, "auth", 4);
        fs_close(h);
    }

    (void)fs_chmod("system:/auth.txt", 0600U, 0U);

    denied = (fs_access_as("system:/auth.txt", FS_ACCESS_READ, 1000U, 1000U) == E_ACCES);
    allowed = (fs_access_as("system:/auth.txt", FS_ACCESS_READ, 0U, 0U) == E_OK);

    if (denied && allowed) {
        kprintln("auth: permission model ok (other denied, root allowed)");
    } else {
        kprintln("auth: permission model FAILED");
    }
}

/* 磁盘持久化自检：存盘 -> 清空内存 -> 从盘恢复 -> 校验文件（不破坏数据） */static void disk_selftest(void)
{
    fs_stat_t st;

    if (backing_disk == 0) {
        kprintln("disk: no block device");
        return;
    }

    if (fs_sync(backing_disk) != E_OK) { kprintln("disk: sync failed"); return; }

    fs_init();                          /* 模拟重启：清空整棵内存树 */
    if (fs_mount_disk(backing_disk) != E_OK) { kprintln("disk: mount failed"); return; }

    if (fs_stat("system:/bin/shell", &st) == E_OK && st.size == installed_shell_size) {
        block_device_t *d = blk_lookup(backing_disk);
        kprintf("disk: remount ok (%s, sectors=%u, system:/bin/shell=%u bytes)\n",
                backing_disk, (unsigned int)((d != 0) ? d->sector_count : 0U),
                (unsigned int)st.size);
    } else {
        kprintln("disk: remount FAILED");
    }
}

/* FAT16 交换盘自检：读根目录里的 host.txt（宿主工具写进去的） */
static void fat16_selftest(void)
{
    char buf[64];
    int h, n;

    if (!fat16_mounted()) return;

    h = fat16_open("host.txt", FS_OPEN_READ);
    if (h < 0) {
        kprintln("fat16: exchange disk is empty (no host.txt)");
        return;
    }
    n = fat16_read(h, buf, sizeof(buf) - 1U);
    fat16_close(h);

    if (n > 0) {
        buf[n] = '\0';
        kprintf("fat16: host.txt = %s\n", buf);
    } else {
        kprintln("fat16: host.txt read failed");
    }
}

/* 内建用户程序映像（由 user_blob.asm incbin 进来） */
extern uint8_t user_shell_blob_start[];
extern uint8_t user_shell_blob_end[];

/* 把内建的用户程序安装到文件系统，之后由 user_exec 从 /system/bin/shell 载入 */
static void install_programs(void)
{
    uint32_t size = (uint32_t)(uintptr_t)(user_shell_blob_end - user_shell_blob_start);
    uint32_t h;
    int n;

    fs_mkdir("system:/bin");
    fs_mount("data");           /* 第二个驱动号，演示多卷 */

    h = fs_open("system:/bin/shell", FS_OPEN_WRITE | FS_OPEN_CREAT);
    if (h == 0) {
        kprintln("fs: cannot create system:/bin/shell");
        return;
    }
    n = fs_write(h, user_shell_blob_start, size);
    fs_close(h);
    installed_shell_size = (n > 0) ? (uint32_t)n : 0U;

    kprintf("fs: volumes = %u, system:/bin/shell installed (%d bytes)\n",
            (unsigned int)fs_volume_count(), n);
    for (uint32_t i = 0; i < fs_volume_count(); i++) {
        char name[FS_DRIVE_MAX];
        if (fs_volume_name(i, name, sizeof(name)) == E_OK) {
            kprintf("  drive: %s\n", name);
        }
    }
}
/* 旧 ABI 自检：迁移期通过 int 0x81 进内核，验证过渡门和分发器都通了 */
static void syscall_selftest(void)
{
    static char msg[] = "sys: int 0x81 works\n";
    int written = sys_call(SYS_WRITE, (int)(uintptr_t)msg, (int)strlen(msg), 0);
    int uptime  = sys_call(SYS_UPTIME, 0, 0, 0);
    int bad     = sys_call(SYS_WRITE, (int)0x50000000, 4, 0);   /* 未映射的用户地址 */

    kprintf("sys: write=%d uptime=%d s\n", written, uptime);
    /* 期望 -1：缺页被恢复机制接住，而不是 panic */
    kprintf("sys: unmapped user pointer -> %d\n", bad);
}

void kernel_main(void)
{
    __asm__ volatile("cli");

    /* ---------- 0. 串口 ---------- */
    serial_init();

    /* ---------- 1. console ---------- */
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

    /* ---------- 2. PMM ---------- */
    unsigned long long total_mem =
        *(volatile unsigned long long *)(unsigned int)MEM_TOTAL;
    kprintf("mem: e820 total = %u MB\n", (unsigned int)(total_mem >> 20));

    pmm_init((unsigned char *)(unsigned int)0x200000U, (unsigned int)total_mem);
    kprintf("pmm: base=%p pages=%u free=%u\n",
            (void *)(uintptr_t)pmm.phys_base, pmm.total_pages, pmm.free_pages);

    extern unsigned char _kernel_start[];
    extern unsigned char _kernel_end[];
    pmm_mark_range((unsigned long long)(unsigned int)_kernel_start,
                   (unsigned long long)(unsigned int)(_kernel_end - _kernel_start));
    kprintf("pmm: kernel %p..%p (%u bytes) reserved\n",
            (void *)(uintptr_t)_kernel_start, (void *)(uintptr_t)_kernel_end,
            (unsigned int)(_kernel_end - _kernel_start));

    /* 堆是从 8MB 起切走的 16MB，必须提前告诉 PMM */
    pmm_mark_range((unsigned long long)HEAP_BASE, (unsigned long long)HEAP_SIZE);
    kprintf("pmm: heap %p +%u MB reserved\n",
            (void *)(uintptr_t)HEAP_BASE, (unsigned int)(HEAP_SIZE >> 20));

    unsigned short e820_count =
        *(volatile unsigned short *)(unsigned int)E820_COUNT_ADDR;
    if (e820_count > E820_MAX_ENTRIES) {
        e820_count = E820_MAX_ENTRIES;
    }
    pmm_init_e820((e820_entry_t *)(unsigned int)E820_TABLE_ADDR, e820_count);
    kprintf("pmm: %u e820 entries applied, free=%u\n",
            (unsigned int)e820_count, pmm.free_pages);
    console_set_status("pmm ready");

    /* ---------- 3. VMM ---------- */
    vmm_init();
    kprintf("vmm: PDPT @%p (PAE, NX=%s)\n",
            (void *)(uintptr_t)page_directory,
            vmm_nx_enabled ? "on" : "off");

    /* 先建显存映射，再开分页 */
    vmm_map_range(fb_addr, fb_addr, fb_size,
                  PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT | PTE_NOEXEC);
    vmm_enable_paging();
    vmm_kernel_protect();
    kprintf("vmm: paging on, framebuffer mapped uncached (%u bytes)\n", fb_size);
    console_set_status("paging on");

    /* 可选 CPU 安全特性：SMEP/SMAP 必须在分页开启后启用 */
    cpu_security_init();

    /* ---------- 4. Heap ---------- */
    heap_init();
    kprintf("heap: %p +%u MB\n",
            (void *)(uintptr_t)HEAP_BASE, (unsigned int)(HEAP_SIZE >> 20));
    if (pmm_selftest()) {
        kprintln("pmm: linear first-fit selftest ok");
    } else {
        kprintln("pmm: linear first-fit selftest FAIL");
    }

    /* 安装 shell 前验证内嵌镜像签名；无效会 panic，篡改副本必须被拒绝。 */
    trust_selftest();

    /* 用户态前提：GDT 增加 DPL3 的用户代码/数据段，并加载 TSS */
    gdt_init();
    kprintln("gdt: user segments + tss loaded");

    /* ---------- 5. 自检 ---------- */
    vmm_teardown_selftest();
    int failures = mm_selftest();
    if (failures == 0) {
        kprintln("selftest: PASS");
    } else {
        kprintf("selftest: FAIL (%d failures)\n", failures);
    }

    /* ---------- 6. 对象子系统：设备表 / 块设备 / 文件卷 / 程序表 ---------- */
    dev_init();                 /* 必须在 blk_init 之前：dev_init 会清空设备表 */
    blk_init();
    ata_init();                 /* 有硬盘就注册 ata0/ata1（重启后文件还在） */
    pci_init();                 /* PCI 总线枚举 */
    if (e1000_init() == E_OK) {
        (void)e1000_send_test();    /* 发一帧，验证 DMA/TX 链路 */
        net_init();                 /* ARP + ICMP RX 验证 */
    }
    acpi_init();                /* 找 FADT，为关机做准备 */
    madt_init();                /* 解析 MADT：CPU 列表 + IOAPIC */
    apic_init();                /* 只探测 LAPIC，不接管中断 */
    percpu_init_bsp(apic_id_from_cpuid(), madt_cpu_count());   /* SMP 地基：cpu0 上线 */
    fs_init();
    select_backing();
    if (blk_lookup("ata1") != 0) {
        (void)fat16_mount("ata1", "fat");   /* 第二块盘做 FAT16 交换盘 */
    }
    fat16_selftest();
    install_programs();
    auth_selftest();            /* 权限模型：0600 文件对其他人拒绝 */
    prog_init();
    kprintf("obj: subsystems ready (devices=%u programs=%u)\n",
            (unsigned int)dev_count(), (unsigned int)prog_count());

    /* ---------- 7. 中断 ---------- */
    interrupt_init();
    kprintf("intr: idt 0..31 exceptions, 32..47 irq\n");
    kprintf("intr: pic remapped to 0x20/0x28, pit %u Hz, irq0+irq1 enabled\n",
            (unsigned int)timer_hz());

    /* AP 必须在共享 IDT 初始化之后启动，否则 timer 中断没有门。 */
    smp_start_aps();
    kprintf("intr: handlers irq0=%s irq1=%s\n", irq_name(0), irq_name(1));
    __asm__ volatile("sti");
    kprintln("intr: interrupts on (IF=1)");

    /* PS/2 鼠标：需要中断已开（IRQ12） */
    (void)mouse_init();

    /* NMI 看门狗：映射 LAPIC 并开始校准（不接管 PIC 中断） */
    (void)apic_watchdog_start();

    /* 系统调用门（DPL3）：0x80 原生 ABI，0x81 旧原型过渡门 */
    kprintln("sys: int 0x80 native + int 0x81 legacy gates installed (DPL3)");
    syscall_selftest();
    native_selftest();

    /* ---------- 8. 状态 ---------- */
    console_set_status("ready - uptime 0 s");
    kprintln("kernel: ready");

    /* 启动自检：只跑一次，验证对象模型（程序/文件/设备互不冒充） */
    demo_run();

    /* 首次启动或更新后落盘；之后重启应能直接挂载回来 */
    if (backing_disk != 0) {
        if (fs_sync_backing() == E_OK) {
            kprintf("fs: synced to %s\n", backing_disk);
        } else {
            kprintln("fs: sync FAILED");
        }
    }

    /* 块设备 + 磁盘格式：存盘后重新挂载，文件应完好 */
    disk_selftest();

    /* 进程模块：把程序变成可调度的内核线程，然后交给调度器。
       shell 与 heartbeat 都是程序表里的程序，内核不解释任何命令。 */
    proc_init();
    if (proc_spawn("shell") == 0) {
        panic("cannot spawn shell program");
    }
    if (proc_spawn("heartbeat") == 0) {
        panic("cannot spawn heartbeat program");
    }
    if (proc_spawn("hello") == 0) {
        kprintln("proc: hello spawn failed");
    }
    if (proc_spawn("userdemo") == 0) {
        panic("cannot spawn userdemo program");
    }

    /* 多核调度回归：4 个忙等线程由负载放置决定落在哪颗核上（不看名字）。
       它们 2 秒后自行退出，不会影响后续交互。 */
    if (smp_cpu_count > 1U) {
        for (uint32_t i = 0; i < 4U; i++) {
            (void)proc_spawn("spin");
        }
    }

    console_set_status("ready - ring3 shell");
    sched_start();

    panic("scheduler returned unexpectedly");
}
