/* =============================================================================
 * filemap.c - 文件映射 mmap 核心（一次性读入 + 只读/MAP_PRIVATE 映射）
 *
 * 语义与限制见 filemap.h。这里不依赖 fs：数据通过 mm_file_reader_t 注入，
 * 生产路径由 mm_syscall.c 提供 fs_read_ex() 适配器，自检用内存源。
 * ============================================================================= */
#include "filemap.h"
#include "vmm.h"
#include "pmm.h"
#include "user_layout.h"
#include "../include/types.h"
#include "include/kernel.h"

/* 丢弃 offset 字节时的固定缓冲：不占大内核栈，也不为 skip 分配页面。 */
#define FILEMAP_SCRATCH 256U

static uint32_t filemap_map_flags(uint32_t prot)
{
    uint32_t flags = PTE_PRESENT | PTE_USER;

    /* 默认 NX（数据页）；显式 PROT_EXEC 才允许执行。
     * 带写权限 → MAP_PRIVATE 的 COW 私有副本；否则真正只读（写 -> #PF）。 */
    if ((prot & VMM_PROT_EXEC) == 0U) flags |= PTE_NOEXEC;
    if ((prot & VMM_PROT_WRITE) != 0U) flags |= PTE_COW;
    return flags;
}

void mm_unmap_range(uint32_t base, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        uint32_t ph = vmm_get_phys(base + off);

        (void)vmm_unmap_page(base + off);
        if (ph != 0U) pmm_free_pages((void *)(uintptr_t)ph, 1);
    }
}

int mm_file_map(uint32_t base, uint32_t len, uint32_t offset,
                mm_file_reader_t reader, void *ctx, uint32_t prot)
{
    uint8_t scratch[FILEMAP_SCRATCH];
    uint32_t skip = offset;
    uint32_t flags;
    int eof = 0;

    if (reader == 0 || len == 0U) return -1;
    if ((base & 0xFFFU) != 0U || (len & 0xFFFU) != 0U ||
        (offset & 0xFFFU) != 0U) {
        return -1;
    }
    flags = filemap_map_flags(prot);

    /* fs 层没有 lseek：按“顺序读 + 丢弃”模拟映射起点。 */
    while (skip > 0U) {
        uint32_t want = (skip < FILEMAP_SCRATCH) ? skip : FILEMAP_SCRATCH;
        int n = reader(ctx, scratch, want);

        if (n < 0) return -1;
        if (n == 0) { eof = 1; break; }             /* 文件比 offset 短：后面零填充 */
        if ((uint32_t)n > want) return -1;
        skip -= (uint32_t)n;
    }

    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        void *page = pmm_alloc_page();
        uint32_t filled = 0;

        if (page == 0) {
            mm_unmap_range(base, off);
            return -1;
        }
        memset(page, 0, PAGE_SIZE);

        while (!eof && filled < PAGE_SIZE) {
            int n = reader(ctx, (uint8_t *)page + filled, PAGE_SIZE - filled);

            if (n < 0 || (uint32_t)n > PAGE_SIZE - filled) {
                pmm_free_pages(page, 1);
                mm_unmap_range(base, off);
                return -1;
            }
            if (n == 0) { eof = 1; break; }
            filled += (uint32_t)n;
        }

        if (vmm_map_user_page(base + off, (uint32_t)(uintptr_t)page,
                              flags) != 0) {
            pmm_free_pages(page, 1);
            mm_unmap_range(base, off);
            return -1;
        }
    }
    return 0;
}

/* =============================================================================
 * 自检
 *
 * 校验（全部在内核态完成，不碰 SMAP：内容通过页表查到的物理页比较，
 * 恒等映射页没有 USER 位）：
 *   1. 只读映射 3 页：前两页整页、第三页 100 字节 + 零填充；
 *      页表无 WRITE/COW（写会被拒绝）；
 *   2. offset=1 页：映射内容来自文件第 2 页，reader 恰好消费 offset+len；
 *   3. MAP_PRIVATE 可写映射带 PTE_COW，物理页是私有副本（写不影响文件源）；
 *      这里不调用 vmm_cow_fault()，避免抢走 build.ps1 用来验证 fork COW 的
 *      "vmm: first COW copy" 首次打印；
 *   4. munmap 后页数归零、PMM 账目与异常计数复原。
 * ============================================================================= */
typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint32_t       pos;
} mm_mem_src_t;

static int mm_mem_read(void *ctx, void *buf, uint32_t len)
{
    mm_mem_src_t *s = (mm_mem_src_t *)ctx;
    uint32_t n;

    if (s == 0 || buf == 0) return -1;
    if (s->pos > s->size) return -1;
    n = s->size - s->pos;
    if (n > len) n = len;
    if (n == 0U) return 0;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return (int)n;
}

/* 比较 va 这一页（经页表取物理页）与文件源的 [src_off, src_off+PAGE)，
 * 超出文件末尾的部分必须是 0（零填充）。 */
static int mm_filemap_check_page(uint32_t va, const uint8_t *src,
                                 uint32_t src_off, uint32_t src_size)
{
    uintptr_t phys = vmm_get_phys(va);
    const uint8_t *p;

    if (phys == 0U) return 0;
    p = (const uint8_t *)phys;

    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        uint8_t want = (src_off + i < src_size) ? src[src_off + i] : 0U;

        if (p[i] != want) return 0;
    }
    return 1;
}

int mm_filemap_selftest(void)
{
    const uint32_t src_size = 2U * PAGE_SIZE + 100U;
    const uint32_t base = USER_LAYOUT_MMAP_BASE;
    pde_t *saved = vmm_current_dir();
    pde_t *dir = 0;
    uint8_t *src = 0;
    mm_mem_src_t ms;
    uint32_t before = pmm.free_pages;
    uint32_t faults_before = pmm_faults;
    uint32_t vmm_faults_before = vmm_faults;
    uint8_t expect;
    int ok = 1;

    dir = vmm_new_dir();
    src = (uint8_t *)vmalloc(3U * PAGE_SIZE + 128U);
    if (dir == 0 || src == 0) {
        if (dir != 0) vmm_destroy_dir(dir);
        if (src != 0) vfree(src);
        kprintln("mm: file mmap FAIL (no resources)");
        return 0;
    }

    for (uint32_t i = 0; i < 3U * PAGE_SIZE + 128U; i++) {
        src[i] = (uint8_t)((i * 7U + 3U) & 0xFFU);
    }
    vmm_switch_dir(dir);

    /* 1) 只读映射 3 页（文件 2 页 + 100 字节） */
    ms.data = src; ms.size = src_size; ms.pos = 0U;
    if (mm_file_map(base, 3U * PAGE_SIZE, 0U, mm_mem_read, &ms,
                    VMM_PROT_READ) != 0) {
        ok = 0;
    } else {
        for (uint32_t p = 0; p < 3U; p++) {
            if (!mm_filemap_check_page(base + p * PAGE_SIZE, src,
                                       p * PAGE_SIZE, src_size)) {
                ok = 0;
            }
        }
        if ((vmm_user_flags(base) & (PTE_WRITE | PTE_COW)) != 0U) ok = 0;
        if (vmm_user_page_count(dir) != 3U) ok = 0;
        if (ms.pos != src_size) ok = 0;             /* 读到 EOF 为止 */
        mm_unmap_range(base, 3U * PAGE_SIZE);
        if (vmm_user_page_count(dir) != 0U) ok = 0;
    }

    /* 2)+3) offset=1 页、len=1 页、MAP_PRIVATE 可写 -> COW 私有副本 */
    expect = src[PAGE_SIZE];
    ms.data = src; ms.size = src_size; ms.pos = 0U;
    if (mm_file_map(base, PAGE_SIZE, PAGE_SIZE, mm_mem_read, &ms,
                    VMM_PROT_READ | VMM_PROT_WRITE) != 0) {
        ok = 0;
    } else {
        if (!mm_filemap_check_page(base, src, PAGE_SIZE, src_size)) ok = 0;
        if ((vmm_user_flags(base) & PTE_COW) == 0U) ok = 0;
        if ((vmm_user_flags(base) & PTE_WRITE) != 0U) ok = 0;
        if (ms.pos != 2U * PAGE_SIZE) ok = 0;       /* 丢弃 offset + 读 len */

        /* MAP_PRIVATE 的“私有副本”语义：这块页是 mmap 时从文件读出来的私有
         * 物理页（PMM 引用计数 1），首次用户写由 vmm_cow_fault() 原地改成可写，
         * 不会回写文件。这里直接经页表物理地址写一个字节，验证文件源不变、
         * 页数不变。
         *
         * 注意：这里**故意不调用 vmm_cow_fault()**——它会增加 vmm_cow_copies
         * 并打印 "vmm: first COW copy"，而 build.ps1 用那行（地址 0x401xxxx）
         * 验证真正的 fork COW。boot 自检不能把“第一次”抢走。真实写缺页路径
         * 由既有用户态 fork 回归覆盖。 */
        {
            uintptr_t phys = vmm_get_phys(base);

            if (phys == 0U) {
                ok = 0;
            } else {
                volatile uint8_t *p = (volatile uint8_t *)phys;

                p[0] = 0xEEU;
                if (p[0] != 0xEEU) ok = 0;
            }
            if (src[PAGE_SIZE] != expect) ok = 0;   /* 文件内容不被写 */
        }
        if (vmm_user_page_count(dir) != 1U) ok = 0;
        mm_unmap_range(base, PAGE_SIZE);
        if (vmm_user_page_count(dir) != 0U) ok = 0;
    }

    vmm_switch_dir(saved);
    vmm_destroy_dir(dir);
    vfree(src);

    if (pmm.free_pages != before) ok = 0;
    if (pmm_faults != faults_before) ok = 0;
    if (vmm_faults != vmm_faults_before) ok = 0;

    if (ok) {
        kprintln("mm: file mmap ok (read-only MAP_PRIVATE, offset+len, cow private)");
    } else {
        kprintln("mm: file mmap FAIL");
    }
    return ok;
}
