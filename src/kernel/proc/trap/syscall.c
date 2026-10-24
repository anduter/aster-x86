/* =============================================================================
 * syscall.c - 旧原型系统调用分发（迁移期 int 0x81）
 *
 * 入口是 isr.asm 的 isr129（压栈布局与异常一致），isr_dispatch 看到 129
 * 就转到 syscall_dispatch。返回值写回现场的 eax 槽位，iret 后就在 eax 里。
 * 新程序请使用 int 0x80 的 Native ABI（intr/native.c）。
 *
 * 注意：现在系统调用由内核线程在 ring0 发起，参数是内核指针，可以直接用。
 * 等 P2 有用户态后，这里必须先校验指针落在用户地址范围，否则用户传一个
 * 内核地址进来就能读写内核内存。
 * ============================================================================= */
#include "syscall.h"
#include "interrupt.h"
#include "uaccess.h"
#include "syscall_internal.h"
#include "../../net/net_syscall.h"
#include "../../fs/fs_syscall.h"
#include "../../proc/proc_syscall.h"
#include "../../mm/mm_syscall.h"
#include "../../svc/svc_syscall.h"
#include "../../security/security_syscall.h"
#include "../../core/error.h"
#include "../../dev/char/console.h"
#include "../../fs/fs.h"
#include "../../fs/fat16.h"
#include "../../user/user_mode.h"
#include "../../mm/user_layout.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../mm/heap.h"
#include "../../dev/device.h"
#include "../../dev/block/block.h"
#include "../../fs/pipe.h"
#include "include/kernel.h"
#include "../../dev/rtc.h"
#include "../../dev/pci.h"
#include "../../dev/net/e1000.h"
#include "../../net/net.h"
#include "../../dev/acpi.h"
#include "../../lib/kprintf.h"
#include "../proc.h"
#include "../proc_internal.h"
#include "../../lib/kprintf.h"
#include "../../lib/string.h"

static unsigned int calls;

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


void syscall_dispatch(interrupt_frame_t *f)
{
    int ret;
    proc_t *sp = proc_current();

    calls++;

    /* 最小沙箱：进程只允许 syscall_mask 中列出的 legacy 调用。 */
    if (sp != 0 && sp->sandboxed && f->eax < 64U &&
        (sp->syscall_mask & (1ULL << f->eax)) == 0ULL) {
        f->eax = (uint32_t)-1;
        return;
    }

    /* 网络组（UDP/socket）已迁到 net/net_syscall.c（M5） */
    if (net_syscall_owns(f->eax)) {
        ret = net_syscall(f);
        goto delegate_out;
    }

    /* 文件系统组已迁到 fs/fs_syscall.c（M4） */
    if (fs_syscall_owns(f->eax)) {
        ret = fs_syscall(f);
        goto delegate_out;
    }

    /* 进程组已迁到 proc/proc_syscall.c（M1） */
    if (proc_syscall_owns(f->eax)) {
        ret = proc_syscall(f);
        goto delegate_out;
    }

    /* 内存组已迁到 mm/mm_syscall.c（M2） */
    if (mm_syscall_owns(f->eax)) {
        ret = mm_syscall(f);
        goto delegate_out;
    }

    /* 服务/能力组已迁到 svc/svc_syscall.c（M6） */
    if (svc_syscall_owns(f->eax)) {
        ret = svc_syscall(f);
        goto delegate_out;
    }

    /* 身份/权限组已迁到 security/security_syscall.c（M7） */
    if (security_syscall_owns(f->eax)) {
        ret = security_syscall(f);
        goto delegate_out;
    }

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
        ret = -1;
        break;
    }

delegate_out:
    /* pushad 保存的 eax 就在现场里，改这里等价于设置返回值 */
    f->eax = (unsigned int)ret;

    /* 返回用户态前处理待处理信号（可能直接终止或跳到用户 handler） */
    proc_check_signals(f);
}

unsigned int syscall_count(void)
{
    return calls;
}
