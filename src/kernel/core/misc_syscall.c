/* =============================================================================
 * core/misc_syscall.c - 杂项/调试相关系统调用（M0 壳层）
 *
 * 从 proc/trap/syscall.c 拆出的 misc/debug 组：write / getchar / reboot /
 * oops / panic / pci / poweroff / hang / resinfo / eth / mouse。
 * 只搬代码不改逻辑。
 * ============================================================================= */
#include "../include/types.h"
#include "include/abi.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/fs.h"
#include "include/mm.h"
#include "include/net.h"
#include "include/proc.h"
#include "include/pkg.h"
#include "misc_syscall.h"
#include "../proc/trap/syscall_internal.h"

/* ring3 传入的缓冲必须落在用户映射区 [USER_LAYOUT_CODE_BASE, 3GB)。
 *
 * 这里只做“地址窗口”校验，不逐个查页是否映射：未映射页仍由 copy_*_user
 * 的异常修复表返回错误（boot 的 sys: unmapped user pointer -> -1 覆盖）。
 *
 * 为什么需要下界：user_range_ok() 只拒绝 >= 0xC0000000 的指针，0x00100000
 * 这样的内核恒等映射地址会被当成用户缓冲。此前 ring3 可以用 SYS_WRITE 把
 * 内核内存（如 0x100000 处的 'KRNL' 头）打印到串口，也可以用 SYS_BLK_READ/
 * PCI/RESINFO/ETH/MOUSE 让 copy_to_user 写内核内存。
 *
 * 内核自检（p->is_user == false）例外：core/bootseq.c 的 syscall_selftest
 * 就是用内核里的字符串调 SYS_WRITE 验证 int 0x81 通路的。 */
static bool misc_user_buf(uint32_t ptr, uint32_t len)
{
    proc_t *p = proc_current();

    if (p != 0 && p->is_user && ptr < USER_LAYOUT_CODE_BASE) return false;
    return user_range_ok(ptr, len);
}

/* 系统级调试/关机入口的权限门：只有 root（或内核线程）能让整机停机。
 * 这些调用在任何 ring3 进程里都可达，若不做校验，普通用户一条 int 0x81
 * 就能 panic/hang/reboot/poweroff 整机。 */
static bool misc_system_privileged(void)
{
    proc_t *p = proc_current();

    return (p == 0) || (p->euid == 0U);
}

/* 分块把用户缓冲区拷进内核栈，再输出。
 * 用户指针未映射时 copy_from_user 会走 #PF 恢复路径返回错误，
 * 而不是把整个系统 panic 掉。 */
static int sys_write_user(uint32_t ptr, uint32_t len)
{
    char buf[129];
    uint32_t done = 0;

    while (done < len) {
        uint32_t n = len - done;

        if (n > 128U) n = 128U;
        if (copy_from_user(buf, (const void *)(uintptr_t)(ptr + done), n) != E_OK) {
            return -1;
        }
        buf[n] = '\0';
        console_write_atomic(buf);
        done += n;
    }
    return (int)done;
}

bool misc_syscall_owns(uint32_t nr)
{
    switch (nr) {
    case SYS_WRITE:
    case SYS_GETCHAR:
    /* 这两个是历史遗留的自检/调试入口，不在原 6 组清单里，随 misc/debug 收口 */
    case SYS_TCP_TEST:
    case SYS_BLK_READ:
    case SYS_REBOOT:
    case SYS_PANIC:
    case SYS_OOPS:
    case SYS_PCI:
    case SYS_POWEROFF:
    case SYS_HANG:
    case SYS_RESINFO:
    case SYS_ETH:
    case SYS_MOUSE:
    case SYS_CONSOLE_CURSOR: /* v1.2：输入框下划线光标（只画 framebuffer） */
    case SYS_ABI_VERSION:   /* ABI v1 只追加的标量版本查询（59） */
    case SYS_PKG:           /* v1.4：组件 CLI（P0，op 编码见 include/pkg.h） */
        return true;
    default:
        return false;
    }
}

int misc_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {
    case SYS_WRITE:
        if (!misc_user_buf(f->ebx, f->ecx)) {
            ret = -1;
            break;
        }
        ret = sys_write_user(f->ebx, f->ecx);
        break;
    case SYS_GETCHAR:
        ret = sys_getchar_block();
        break;
    case SYS_CONSOLE_CURSOR:
        /* ebx=0 隐藏；非 0 显示。只动 framebuffer 输入框，不写串口，
         * 所以不影响任何日志断言。 */
        console_cursor_set((f->ebx != 0U) ? 1 : 0);
        ret = 0;
        break;
    case SYS_TCP_TEST:
        ret = net_tcp_echo_test();
        break;
    case SYS_BLK_READ: {
        block_device_t *dev = blk_lookup("ata0");
        uint8_t sec[BLK_SECTOR];

        if (!misc_user_buf(f->ecx, BLK_SECTOR)) {
            ret = -1;
            break;
        }
        if (dev == 0 || blk_read(dev, f->ebx, sec) != 0 ||
            copy_to_user((void *)(uintptr_t)f->ecx, sec, sizeof(sec)) != E_OK) {
            ret = -1;
            break;
        }
        ret = 0;
        break;
    }
    case SYS_REBOOT:
        if (!misc_system_privileged()) { ret = -1; break; }
        machine_reboot();       /* 不返回 */
        ret = 0;
        break;
    case SYS_OOPS: {
        /* 调试：故意访问未映射的内核地址，走 oops + 符号化回溯 */
        volatile uint32_t *bad = (volatile uint32_t *)(uintptr_t)0xDEADB000U;
        ret = (int)*bad;
        break;
    }
    case SYS_PANIC: {
        proc_t *p = proc_current();

        if (!misc_system_privileged()) { ret = -1; break; }
        panic("panic requested by userland (pid %u)", (p != 0) ? p->pid : 0U);
        ret = 0;
        break;
    }
    case SYS_PCI: {
        /* ebx=index, ecx=用户缓冲：格式化 PCI 设备信息，index 越界返回 -1 */
        const pci_dev_t *d = pci_at((uint32_t)f->ebx);
        char b[64];

        if (d == 0 || !misc_user_buf(f->ecx, sizeof(b))) { ret = -1; break; }
        ksnprintf(b, sizeof(b), "%x:%x.%x %04x:%04x class=%02x%02x",
                  (unsigned int)d->bus, (unsigned int)d->dev, (unsigned int)d->func,
                  (unsigned int)d->vendor, (unsigned int)d->device,
                  (unsigned int)d->class_code, (unsigned int)d->subclass);
        ret = (copy_to_user((void *)(uintptr_t)f->ecx, b,
                            (uint32_t)strlen(b) + 1U) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_POWEROFF:
        if (!misc_system_privileged()) { ret = -1; break; }
        acpi_poweroff();        /* 不返回 */
        ret = 0;
        break;
    case SYS_HANG:
        if (!misc_system_privileged()) { ret = -1; break; }
        /* 调试：关中断并死循环，只有 NMI 看门狗能发现 */
        kprintln("hang: disabling interrupts and spinning (NMI watchdog should fire)");
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
        ret = 0;
        break;
    case SYS_RESINFO: {
        /* ebx=用户 u32[10]：物理页/堆/进程/系统调用/COW/FS/设备/PCI */
        unsigned int out[10];
        unsigned int hu = 0, ht = 0;

        if (!misc_user_buf(f->ebx, sizeof(out))) { ret = -1; break; }
        heap_stats(&hu, &ht);
        out[0] = pmm.free_pages;
        out[1] = pmm.total_pages;
        out[2] = hu;
        out[3] = ht;
        out[4] = proc_count();
        out[5] = syscall_count();
        out[6] = vmm_cow_copies;
        out[7] = fs_node_count();
        out[8] = dev_count();
        out[9] = pci_count();
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, out, sizeof(out)) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_ETH: {
        /* ebx=cmd(0=info,1=send test), ecx=用户缓冲 */
        if (f->ebx == 0U) {
            uint8_t info[8];

            if (!misc_user_buf(f->ecx, sizeof(info))) { ret = -1; break; }
            e1000_mac(info);
            info[6] = (uint8_t)(e1000_present() ? 1 : 0);
            info[7] = (uint8_t)(e1000_link_up() ? 1 : 0);
            ret = (copy_to_user((void *)(uintptr_t)f->ecx, info, sizeof(info)) == E_OK) ? 0 : -1;
        } else if (f->ebx == 1U) {
            ret = (e1000_send_test() == E_OK) ? 0 : -1;
        } else {
            ret = -1;
        }
        break;
    }
    case SYS_MOUSE: {
        /* ebx=用户 int[3]：写 {x, y, buttons} */
        int x = 0, y = 0, b = 0;
        int out[3];

        if (!misc_user_buf(f->ebx, sizeof(out))) { ret = -1; break; }
        mouse_get(&x, &y, &b);
        out[0] = x;
        out[1] = y;
        out[2] = b;
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, out, sizeof(out)) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_ABI_VERSION:
        /* ABI v1：高 16 位版本，低 16 位 feature bits；无参数、无指针 */
        ret = (int)((OS_ABI_VERSION << 16) | OS_ABI_FEATURES);
        break;
    case SYS_PKG:
        /* v1.4：ebx=op, ecx=a, edx=b；组件 CLI（实现在 pkg/pkg_syscall.c） */
        ret = pkg_syscall(f->ebx, f->ecx, f->edx);
        break;
    default:
        return -1000;
    }
    return ret;
}
