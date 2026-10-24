/* =============================================================================
 * core/misc_syscall.c - 杂项/调试相关系统调用（M0 壳层）
 *
 * 从 proc/trap/syscall.c 拆出的 misc/debug 组：write / getchar / reboot /
 * oops / panic / pci / poweroff / hang / resinfo / eth / mouse。
 * 只搬代码不改逻辑。
 * ============================================================================= */
#include "../types.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/fs.h"
#include "include/mm.h"
#include "include/net.h"
#include "include/proc.h"
#include "misc_syscall.h"
#include "../proc/trap/syscall_internal.h"

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
        if (!user_range_ok(f->ebx, f->ecx)) {
            ret = -1;
            break;
        }
        ret = sys_write_user(f->ebx, f->ecx);
        break;
    case SYS_GETCHAR:
        ret = sys_getchar_block();
        break;
    case SYS_TCP_TEST:
        ret = net_tcp_echo_test();
        break;
    case SYS_BLK_READ: {
        block_device_t *dev = blk_lookup("ata0");
        uint8_t sec[BLK_SECTOR];

        if (dev == 0 || blk_read(dev, f->ebx, sec) != 0 ||
            copy_to_user((void *)(uintptr_t)f->ecx, sec, sizeof(sec)) != E_OK) {
            ret = -1;
            break;
        }
        ret = 0;
        break;
    }
    case SYS_REBOOT:
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
        panic("panic requested by userland (pid %u)", (p != 0) ? p->pid : 0U);
        ret = 0;
        break;
    }
    case SYS_PCI: {
        /* ebx=index, ecx=用户缓冲：格式化 PCI 设备信息，index 越界返回 -1 */
        const pci_dev_t *d = pci_at((uint32_t)f->ebx);
        char b[64];

        if (d == 0) { ret = -1; break; }
        ksnprintf(b, sizeof(b), "%x:%x.%x %04x:%04x class=%02x%02x",
                  (unsigned int)d->bus, (unsigned int)d->dev, (unsigned int)d->func,
                  (unsigned int)d->vendor, (unsigned int)d->device,
                  (unsigned int)d->class_code, (unsigned int)d->subclass);
        ret = (copy_to_user((void *)(uintptr_t)f->ecx, b,
                            (uint32_t)strlen(b) + 1U) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_POWEROFF:
        acpi_poweroff();        /* 不返回 */
        ret = 0;
        break;
    case SYS_HANG:
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

        mouse_get(&x, &y, &b);
        out[0] = x;
        out[1] = y;
        out[2] = b;
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, out, sizeof(out)) == E_OK) ? 0 : -1;
        break;
    }
    default:
        return -1000;
    }
    return ret;
}
