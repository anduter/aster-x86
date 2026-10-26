/* =============================================================================
 * mm/mm_syscall.c - 内存相关系统调用（M2）
 *
 * 从 proc/trap/syscall.c 拆出的 mm 组：brk / mmap / munmap。只搬代码不改逻辑。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/proc.h"
#include "include/fs.h"
#include "mm_syscall.h"
#include "filemap.h"
#include "brk_range.h"
#include "../proc/trap/syscall_internal.h"

/* 用户堆：布局由 user_layout.h 统一提供（0x40200000 起，1MB 上限）。 */
#define USER_HEAP_BASE USER_LAYOUT_HEAP_BASE
#define USER_HEAP_MAX  USER_LAYOUT_HEAP_END

/* =============================================================================
 * SYS_MMAP 参数扩展（不新增 syscall 编号、不改 include/abi.h；语义见
 * docs/claims/mm.md）：
 *
 *   ebx = len（字节，内部向上取整到页）
 *   ecx = prot | flags | (offset_pages << 16)
 *           bits 0..7   PROT_*（复用 abi.h 的 PROT_READ/WRITE/EXEC）
 *           bit  8      MMAP_F_FILE      文件后端（edx = fd）
 *           bit  9      MMAP_F_PRIVATE   MAP_PRIVATE（文件映射必须置位）
 *           bits 16..31 offset 页数（仅 MAP_FILE；0 = 文件开头）
 *   edx = fd（仅 MAP_FILE；0 = 匿名映射，兼容旧调用）
 *
 * 旧匿名调用（ecx = 0 或 3、edx = 0，例如用户态 sectest 的 mmap/mprotect）
 * 行为完全不变：prot/flags 高位都忽略，映射 RW+NX 零页。
 * ========================================================================== */
#define MMAP_F_FILE      0x100U
#define MMAP_F_PRIVATE   0x200U
#define MMAP_PROT_MASK   0x0FFU

/* 文件映射的数据源：把一个进程 fd 适配成 mm_file_reader_t。
 * fs 层句柄是顺序读，offset 由 mm_file_map() 用“读+丢弃”模拟。 */
typedef struct {
    uint8_t be;
    int     handle;
} mm_fs_reader_t;

static int mm_fs_read(void *ctx, void *buf, uint32_t len)
{
    mm_fs_reader_t *r = (mm_fs_reader_t *)ctx;
    int n;

    if (r == 0 || len == 0U) return (len == 0U) ? 0 : -1;
    n = fs_read_ex(r->be, r->handle, buf, len);
    return (n < 0) ? -1 : n;
}

/* 匿名映射：保持既有行为（零页、RW+NX），失败回滚已映射部分。 */
static int mmap_anon(uint32_t base, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        void *page = pmm_alloc_page();

        if (page == 0) {
            mm_unmap_range(base, off);
            return -1;
        }
        memset(page, 0, PAGE_SIZE);

        if (vmm_map_user_page(base + off, (uint32_t)(uintptr_t)page,
                              PTE_PRESENT | PTE_WRITE | PTE_USER |
                              PTE_NOEXEC) != 0) {
            pmm_free_pages(page, 1);
            mm_unmap_range(base, off);
            return -1;
        }
    }
    return 0;
}

static int sys_brk_user(uint32_t inc)
{
    proc_t *p = proc_current();
    uint32_t old, neu, floor;

    if (p == 0) return -1;

    old = (p->user_brk != 0U) ? p->user_brk
                              : (p->user_heap_base != 0U ? p->user_heap_base : USER_HEAP_BASE);
    floor = (p->user_heap_base != 0U) ? p->user_heap_base : USER_HEAP_BASE;
    /* 旧状态可能低于堆基址（修复前 brk 回绕留下的值 / 跨 exec 的旧 brk）：
     * 以堆基址为起点恢复，避免把 brk 再往下带。 */
    if (old < floor) old = floor;

    /* 关键：old + inc 是 32 位无符号加法。inc = 0xC0000000、old = 0x40220000
     * 时会回绕成 0x00022000（内核恒等映射区），旧代码只判 neu > 堆上限，
     * 于是把“成功”的 brk 返回值设到内核地址，后续 brk 还按它推进。
     * 这里用纯函数同时挡住正向越界/回绕和负向越过堆底。 */
    if (!brk_range_ok(old, inc, floor, USER_HEAP_MAX)) return -1;
    neu = old + inc;
    if (neu < floor || neu > USER_HEAP_MAX) return -1;   /* 双保险 */

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
        uint32_t req = f->ecx;
        uint32_t prot = req & MMAP_PROT_MASK;
        uint32_t flags = req & 0xFF00U;
        uint32_t len = (f->ebx + 0xFFFU) & ~0xFFFU;
        uint32_t slot = PROC_MMAP_MAX;
        uint32_t base;

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

        base = USER_LAYOUT_MMAP_BASE + p->mmap_next;

        if ((flags & MMAP_F_FILE) != 0U) {
            /* 文件后端（只读 MAP_PRIVATE 起步）：fd 必须可读且是普通文件。 */
            int fd = (int)f->edx;
            uint32_t offset = ((req >> 16) & 0xFFFFU) << PAGE_SHIFT;
            proc_fd_t *fdp;
            mm_fs_reader_t reader;

            if ((flags & MMAP_F_PRIVATE) == 0U) { ret = -1; break; }
            if ((prot & ~(uint32_t)(VMM_PROT_READ | VMM_PROT_WRITE |
                                    VMM_PROT_EXEC)) != 0U) {
                ret = -1;
                break;
            }
            /* offset 用“顺序读+丢弃”实现，限制在单文件上限内以保证 O(64KB)。 */
            if (fd <= 0 || offset > FS_FILE_MAX) { ret = -1; break; }

            fdp = fd_get(p, fd);
            if (fdp == 0 || fdp->kind != FD_KIND_FILE ||
                (fdp->rights & FD_RIGHT_READ) == 0U) {
                ret = -1;
                break;
            }

            reader.be = fdp->fs_be;
            reader.handle = (int)fdp->handle;
            if (mm_file_map(base, len, offset, mm_fs_read, &reader, prot) != 0) {
                ret = -1;
                break;
            }
        } else if (mmap_anon(base, len) != 0) {
            ret = -1;
            break;
        }

        p->mmaps[slot].base = base;
        p->mmaps[slot].len = len;
        p->mmap_next += len;
        ret = (int)base;
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

        mm_unmap_range(addr, len);
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
