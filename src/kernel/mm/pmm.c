#include "pmm.h"
#include "../io.h"
#include "../types.h"
#include "include/mm.h"
#include "include/kernel.h"

/* 物理页分配器的锁：位图、rover、free_pages、引用计数都受它保护 */
static spinlock_t pmm_lock = SPINLOCK_INIT;

PhysicalMemoryManager pmm;
/* 非法释放等异常累计次数，自检时用来判断内存管理是否健康 */
unsigned int pmm_faults;

/* 物理内存基准地址：所有分配地址都以它为起点计算
   （关键：页号 0 对应的就是这块地址，而不是物理地址 0） */
static uint8_t* memory_start;

/* 搜索起点（rover）：每次分配成功后把起点移到分配区之后，
   下一次从这里开始找，避免每次都从第 0 页重新扫一遍。 */
static size_t pmm_rover;
static size_t pmm_bitmap_pages;

/* 打印一条告警：pmm: WARN <说明> at 0x<地址> */
static void pmm_warn(const char *what, uintptr_t addr)
{
    pmm_faults++;
    serial_puts("pmm: WARN ");
    serial_puts(what);
    serial_puts(" at ");
    serial_puthex32((unsigned int)addr);
    serial_puts("\r\n");
}

/* ==================== 位图操作 ==================== */
static void bitmap_set(size_t idx)
{
    pmm.bitmap[idx >> 3] |= (1 << (idx & 7));
}

static void bitmap_clear(size_t idx)
{
    pmm.bitmap[idx >> 3] &= ~(1 << (idx & 7));
}

static int bitmap_test(size_t idx)
{
    return pmm.bitmap[idx >> 3] & (1 << (idx & 7));
}

/* 内核堆是 PMM 管理区内的保留洞；任何扫描都不能把它当空闲页。 */
static int page_is_used(size_t idx)
{
    uintptr_t phys = (uintptr_t)memory_start + (idx << PAGE_SHIFT);

    /* 位图页永远不能被分配，否则位图会被 memset 清零。 */
    if (idx < pmm_bitmap_pages) return 1;
    if (phys >= (uintptr_t)HEAP_BASE &&
        phys < (uintptr_t)HEAP_BASE + (uintptr_t)HEAP_SIZE) {
        return 1;
    }
    return bitmap_test(idx) ? 1 : 0;
}

/* =============================================================================
 * find_free_run - 查找 count 个连续空闲页，返回起始页号，失败返回 (size_t)-1
 *
 * 唯一的分配扫描路径：按页线性 first-fit，从 rover 开始，扫到末尾后
 * 回绕到 0 继续扫到 rover。保留区（PMM 位图、内核堆）由 page_is_used()
 * 显式排除，因此不依赖优化扫描与位图的一致性。
 *
 * 之前用“32 位字整段跳过”的优化扫描，出现过返回已标记页的 bug；
 * 这里不再保留优化路径，宁可慢一点也保证正确。
 * ============================================================================= */
static size_t find_free_run(size_t count)
{
    size_t total = pmm.total_pages;
    size_t start = (pmm_rover < total) ? pmm_rover : 0;
    size_t run_start = 0;
    size_t run_len = 0;

    for (int pass = 0; pass < 2; pass++) {
        size_t from = (pass == 0) ? start : 0;
        size_t to   = (pass == 0) ? total : start;

        for (size_t i = from; i < to; i++) {
            if (page_is_used(i)) {
                run_len = 0;
                continue;
            }
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= count) return run_start;
        }
        run_len = 0;
    }

    return (size_t)-1;
}

/* ==================== 初始化 ==================== */
void pmm_init(uint8_t* bitmap_addr, size_t mem_size)
{
    uintptr_t base = (uintptr_t)bitmap_addr;

    memory_start = bitmap_addr;
    pmm.bitmap = bitmap_addr;
    pmm.phys_base = (unsigned int)base;
    pmm_rover = 0;

    /* 页号 0 就是位图所在的物理地址（2MB），所以页数只能统计它上面的
       那部分内存。若从 0 开始统计，就会把 base+mem_size 以内的页都算进来，
       而最后 2MB 超出了 vmm_init() 建立的恒等映射范围，
       PMM 一旦把这些页分配出去就必然触发 #PF。 */
    if (mem_size <= base) {
        pmm.total_pages = 0;
        pmm.free_pages = 0;
        return;
    }

    pmm.total_pages = (mem_size - base) >> PAGE_SHIFT;
    pmm.free_pages = pmm.total_pages;

    size_t bitmap_bytes = (pmm.total_pages + 7) >> 3;

    /* 位图初始全为 0（空闲） */
    for (size_t i = 0; i < bitmap_bytes; i++) {
        pmm.bitmap[i] = 0;
    }

    /* 把位图自身占用的页标记为已用 */
    size_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
    pmm_bitmap_pages = bitmap_pages;
    for (size_t i = 0; i < bitmap_pages; i++) {
        bitmap_set(i);
        pmm.free_pages--;
    }
}

/* ==================== 物理页引用计数（COW 用） ==================== */
/* 引用计数覆盖 32 位物理地址空间的上限（4GB / 4KB = 1,048,576 页）。
 * 之前只跟踪 128MB，超过部分 COW 会误判为独占页；这里覆盖全部可管理页。 */
#define PMM_REF_PAGES (4U * 1024U * 1024U)

static uint8_t page_ref[PMM_REF_PAGES];

static size_t ref_index(uintptr_t phys)
{
    uintptr_t base = (uintptr_t)memory_start;

    if (phys < base) return (size_t)-1;
    size_t idx = (phys - base) >> PAGE_SHIFT;
    return (idx < PMM_REF_PAGES) ? idx : (size_t)-1;
}

unsigned int pmm_page_ref(uint32_t phys)
{
    size_t idx = ref_index(phys);

    return (idx == (size_t)-1) ? 0U : (unsigned int)page_ref[idx];
}

int pmm_is_used(unsigned int phys)
{
    uintptr_t base = (uintptr_t)memory_start;

    if (phys < base) return 1;
    size_t idx = ((uintptr_t)phys - base) >> PAGE_SHIFT;
    if (idx >= pmm.total_pages) return 1;
    return page_is_used(idx);
}

int pmm_selftest(void)
{
    void *pages[256];
    uint32_t before = pmm.free_pages;

    for (uint32_t i = 0; i < 256U; i++) pages[i] = 0;

    if (!pmm_is_used(0x00200000U) || !pmm_is_used(HEAP_BASE)) return 0;

    for (uint32_t i = 0; i < 256U; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == 0) goto fail;
        if (pmm_is_used((unsigned int)(uintptr_t)pages[i]) != 1) goto fail;
        if ((uintptr_t)pages[i] >= (uintptr_t)HEAP_BASE &&
            (uintptr_t)pages[i] < (uintptr_t)HEAP_BASE + HEAP_SIZE) goto fail;
        for (uint32_t j = 0; j < i; j++) {
            if (pages[j] == pages[i]) goto fail;
        }
    }

    for (uint32_t i = 0; i < 256U; i++) pmm_free_page(pages[i]);
    return (pmm.free_pages == before) ? 1 : 0;

fail:
    for (uint32_t i = 0; i < 256U; i++) {
        if (pages[i] != 0) pmm_free_page(pages[i]);
    }
    return 0;
}

void pmm_page_share(uint32_t phys)
{
    uint32_t flags = spin_lock_irqsave(&pmm_lock);
    size_t idx = ref_index(phys);

    if (idx != (size_t)-1) {
        if (page_ref[idx] < 255U) page_ref[idx]++;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

/* 分配/释放都要过 pmm_lock：位图、rover、free_pages 都是共享状态 */
static void* alloc_pages_locked(size_t count);
static void  free_pages_locked(void* base_addr, size_t count);

void* pmm_alloc_pages(size_t count)
{
    uint32_t flags = spin_lock_irqsave(&pmm_lock);
    void *r = alloc_pages_locked(count);

    spin_unlock_irqrestore(&pmm_lock, flags);
    return r;
}

void pmm_free_pages(void* base_addr, size_t count)
{
    uint32_t flags = spin_lock_irqsave(&pmm_lock);

    free_pages_locked(base_addr, count);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

/* ==================== 连续多页分配 ==================== */
static void* alloc_pages_locked(size_t count)
{
    if (count == 0 || count > pmm.free_pages) {
        return 0;
    }

    size_t start_idx = find_free_run(count);
    if (start_idx == (size_t)-1) {
        return 0;   /* 内存足够但碎片化，找不到连续段 */
    }

    /* find_free_run 已对每个候选页调用 page_is_used()，无需二次兜底。 */

    for (size_t j = start_idx; j < start_idx + count; j++) {
        bitmap_set(j);
        if (j < PMM_REF_PAGES) page_ref[j] = 1U;
    }
    pmm.free_pages -= count;

    /* 下一次从这次分配结束的位置继续找 */
    pmm_rover = start_idx + count;
    if (pmm_rover >= pmm.total_pages) {
        pmm_rover = 0;
    }

    /* 返回真实物理地址 = 基准地址 + 页号 * 页大小 */
    return (void*)(memory_start + (start_idx << PAGE_SHIFT));
}

/* ==================== 连续多页释放（带合法性校验） ==================== */
static void free_pages_locked(void* base_addr, size_t count)
{
    if (count == 0 || base_addr == 0) return;

    uintptr_t addr = (uintptr_t)base_addr;
    uintptr_t base = (uintptr_t)memory_start;

    /* 堆是保留区，任何释放请求都不能把 heap 页还给 PMM。 */
    if (addr < (uintptr_t)HEAP_BASE + (uintptr_t)HEAP_SIZE &&
        addr + count * PAGE_SIZE > (uintptr_t)HEAP_BASE) {
        pmm_warn("free reserved heap", addr);
        return;
    }

    /* 位图页本身也是永久保留区。 */
    if (addr < (uintptr_t)memory_start + pmm_bitmap_pages * PAGE_SIZE) {
        pmm_warn("free reserved bitmap", addr);
        return;
    }

    /* 只接受按页对齐、且落在管理区间内的地址，越界和错位都算异常 */
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        pmm_warn("unaligned free", addr);
        return;
    }
    if (addr < base) {
        pmm_warn("free below managed base", addr);
        return;
    }

    /* 物理地址 -> 相对基准地址的页号 */
    size_t start_idx = (addr - base) >> PAGE_SHIFT;

    if (start_idx >= pmm.total_pages) {
        pmm_warn("free beyond managed end", addr);
        return;
    }
    if (start_idx + count > pmm.total_pages) {
        pmm_warn("free range crosses managed end", addr);
        count = pmm.total_pages - start_idx;
    }

    for (size_t i = start_idx; i < start_idx + count; i++) {
        if (!bitmap_test(i)) {
            /* 这一页本来就是空闲的：重复释放 */
            pmm_warn("double free", base + (i << PAGE_SHIFT));
            continue;
        }
        /* 还有别的页表引用它（COW 共享）：只减引用，不真正释放 */
        if (i < PMM_REF_PAGES && page_ref[i] > 1U) {
            page_ref[i]--;
            continue;
        }
        if (i < PMM_REF_PAGES) page_ref[i] = 0U;
        bitmap_clear(i);
        pmm.free_pages++;
    }
}

/* ==================== 单页申请/释放 ==================== */
void* pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

void pmm_free_page(void* phys_addr)
{
    pmm_free_pages(phys_addr, 1);
}

/* ==================== 把一段物理内存标记为已用 ==================== */
void pmm_mark_range(uint64_t base, uint64_t length)
{
    if (length == 0) return;

    uint64_t end = base + length;
    uintptr_t ms = (uintptr_t)memory_start;

    /* 边界裁剪：页号是相对 memory_start 的，若范围整体位于
       memory_start 之下（例如 1MB 处的内核镜像，而位图基准是 2MB），
       起始页号会下溢成一个巨大的无符号数，直接把位图写坏。
       所以整段都在基准以下时，什么都不用标记。 */
    if (end <= ms) {
        return;
    }

    size_t start_page;
    if (base < ms) {
        start_page = 0;
    } else {
        start_page = (size_t)((base - ms) >> PAGE_SHIFT);
    }

    size_t end_page = (size_t)((end - ms + PAGE_SIZE - 1) >> PAGE_SHIFT);

    if (end_page > pmm.total_pages) {
        end_page = pmm.total_pages;
    }

    for (size_t i = start_page; i < end_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            pmm.free_pages--;
        }
    }
}

/* ==================== 按 E820 表标记保留区域 ==================== */
void pmm_init_e820(e820_entry_t* e820_table, uint32_t entry_count)
{
    for (uint32_t i = 0; i < entry_count; i++) {
        /* 所有非“可用”区域（保留区/ACPI/NVS 等）都标记为已用 */
        if (e820_table[i].type != E820_TYPE_FREE) {
            pmm_mark_range(e820_table[i].base, e820_table[i].length);
        }
    }
}
