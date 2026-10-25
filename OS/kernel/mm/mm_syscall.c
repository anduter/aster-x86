/* =============================================================================
 * mm/mm_syscall.c - 内存相关系统调用（M2）
 *
 * 从 proc/trap/syscall.c 拆出的 mm 组：brk / mmap / munmap。只搬代码不改逻辑。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/proc.h"
#include "mm_syscall.h"
#include "../proc/trap/syscall_internal.h"

/* 用户堆：布局由 user_layout.h 统一提供（0x40200000 起，1MB 上限）。 */
#define USER_HEAP_BASE USER_LAYOUT_HEAP_BASE
#define USER_HEAP_MAX  USER_LAYOUT_HEAP_END

static int sys_brk_user(uint32_t inc)
{
    proc_t *p = proc_current();
    uint32_t old, neu;

    if (p == 0) return -1;

    old = (p->user_brk != 0U) ? p->user_brk
                              : (p->user_heap_base != 0U ? p->user_heap_base : USER_HEAP_BASE);
    neu = old + inc;

    if (neu > USER_HEAP_MAX) return -1;

    for (uint32_t va = old & 0xFFFFF000U; va < neu; va += 4096U) {
        void *phys;
        if (vmm_get_phys(va) != 0U) continue;       /* 已映射 */

        phys = pmm_alloc_page();
        if (phys == 0) return -1;
        memset(phys, 0, 4096U);

        if (vmm_map_user_page(va, (uint32_t)(uintptr_t)phys,
                              PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
            return -1;
        }
    }

    p->user_brk = neu;
    return (int)neu;
}

/* mprotect(addr, len, prot)：只改当前进程用户页的 PTE 权限。
 * 先整段校验（对齐/范围/已映射），再逐页修改；任何非法参数返回 -1 且不改页。
 * prot 位与 VMM_PROT_*（= include/abi.h 的 PROT_*）一致。 */
static int sys_mprotect_user(uint32_t addr, uint32_t len, uint32_t prot)
{
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (len == 0U) return -1;
    if ((addr & 0xFFFU) != 0U) return -1;
    if ((prot & ~(uint32_t)(VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_EXEC)) != 0U) {
        return -1;
    }

    /* 用户地址空间上界是 mmap 区末尾；下界是代码段起点 */
    if (addr < USER_LAYOUT_CODE_BASE) return -1;
    if (addr >= USER_LAYOUT_MMAP_END) return -1;
    if (len > USER_LAYOUT_MMAP_END - addr) return -1;

    /* 第一遍：整段都必须是已映射的用户页，避免改到一半才发现非法 */
    for (uint32_t va = addr; va < addr + len; va += PAGE_SIZE) {
        if ((vmm_user_flags(va) & (PTE_PRESENT | PTE_USER)) !=
            (PTE_PRESENT | PTE_USER)) {
            return -1;
        }
    }

    /* 第二遍：逐页改权限；vmm_protect_page 内部做 COW 私有化和 invlpg */
    for (uint32_t va = addr; va < addr + len; va += PAGE_SIZE) {
        if (vmm_protect_page(va, prot) != 0) return -1;
    }

    return 0;
}

bool mm_syscall_owns(uint32_t nr)
{
    return nr == SYS_BRK || nr == SYS_MMAP || nr == SYS_MUNMAP ||
           nr == SYS_MPROTECT;
}

int mm_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {
    case SYS_BRK:
        ret = sys_brk_user(f->ebx);
        break;
    case SYS_MMAP: {
        proc_t *p = proc_current();
        uint32_t len = (f->ebx + 0xFFFU) & ~0xFFFU;
        uint32_t slot = PROC_MMAP_MAX;

        if (p == 0 || len == 0U ||
            len > USER_LAYOUT_MMAP_END - USER_LAYOUT_MMAP_BASE ||
            p->mmap_next + len > USER_LAYOUT_MMAP_END - USER_LAYOUT_MMAP_BASE) {
            ret = -1;
            break;
        }
        for (uint32_t i = 0; i < PROC_MMAP_MAX; i++) {
            if (p->mmaps[i].len == 0U) { slot = i; break; }
        }
        if (slot == PROC_MMAP_MAX) { ret = -1; break; }

        uint32_t base = USER_LAYOUT_MMAP_BASE + p->mmap_next;
        for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
            void *page = pmm_alloc_page();
            if (page == 0) {
                for (uint32_t j = 0; j < off; j += PAGE_SIZE) {
                    uint32_t ph = vmm_get_phys(base + j);
                    vmm_unmap_page(base + j);
                    if (ph != 0U) pmm_free_page((void *)(uintptr_t)ph);
                }
                ret = -1;
                goto mmap_out;
            }
            memset(page, 0, PAGE_SIZE);
            if (vmm_map_user_page(base + off, (uint32_t)(uintptr_t)page,
                                  PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NOEXEC) != 0) {
                pmm_free_page(page);
                for (uint32_t j = 0; j < off; j += PAGE_SIZE) {
                    uint32_t ph = vmm_get_phys(base + j);
                    vmm_unmap_page(base + j);
                    if (ph != 0U) pmm_free_page((void *)(uintptr_t)ph);
                }
                ret = -1;
                goto mmap_out;
            }
        }
        p->mmaps[slot].base = base;
        p->mmaps[slot].len = len;
        p->mmap_next += len;
        ret = (int)base;
mmap_out:
        break;
    }
    case SYS_MUNMAP: {
        proc_t *p = proc_current();
        uint32_t addr = f->ebx;
        uint32_t len = (f->ecx + 0xFFFU) & ~0xFFFU;
        uint32_t slot = PROC_MMAP_MAX;

        if (p == 0 || len == 0U) { ret = -1; break; }
        for (uint32_t i = 0; i < PROC_MMAP_MAX; i++) {
            if (p->mmaps[i].len == len && p->mmaps[i].base == addr) {
                slot = i;
                break;
            }
        }
        if (slot == PROC_MMAP_MAX) { ret = -1; break; }

        for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
            uint32_t ph = vmm_get_phys(addr + off);
            vmm_unmap_page(addr + off);
            if (ph != 0U) pmm_free_page((void *)(uintptr_t)ph);
        }
        p->mmaps[slot].base = 0;
        p->mmaps[slot].len = 0;
        ret = 0;
        break;
    }
    case SYS_MPROTECT:
        ret = sys_mprotect_user(f->ebx, f->ecx, f->edx);
        break;
    default:
        return -1000;
    }
    return ret;
}
