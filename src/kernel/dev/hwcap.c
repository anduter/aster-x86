/* =============================================================================
 * dev/hwcap.c - 硬件 capability 后端（MMIO / IRQ / DMA）
 *
 * 设计：docs/design/three-layer.md §3.3/§6。P1 的 SYS_CAP_GRANT 只在端口上
 * 记 (type,id,rights)，本模块把 cap 变成真正可用的硬件资源：
 *
 *   MMIO  SYS_MMIO_MAP(90)：base = grant 的物理基址；在 HWCAP_VA 窗口找一段
 *         空闲 VA，逐页 vmm_map_user_page。PTE 带 USER|NX|PCD|PWT；grant
 *         没有 WRITE 时 PTE 只读。PTE_HWCAP 让 vmm 拆除用户区时不要对
 *         设备物理页调 pmm_free_pages（只有 PMM 管理的假 MMIO 测试页才释放）。
 *
 *   IRQ   SYS_CAP_GRANT(84, CAP_IRQ) 调 hwcap_irq_bind()：IRQ 线放行并记到
 *         端口；irq_dispatch 先问 hwcap_irq_deliver()。绑定事件先计数并投递
 *         HWCAP_EV_IRQ 到端口；用户态 ack（SYS_IRQ_WAIT 成功返回）前堆积
 *         超过 HWCAP_IRQ_STORM_LIMIT 就把线掩掉，避免中断风暴。owner 退出/
 *         端口释放/IRQ 线被别的 handler 占用 -> 解绑并恢复屏蔽。
 *
 *   DMA   SYS_DMA_ALLOC(91)：必须持有 CAP_DMA grant（id = 允许的最大字节数），
 *         分配 <=64KB 物理连续页、映射进用户空间、返回 cap id；FREE(92)
 *         解映射并 free_pages。坏 cap/非 owner/重复释放返回 -1。
 *
 * 无 IOMMU 时本模块只保证“DMA 缓冲是内核分配的物理连续页”，设备仍可能
 * 越界访问其它内存（设计 §6：netd 迁 ring3 必须开 vIOMMU）。
 * ============================================================================= */
#include "hwcap.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/proc.h"
#include "include/svc.h"

#define HWCAP_MAX            32U
#define HWCAP_KIND_MMIO      1U
#define HWCAP_KIND_DMA       2U

typedef struct {
    bool     used;
    uint32_t kind;              /* HWCAP_KIND_* */
    uint32_t cap;               /* MMIO: 物理基址；DMA: 返回给用户的 cap id */
    uint32_t owner_pid;
    uint32_t phys;
    uint32_t len;               /* 字节数（页对齐） */
    uint32_t va;
    uint32_t rights;
    uint32_t pages;
} hwcap_entry_t;

typedef struct {
    bool     used;
    uint32_t irq;
    uint32_t port;
    uint32_t owner_pid;         /* 端口 owner（cap 的持有者） */
    uint32_t count;             /* 尚未被 SYS_IRQ_WAIT 消费的事件数 */
    uint32_t dropped;           /* pending 期间到达但未处理的事件数（风暴计数） */
    bool     pending;           /* 有事件尚未 ack */
    bool     masked;            /* 是否因为风暴被本模块掩掉 */
} hwcap_irq_t;

static hwcap_entry_t hwcap_entries[HWCAP_MAX];
static hwcap_irq_t   hwcap_irqs[HWCAP_IRQ_MAX];
static spinlock_t    hwcap_lock = SPINLOCK_INIT;
static uint32_t      hwcap_used;          /* 已用 cap 记录数（快速判空） */
static uint32_t      hwcap_bind_count;    /* 已绑定 IRQ 数（中断路径快速判空） */
static uint32_t      hwcap_dma_gen = 1U;  /* DMA cap id 的 generation */

/* ---------------- 小工具 ---------------- */

static uint32_t hwcap_va_alloc(uint32_t pages)
{
    uint32_t span = pages * PAGE_SIZE;

    if (pages == 0U || span / PAGE_SIZE != pages) return 0U;
    if (span > HWCAP_VA_END - HWCAP_VA_BASE) return 0U;

    for (uint32_t va = HWCAP_VA_BASE; va + span <= HWCAP_VA_END;
         va += PAGE_SIZE) {
        uint32_t i;

        for (i = 0; i < pages; i++) {
            if (vmm_get_phys(va + i * PAGE_SIZE) != 0U) break;
        }
        if (i == pages) return va;
    }
    return 0U;
}

static hwcap_entry_t *hwcap_entry_free_locked(void)
{
    for (uint32_t i = 0; i < HWCAP_MAX; i++) {
        if (!hwcap_entries[i].used) return &hwcap_entries[i];
    }
    return 0;
}

static hwcap_entry_t *hwcap_entry_find_locked(uint32_t owner_pid,
                                              uint32_t kind, uint32_t cap)
{
    for (uint32_t i = 0; i < HWCAP_MAX; i++) {
        hwcap_entry_t *e = &hwcap_entries[i];

        if (e->used && e->owner_pid == owner_pid && e->kind == kind &&
            e->cap == cap) {
            return e;
        }
    }
    return 0;
}

/* 记录还对应着调用者当前地址空间里的映射吗？
 * 进程退出后 PMM 会回收 DMA 页、MMIO 页表项也会消失；如果 pid 被复用，
 * 旧记录可能“看起来”属于新进程。释放/重复映射前用 PTE 反查校验，
 * 避免把复用了 pid 的新进程的页当成旧 cap 释放。 */
static bool hwcap_entry_mapping_ok_locked(const hwcap_entry_t *e)
{
    uint32_t flags;

    if (e->va == 0U || e->pages == 0U) return false;
    if (vmm_get_phys(e->va) != e->phys) return false;
    flags = vmm_user_flags(e->va);
    return (flags & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER);
}

static hwcap_irq_t *hwcap_irq_find_locked(uint32_t irq)
{
    for (uint32_t i = 0; i < HWCAP_IRQ_MAX; i++) {
        if (hwcap_irqs[i].used && hwcap_irqs[i].irq == irq) {
            return &hwcap_irqs[i];
        }
    }
    return 0;
}

/* 取消一个绑定（持锁调用）；屏蔽动作由调用方在锁外做。 */
static void hwcap_irq_clear_locked(hwcap_irq_t *b)
{
    memset(b, 0, sizeof(*b));
    if (hwcap_bind_count != 0U) hwcap_bind_count--;
}

/* ---------------- MMIO ---------------- */

int hwcap_mmio_map_core(uint32_t base, uint32_t *out_va, struct process *caller)
{
    uint32_t gid = 0U, rights = 0U, len_pages = 0U;
    uint32_t va, flags, lock_flags;
    hwcap_entry_t *e;
    uint32_t i;

    if (caller == 0 || out_va == 0 || base == 0U) return -1;
    if ((base & 0xFFFU) != 0U) return -1;

    /* 只允许使用“自己拥有的端口上的 grant”；grant 本身只能 root 授。 */
    if (ipc_cap_lookup(caller->pid, IPC_CAP_MMIO, base,
                       &gid, &rights, &len_pages) != 0) {
        return -1;
    }
    if (gid != base) return -1;
    if (len_pages == 0U) len_pages = 1U;         /* 老 3 参 cap_grant 的默认 */
    if (len_pages > HWCAP_MMIO_MAX_PAGES) return -1;
    if ((rights & IPC_RIGHT_READ) == 0U) return -1;   /* MMIO 至少可读 */
    if (base + len_pages * PAGE_SIZE < base) return -1;   /* 物理范围回绕 */

    lock_flags = spin_lock_irqsave(&hwcap_lock);

    /* 同一个 owner 对同一段物理区间只映射一次（避免重复 PTE/资源泄漏）。
     * 若是 pid 复用遗留的失效记录（PTE 已不在），清掉后允许重新映射。 */
    e = hwcap_entry_find_locked(caller->pid, HWCAP_KIND_MMIO, base);
    if (e != 0) {
        if (hwcap_entry_mapping_ok_locked(e)) {
            spin_unlock_irqrestore(&hwcap_lock, lock_flags);
            return -1;
        }
        memset(e, 0, sizeof(*e));
        if (hwcap_used != 0U) hwcap_used--;
    }
    e = hwcap_entry_free_locked();
    if (e == 0) {
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        return -1;
    }
    va = hwcap_va_alloc(len_pages);
    if (va == 0U) {
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        return -1;
    }

    memset(e, 0, sizeof(*e));
    e->used = true;
    e->kind = HWCAP_KIND_MMIO;
    e->cap = base;
    e->owner_pid = caller->pid;
    e->phys = base;
    e->len = len_pages * PAGE_SIZE;
    e->va = va;
    e->rights = rights;
    e->pages = len_pages;
    hwcap_used++;

    flags = PTE_PRESENT | PTE_USER | PTE_PCD | PTE_PWT |
            PTE_HWCAP | PTE_NOEXEC;
    if ((rights & IPC_RIGHT_WRITE) != 0U) flags |= PTE_WRITE;

    for (i = 0; i < len_pages; i++) {
        if (vmm_map_user_page(va + i * PAGE_SIZE, base + i * PAGE_SIZE,
                              flags) != 0) {
            for (uint32_t j = 0; j < i; j++) {
                (void)vmm_unmap_page(va + j * PAGE_SIZE);
            }
            memset(e, 0, sizeof(*e));
            if (hwcap_used != 0U) hwcap_used--;
            spin_unlock_irqrestore(&hwcap_lock, lock_flags);
            return -1;
        }
    }
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    *out_va = va;
    return 0;
}

int hwcap_mmio_map(uint32_t base, uint32_t uptr, struct process *caller)
{
    uint32_t va = 0U;
    int r = hwcap_mmio_map_core(base, &va, caller);

    if (r != 0) return -1;
    if (copy_to_user((void *)(uintptr_t)uptr, &va, sizeof(va)) != E_OK) {
        /* 用户缓冲坏了：把刚建的映射撤掉，不能留下不可见的页表项。 */
        (void)hwcap_mmio_unmap_core(base, caller);
        return -1;
    }
    return 0;
}

int hwcap_mmio_unmap_core(uint32_t base, struct process *caller)
{
    uint32_t lock_flags, va = 0U, pages = 0U;
    hwcap_entry_t *e;

    if (caller == 0) return -1;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    e = hwcap_entry_find_locked(caller->pid, HWCAP_KIND_MMIO, base);
    if (e == 0 || !hwcap_entry_mapping_ok_locked(e)) {
        if (e != 0) {
            memset(e, 0, sizeof(*e));
            if (hwcap_used != 0U) hwcap_used--;
        }
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        return -1;
    }
    va = e->va;
    pages = e->pages;
    memset(e, 0, sizeof(*e));
    if (hwcap_used != 0U) hwcap_used--;
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    for (uint32_t i = 0; i < pages; i++) {
        (void)vmm_unmap_page(va + i * PAGE_SIZE);
    }
    return 0;
}

/* ---------------- IRQ ---------------- */

int hwcap_irq_bind(uint32_t port, uint32_t irq, struct process *caller)
{
    hwcap_irq_t *b = 0;
    uint32_t owner, lock_flags;
    int free_slot = -1;

    if (caller == 0 || port == 0U) return -1;
    if (!proc_is_root(caller)) return -1;
    if (irq >= HWCAP_IRQ_MAX) return -1;
    /* 级联线/伪中断线不交给用户态；有内核 handler 的线也不许抢
     * （0/1/4/11/12 当前分别属于 timer/keyboard/serial/e1000/mouse）。 */
    if (irq == 2U || irq == 7U || irq == 15U) return -1;
    if (strcmp(irq_name(irq), "unused") != 0) return -1;
    if (!ipc_port_exists(port)) return -1;
    owner = ipc_port_owner_pid(port);
    if (owner == 0U) return -1;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    b = hwcap_irq_find_locked(irq);
    if (b != 0) {
        int same = (b->port == port && b->owner_pid == owner);

        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        return same ? 0 : -1;          /* 幂等；别人已绑定则拒绝 */
    }
    for (uint32_t i = 0; i < HWCAP_IRQ_MAX; i++) {
        if (!hwcap_irqs[i].used) { free_slot = (int)i; break; }
    }
    if (free_slot < 0) {
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        return -1;
    }
    b = &hwcap_irqs[free_slot];
    memset(b, 0, sizeof(*b));
    b->used = true;
    b->irq = irq;
    b->port = port;
    b->owner_pid = owner;
    hwcap_bind_count++;
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    /* 允许该线产生中断；解绑/owner 退出时恢复到默认屏蔽。 */
    (void)irq_unmask(irq);
    return 0;
}

int hwcap_irq_wait(uint32_t irq, uint32_t timeout_ticks, struct process *caller)
{
    uint32_t deadline;

    if (caller == 0 || irq >= HWCAP_IRQ_MAX) return -1;
    if (timeout_ticks == 0U) timeout_ticks = HWCAP_IRQ_WAIT_DEFAULT;
    if (timeout_ticks > HWCAP_IRQ_WAIT_MAX) return -1;
    deadline = timer_ticks() + timeout_ticks;

    for (;;) {
        uint32_t lock_flags, port = 0U;
        bool got = false;
        bool unmask = false;
        hwcap_irq_t *b;

        lock_flags = spin_lock_irqsave(&hwcap_lock);
        b = hwcap_irq_find_locked(irq);
        if (b == 0 || b->owner_pid != caller->pid) {
            spin_unlock_irqrestore(&hwcap_lock, lock_flags);
            return -1;
        }
        if (b->count != 0U) {
            b->count--;
            if (b->count == 0U) {
                b->pending = false;
                b->dropped = 0U;
                if (b->masked) {
                    b->masked = false;
                    unmask = true;
                }
            }
            got = true;
        }
        port = b->port;
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);

        if (got) {
            /* 用户态 ack：风暴期间被掩掉的线在这里重新放行。 */
            if (unmask) (void)irq_unmask(irq);
            return (int)irq;            /* 事件带 IRQ 号 */
        }

        if (!ipc_port_exists(port)) return -1;   /* 端口被释放 -> 绑定作废 */
        if (caller->pending_sig != 0U) return -IPC_EINTR;
        if (proc_current() == 0) return -IPC_EAGAIN;
        if ((int32_t)(timer_ticks() - deadline) >= 0) return -IPC_EAGAIN;
        proc_sleep_ticks(1U);
    }
}

int hwcap_irq_deliver(uint32_t irq)
{
    uint32_t lock_flags, port = 0U, seq = 0U, dropped = 0U;
    bool post = false, mask_now = false;
    hwcap_irq_t *b;

    if (irq >= HWCAP_IRQ_MAX || hwcap_bind_count == 0U) return 0;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    b = hwcap_irq_find_locked(irq);
    if (b == 0) {
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        return 0;
    }
    /* owner 已退出或端口已释放：解绑并交回默认处理（没有 handler 的线
     * 由 irq_dispatch 的默认分支重新屏蔽）。 */
    if (proc_find(b->owner_pid) == 0 || !ipc_port_exists(b->port)) {
        hwcap_irq_clear_locked(b);
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        (void)irq_mask(irq);
        return 0;
    }

    b->count++;
    if (b->pending) {
        if (b->dropped != 0xFFFFFFFFU) b->dropped++;
        if (b->dropped >= HWCAP_IRQ_STORM_LIMIT && !b->masked) {
            b->masked = true;
            mask_now = true;
        }
    } else {
        b->pending = true;
    }
    port = b->port;
    seq = b->count;
    dropped = b->dropped;
    post = true;
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    if (mask_now) (void)irq_mask(irq);   /* 用户态 ack 前先静音，防风暴 */

    if (post) {
        uint32_t ev[3];

        ev[0] = irq;
        ev[1] = seq;
        ev[2] = dropped;
        /* 端口满时事件丢弃，但 count 已经记上，IRQ_WAIT 仍能拿到事件。 */
        (void)ipc_port_post(port, HWCAP_EV_IRQ, ev, sizeof(ev));
    }
    return 1;
}

void hwcap_port_released(uint32_t port)
{
    uint32_t lock_flags;
    uint32_t mask_list[HWCAP_IRQ_MAX];
    uint32_t n = 0U;

    if (port == 0U || hwcap_bind_count == 0U) return;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    for (uint32_t i = 0; i < HWCAP_IRQ_MAX; i++) {
        hwcap_irq_t *b = &hwcap_irqs[i];

        if (!b->used || b->port != port) continue;
        mask_list[n++] = b->irq;
        (void)hwcap_irq_clear_locked(b);
    }
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    for (uint32_t i = 0; i < n; i++) {
        (void)irq_mask(mask_list[i]);
    }
}

void hwcap_poll(void)
{
    uint32_t lock_flags;
    uint32_t mask_list[HWCAP_IRQ_MAX];
    uint32_t n = 0U;

    if (hwcap_bind_count == 0U && hwcap_used == 0U) return;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    for (uint32_t i = 0; i < HWCAP_IRQ_MAX; i++) {
        hwcap_irq_t *b = &hwcap_irqs[i];

        if (!b->used) continue;
        if (proc_find(b->owner_pid) != 0 && ipc_port_exists(b->port)) continue;
        mask_list[n++] = b->irq;
        (void)hwcap_irq_clear_locked(b);
    }
    for (uint32_t i = 0; i < HWCAP_MAX; i++) {
        hwcap_entry_t *e = &hwcap_entries[i];

        if (!e->used) continue;
        if (proc_find(e->owner_pid) != 0) continue;
        /* 进程已经退出：用户页由 vmm_clear_user_space() 回收（MMIO 的设备
         * 物理页带 PTE_HWCAP，不会被误还给 PMM；DMA 页是 PMM 页，正常回收）。
         * 这里只清掉记录，绝不能再 free 一次。 */
        memset(e, 0, sizeof(*e));
        if (hwcap_used != 0U) hwcap_used--;
    }
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    for (uint32_t i = 0; i < n; i++) {
        (void)irq_mask(mask_list[i]);
    }
}

/* ---------------- DMA ---------------- */

static uint32_t hwcap_dma_cap_make(uint32_t slot)
{
    uint32_t gen = hwcap_dma_gen++;

    if (hwcap_dma_gen == 0U || hwcap_dma_gen > 0x007FFFFFU) hwcap_dma_gen = 1U;
    return ((gen & 0x007FFFFFU) << 8) | (slot & 0xFFU);
}

int hwcap_dma_alloc_core(uint32_t len, uint32_t align, uint32_t *out_va,
                         struct process *caller)
{
    uint32_t gid = 0U, rights = 0U, len_pages_grant = 0U;
    uint32_t pages, va, lock_flags, cap = 0U;
    uint32_t flags = PTE_PRESENT | PTE_USER | PTE_WRITE | PTE_NOEXEC;
    void *phys;
    hwcap_entry_t *e;
    uint32_t i;

    if (caller == 0 || out_va == 0) return -1;
    if (len == 0U || len > HWCAP_DMA_MAX) return -1;
    if (align != 0U && align != PAGE_SIZE) return -1;

    /* CAP_DMA grant：id 是允许的最大字节数；rights 必须含 READ|WRITE。 */
    if (ipc_cap_lookup(caller->pid, IPC_CAP_DMA, 0U,
                       &gid, &rights, &len_pages_grant) != 0) {
        return -1;
    }
    (void)len_pages_grant;
    if (gid < len) return -1;
    if ((rights & (IPC_RIGHT_READ | IPC_RIGHT_WRITE)) !=
        (IPC_RIGHT_READ | IPC_RIGHT_WRITE)) {
        return -1;
    }

    pages = (len + PAGE_SIZE - 1U) / PAGE_SIZE;
    if (pages == 0U || pages > HWCAP_DMA_MAX / PAGE_SIZE) return -1;

    phys = alloc_pages(pages);
    if (phys == 0) return -1;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    e = hwcap_entry_free_locked();
    if (e == 0) {
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        free_pages(phys, pages);
        return -1;
    }
    va = hwcap_va_alloc(pages);
    if (va == 0U) {
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        free_pages(phys, pages);
        return -1;
    }

    memset(e, 0, sizeof(*e));
    e->used = true;
    e->kind = HWCAP_KIND_DMA;
    e->owner_pid = caller->pid;
    e->phys = (uint32_t)(uintptr_t)phys;
    e->len = len;
    e->va = va;
    e->rights = rights;
    e->pages = pages;
    e->cap = hwcap_dma_cap_make((uint32_t)(e - hwcap_entries));
    cap = e->cap;
    hwcap_used++;

    for (i = 0; i < pages; i++) {
        if (vmm_map_user_page(va + i * PAGE_SIZE,
                              (uint32_t)(uintptr_t)phys + i * PAGE_SIZE,
                              flags) != 0) {
            for (uint32_t j = 0; j < i; j++) {
                (void)vmm_unmap_page(va + j * PAGE_SIZE);
            }
            memset(e, 0, sizeof(*e));
            if (hwcap_used != 0U) hwcap_used--;
            spin_unlock_irqrestore(&hwcap_lock, lock_flags);
            free_pages(phys, pages);
            return -1;
        }
    }
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    *out_va = va;
    return (int)cap;
}

int hwcap_dma_alloc(uint32_t len, uint32_t align, uint32_t uptr,
                    struct process *caller)
{
    uint32_t va = 0U;
    int cap = hwcap_dma_alloc_core(len, align, &va, caller);

    if (cap <= 0) return -1;
    if (uptr != 0U &&
        copy_to_user((void *)(uintptr_t)uptr, &va, sizeof(va)) != E_OK) {
        (void)hwcap_dma_free((uint32_t)cap, caller);
        return -1;
    }
    return cap;
}

int hwcap_dma_free(uint32_t cap, struct process *caller)
{
    uint32_t lock_flags, va = 0U, pages = 0U, phys = 0U;
    hwcap_entry_t *e;

    if (caller == 0 || cap == 0U) return -1;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    e = hwcap_entry_find_locked(caller->pid, HWCAP_KIND_DMA, cap);
    if (e == 0 || !hwcap_entry_mapping_ok_locked(e)) {
        if (e != 0) {
            /* 失效记录（进程已退出/地址空间已换）：只清记录，绝不再 free。 */
            memset(e, 0, sizeof(*e));
            if (hwcap_used != 0U) hwcap_used--;
        }
        spin_unlock_irqrestore(&hwcap_lock, lock_flags);
        return -1;                  /* 坏 cap 或重复释放 */
    }
    va = e->va;
    pages = e->pages;
    phys = e->phys;
    memset(e, 0, sizeof(*e));
    if (hwcap_used != 0U) hwcap_used--;
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);

    if (pages != 0U && va != 0U) {
        for (uint32_t i = 0; i < pages; i++) {
            (void)vmm_unmap_page(va + i * PAGE_SIZE);
        }
    }
    if (phys != 0U && pages != 0U) {
        free_pages((void *)(uintptr_t)phys, pages);
    }
    return 0;
}

/* ---------------- 自检（-Test 构建，supervisor 线程） ---------------- */

static bool hwcap_selftest_entry_unmap(uint32_t base, struct process *p)
{
    return hwcap_mmio_unmap_core(base, p) == 0;
}

static bool hwcap_selftest_irq_masked(uint32_t irq)
{
    uint32_t lock_flags;
    bool masked = false;
    hwcap_irq_t *b;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    b = hwcap_irq_find_locked(irq);
    if (b != 0) masked = b->masked;
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);
    return masked;
}

static bool hwcap_selftest_irq_bound(uint32_t irq)
{
    uint32_t lock_flags;
    bool bound;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    bound = (hwcap_irq_find_locked(irq) != 0);
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);
    return bound;
}

static bool hwcap_selftest_irq_dropped(uint32_t irq, uint32_t *out)
{
    uint32_t lock_flags;
    bool ok = false;
    hwcap_irq_t *b;

    lock_flags = spin_lock_irqsave(&hwcap_lock);
    b = hwcap_irq_find_locked(irq);
    if (b != 0) {
        *out = b->dropped;
        ok = true;
    }
    spin_unlock_irqrestore(&hwcap_lock, lock_flags);
    return ok;
}

static void hwcap_selftest_pattern(uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)(i * 7U + 3U);
    }
}

void hwcap_selftest(void)
{
    struct process *p = proc_current();
    int ok = 1;
    int mport = -1, iport = -1, dport = -1;
    uint32_t fake = 0U, mmio_va = 0U, mmio_ro_va = 0U;
    uint32_t dma_va = 0U, dma_cap = 0U;
    uint8_t pattern[64], check[64];
    int r;

    if (p == 0) {
        kprintln("hwcap: selftest SKIP (no process context)");
        return;
    }

    /* ---- 1) MMIO：一页可读写假 MMIO + 一页只读假 MMIO ---- */
    mport = ipc_port_create(0U, p);
    fake = (uint32_t)(uintptr_t)alloc_pages(2U);
    if (mport <= 0 || fake == 0U) {
        if (fake != 0U) free_pages((void *)(uintptr_t)fake, 2U);
        if (mport > 0) ipc_port_release((uint32_t)mport, p);
        kprintln("hwcap: selftest FAIL (mmio setup)");
        return;
    }

    /* 假 MMIO 用 PMM 分配的两页：第一页 RW，第二页 RO。
     * grant len 通过 rights 的高 16 位传（页数），见 svc_syscall.c。 */
    r = ipc_cap_grant_ex((uint32_t)mport, IPC_CAP_MMIO, fake,
                         IPC_RIGHTS_DEFAULT, 1U, p);
    if (r == 0) {
        r = ipc_cap_grant_ex((uint32_t)mport, IPC_CAP_MMIO, fake + PAGE_SIZE,
                             IPC_RIGHT_READ, 1U, p);
    }
    if (r == 0) {
        r = hwcap_mmio_map_core(fake, &mmio_va, p);
    }
    if (r == 0) {
        r = hwcap_mmio_map_core(fake + PAGE_SIZE, &mmio_ro_va, p);
    }
    if (r == 0) {
        uint32_t flags = vmm_user_flags(mmio_va);
        uint32_t ro_flags = vmm_user_flags(mmio_ro_va);

        ok = ok && (vmm_get_phys(mmio_va) == fake);
        ok = ok && ((flags & (PTE_PRESENT | PTE_USER | PTE_WRITE |
                              PTE_NOEXEC | PTE_PCD | PTE_PWT)) ==
                    (PTE_PRESENT | PTE_USER | PTE_WRITE |
                     PTE_NOEXEC | PTE_PCD | PTE_PWT));
        ok = ok && ((ro_flags & (PTE_PRESENT | PTE_USER | PTE_WRITE)) ==
                    (PTE_PRESENT | PTE_USER));
        ok = ok && ((ro_flags & PTE_NOEXEC) != 0U);

        hwcap_selftest_pattern(pattern, sizeof(pattern));
        ok = ok && (copy_to_user((void *)(uintptr_t)mmio_va, pattern,
                                 sizeof(pattern)) == E_OK);
        memset(check, 0, sizeof(check));
        ok = ok && (copy_from_user(check, (const void *)(uintptr_t)mmio_va,
                                   sizeof(check)) == E_OK);
        ok = ok && (memcmp(pattern, check, sizeof(pattern)) == 0);

        /* 只读映射：内核写用户页必须失败（uaccess fixup 返回 E_INVAL）。 */
        ok = ok && (copy_to_user((void *)(uintptr_t)mmio_ro_va, pattern,
                                 sizeof(pattern)) != E_OK);
    }

    /* 坏 cap：未授权基址、0 基址、同 owner 重复映射都必须拒绝。 */
    {
        uint32_t bad_va = 0U;

        ok = ok && (hwcap_mmio_map_core(fake + 2U * PAGE_SIZE, &bad_va, p) == -1);
        ok = ok && (hwcap_mmio_map_core(0U, &bad_va, p) == -1);
        ok = ok && (hwcap_mmio_map_core(fake, &bad_va, p) == -1);
    }

    if (ok) {
        kprintf("hwcap: mmio map ok (cap=0x%x len=4096 rw=1 ro=1 nx=1)\n",
                (unsigned)fake);
        kprintln("hwcap: mmio bad cap rejected");
    } else {
        kprintln("hwcap: selftest FAIL (mmio)");
    }

    /* 清理 MMIO 映射；假 MMIO 页是 PMM 分配的，必须还回去。 */
    if (mmio_va != 0U) ok = hwcap_selftest_entry_unmap(fake, p) && ok;
    if (mmio_ro_va != 0U) {
        ok = hwcap_selftest_entry_unmap(fake + PAGE_SIZE, p) && ok;
    }
    free_pages((void *)(uintptr_t)fake, 2U);
    if (mport > 0) ipc_port_release((uint32_t)mport, p);
    mport = -1;

    /* ---- 2) IRQ：绑定 IRQ5（QEMU 未使用），软件触发 + wait + 超时/EINTR ---- */
    iport = ipc_port_create(0U, p);
    if (iport <= 0) {
        ok = 0;
    } else {
        r = ipc_cap_grant_ex((uint32_t)iport, IPC_CAP_IRQ, 5U, 0U, 0U, p);
        if (r == 0) r = hwcap_irq_bind((uint32_t)iport, 5U, p);
        if (r != 0) {
            ok = 0;
            kprintln("hwcap: selftest FAIL (irq bind)");
        } else {
            /* 软件触发 IRQ5 的 IDT 桩（不碰 0/1/4/11/12）。 */
            __asm__ volatile ("int $0x25" ::: "memory");

            {
                uint8_t frame[64];
                uint32_t ev_irq = 0U;
                int n = ipc_port_recv((uint32_t)iport, frame, sizeof(frame), p);

                if (n >= (int)(IPC_HDR_SIZE + sizeof(ev_irq))) {
                    memcpy(&ev_irq, frame + IPC_HDR_SIZE, sizeof(ev_irq));
                }
                ok = ok && (n == (int)(IPC_HDR_SIZE + 12U));
                ok = ok && (ev_irq == 5U);
            }

            ok = ok && (hwcap_irq_wait(5U, 50U, p) == 5);
            /* 超时路径：没有事件 -> -EAGAIN（5 tick = 50ms）。 */
            ok = ok && (hwcap_irq_wait(5U, 5U, p) == -IPC_EAGAIN);
            /* 未屏蔽信号打断 -> -EINTR。 */
            {
                uint32_t saved = p->pending_sig;

                p->pending_sig = (uint32_t)SIGINT;
                ok = ok && (hwcap_irq_wait(5U, 50U, p) == -IPC_EINTR);
                p->pending_sig = saved;
            }
            /* 风暴保护：pending 未 ack 时连续 9 次触发，
             * 第 8 次丢弃后应把线掩掉；ack 后重新放行。 */
            for (uint32_t i = 0; i < 9U; i++) {
                __asm__ volatile ("int $0x25" ::: "memory");
            }
            {
                uint32_t dropped = 0U;

                ok = ok && hwcap_selftest_irq_dropped(5U, &dropped);
                ok = ok && (dropped >= HWCAP_IRQ_STORM_LIMIT);
                ok = ok && hwcap_selftest_irq_masked(5U);
            }
            /* 9 次触发都计数了；SYS_IRQ_WAIT 每次消费一个事件，最后一次
             * 才把 pending 清掉并重新放行 IRQ。 */
            for (uint32_t i = 0; i < 9U; i++) {
                ok = ok && (hwcap_irq_wait(5U, 50U, p) == 5);
            }
            ok = ok && !hwcap_selftest_irq_masked(5U);

            /* 把 9 条事件消息收干净（事件已由 IRQ_WAIT 计数消费，消息仅通知）。 */
            for (uint32_t i = 0; i < 9U; i++) {
                uint8_t frame[64];

                (void)ipc_port_recv((uint32_t)iport, frame, sizeof(frame), p);
            }

            if (ok) {
                kprintf("hwcap: irq bind/wait ok (irq=5 guard=%u)\n",
                        (unsigned)HWCAP_IRQ_STORM_LIMIT);
            } else {
                kprintln("hwcap: selftest FAIL (irq)");
            }

            /* 端口释放必须解绑并恢复默认屏蔽。 */
            ipc_port_release((uint32_t)iport, p);
            iport = -1;
            ok = ok && !hwcap_selftest_irq_bound(5U);
        }
    }

    /* ---- 3) DMA：CAP_DMA 配额 + 分配/写图案/释放/重复释放 ---- */
    dport = ipc_port_create(0U, p);
    if (dport <= 0) {
        ok = 0;
    } else {
        r = ipc_cap_grant_ex((uint32_t)dport, IPC_CAP_DMA, HWCAP_DMA_MAX,
                             IPC_RIGHTS_DEFAULT, 0U, p);
        if (r != 0) {
            ok = 0;
        } else {
            int cap = hwcap_dma_alloc_core(8192U, PAGE_SIZE, &dma_va, p);

            if (cap <= 0) {
                ok = 0;
            } else {
                uint32_t flags = vmm_user_flags(dma_va);

                dma_cap = (uint32_t)cap;
                ok = ok && ((flags & (PTE_PRESENT | PTE_USER | PTE_WRITE |
                                      PTE_NOEXEC)) ==
                            (PTE_PRESENT | PTE_USER | PTE_WRITE | PTE_NOEXEC));
                ok = ok && (vmm_get_phys(dma_va) % PAGE_SIZE == 0U);

                hwcap_selftest_pattern(pattern, sizeof(pattern));
                ok = ok && (copy_to_user((void *)(uintptr_t)dma_va, pattern,
                                         sizeof(pattern)) == E_OK);
                memset(check, 0, sizeof(check));
                ok = ok && (copy_from_user(check,
                                           (const void *)(uintptr_t)dma_va,
                                           sizeof(check)) == E_OK);
                ok = ok && (memcmp(pattern, check, sizeof(pattern)) == 0);

                ok = ok && (hwcap_dma_free(dma_cap, p) == 0);
                /* 第二次释放同一个 cap：必须失败（double free 拒绝）。 */
                ok = ok && (hwcap_dma_free(dma_cap, p) == -1);

                /* 未授权/超限/对齐非法也必须拒绝。 */
                ok = ok && (hwcap_dma_alloc_core(HWCAP_DMA_MAX + 1U,
                                                 PAGE_SIZE, &dma_va, p) == -1);
                ok = ok && (hwcap_dma_alloc_core(4096U, 512U,
                                                 &dma_va, p) == -1);
                ok = ok && (hwcap_dma_free(0xDEADBEEFU, p) == -1);
            }
        }
        ipc_port_release((uint32_t)dport, p);
        dport = -1;
    }

    if (ok) {
        kprintln("hwcap: dma alloc/free ok (len=8192 align=4096)");
        kprintln("hwcap: dma double free rejected");
        kprintln("hwcap: selftest PASS");
    } else {
        kprintln("hwcap: selftest FAIL");
    }
}
