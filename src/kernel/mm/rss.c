/* =============================================================================
 * rss.c - per-process RSS（常驻用户页）统计
 *
 * 设计见 rss.h：RSS 直接由页表推导（扫描 PRESENT|USER 的 4KB 页），不在
 * fork/exit/munmap 各处埋增量计数器。这样：
 *   - 进程退出/地址空间销毁（proc_reap → vmm_destroy_dir）后进程表项变
 *     PROC_FREE、page_dir 归零，RSS 自然为 0，不会留下悬挂计数；
 *   - COW：fork 时子进程复制父进程的 PTE（每页各算 1 次）；写缺页把共享页
 *     换成私有副本，页数不变；
 *   - munmap / 释放：PTE 清 0 后不再计入。
 * 代价是每次查询 O(用户区 PDE×PTE) ≈ 最多 1536 项，只有 res/ps 这类展示
 * 路径会调用，可以忽略。
 * ============================================================================= */
#include "rss.h"
#include "vmm.h"
#include "pmm.h"
#include "user_layout.h"
#include "../include/types.h"
#include "include/kernel.h"
#include "include/proc.h"

uint32_t mm_rss_pages(const struct process *p)
{
    if (p == 0) return 0;
    if (p->state == PROC_FREE) return 0;
    if (p->page_dir == 0) return 0;

    return vmm_user_page_count((pde_t *)p->page_dir);
}

uint32_t mm_rss_bytes(const struct process *p)
{
    return mm_rss_pages(p) * (uint32_t)PAGE_SIZE;
}

uint32_t mm_rss_total_pages(void)
{
    uint32_t total = 0;

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        total += mm_rss_pages(proc_at(i));
    }
    return total;
}

void mm_rss_print(const struct process *p)
{
    if (p == 0 || p->state == PROC_FREE) return;

    kprintf("mm: ps pid=%u name=%s state=%u rss_pages=%u rss_kb=%u\n",
            (unsigned int)p->pid,
            (p->name != 0) ? p->name : "?",
            (unsigned int)p->state,
            (unsigned int)mm_rss_pages(p),
            (unsigned int)(mm_rss_pages(p) * 4U));
}

uint32_t mm_rss_dump(void)
{
    uint32_t lines = 0;

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        const proc_t *p = proc_at(i);
        if (p == 0 || p->state == PROC_FREE) continue;
        mm_rss_print(p);
        lines++;
    }
    return lines;
}

/* =============================================================================
 * 自检（在 memtest 阶段跑，此时还没有真实进程，所以用临时地址空间 +
 * 栈上假 proc_t 直接验证计数口径）
 *
 *   1. 3 个用户页 → RSS=3；
 *   2. 无 page_dir / PROC_FREE → RSS=0；
 *   3. fork COW 共享：子目录同样计 3，父进程仍计 3（共享页各计一次）；
 *   4. 取消映射并释放 1 页 → RSS=2（扣减）；
 *   5. 地址空间销毁/进程退出 → RSS=0；
 *   6. PMM 账目完全复原（页数不漏不重）。
 * ============================================================================= */
int mm_rss_selftest(void)
{
    pde_t *saved = vmm_current_dir();
    pde_t *dir = 0;
    pde_t *fork_dir = 0;
    void *page[3] = { 0, 0, 0 };
    proc_t fake;
    uint32_t before = pmm.free_pages;
    uint32_t faults_before = pmm_faults;
    int ok = 1;

    memset(&fake, 0, sizeof(fake));
    fake.pid = 0;
    fake.name = "rss-selftest";
    fake.state = PROC_READY;

    dir = vmm_new_dir();
    if (dir == 0) {
        kprintln("mm: rss accounting FAIL (no address space)");
        return 0;
    }
    vmm_switch_dir(dir);

    for (uint32_t i = 0; i < 3U; i++) {
        page[i] = pmm_alloc_page();
        if (page[i] == 0) {
            ok = 0;
            break;
        }
        memset(page[i], 0, PAGE_SIZE);
        if (vmm_map_user_page(USER_LAYOUT_MMAP_BASE + i * PAGE_SIZE,
                              (uint32_t)(uintptr_t)page[i],
                              PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
            pmm_free_pages(page[i], 1);
            page[i] = 0;
            ok = 0;
            break;
        }
    }

    /* 1. 3 页 → RSS=3 */
    fake.page_dir = dir;
    if (ok && mm_rss_pages(&fake) != 3U) ok = 0;
    if (ok && mm_rss_bytes(&fake) != 3U * (uint32_t)PAGE_SIZE) ok = 0;

    /* ps 输出格式演示（真实进程在 memtest 阶段还不存在；mm_rss_dump() 在有
     * 进程后打印同样的行）。期望行见 docs/claims/mm.md。 */
    if (ok) mm_rss_print(&fake);

    /* 2. 没有页目录 / 已退出进程 → 0 */
    fake.page_dir = 0;
    if (ok && mm_rss_pages(&fake) != 0U) ok = 0;
    fake.page_dir = dir;
    fake.state = PROC_FREE;
    if (ok && mm_rss_pages(&fake) != 0U) ok = 0;
    fake.state = PROC_READY;

    /* 3. fork COW 共享：两个地址空间各 3 页（共享页各计一次） */
    if (ok) {
        fork_dir = vmm_new_dir();
        if (fork_dir == 0) {
            ok = 0;
        } else if (vmm_clone_user_pages(fork_dir) != 0) {
            ok = 0;
        } else {
            if (vmm_user_page_count(dir) != 3U) ok = 0;
            fake.page_dir = fork_dir;
            if (mm_rss_pages(&fake) != 3U) ok = 0;      /* 子进程 */
            fake.page_dir = dir;
            if (mm_rss_pages(&fake) != 3U) ok = 0;      /* 父进程仍 3 */
        }
    }

    /* 4. 释放 1 页（munmap/释放路径）→ 2；随后 5. 退出 → 0 */
    if (ok) {
        uint32_t ph = vmm_get_phys(USER_LAYOUT_MMAP_BASE);

        if (vmm_unmap_page(USER_LAYOUT_MMAP_BASE) != 0) ok = 0;
        if (ph != 0U) pmm_free_pages((void *)(uintptr_t)ph, 1);
        page[0] = 0;
        if (mm_rss_pages(&fake) != 2U) ok = 0;
    }

    /* 先回到调用者的地址空间，再销毁临时目录（不能销毁正在使用的 CR3）。 */
    vmm_switch_dir(saved);
    if (fork_dir != 0) vmm_destroy_dir(fork_dir);
    fake.page_dir = 0;                                  /* 模拟 proc_reap 清槽 */
    if (mm_rss_pages(&fake) != 0U) ok = 0;
    vmm_destroy_dir(dir);

    /* 6. 账目与异常计数复原 */
    if (pmm.free_pages != before) ok = 0;
    if (pmm_faults != faults_before) ok = 0;

    if (ok) {
        kprintln("mm: rss accounting ok (pages=3, cow shared counted per process)");
    } else {
        kprintln("mm: rss accounting FAIL");
    }
    return ok;
}
