/* =============================================================================
 * user_mode.c - 用户程序加载与 ring3 切换
 *
 * 支持的映像格式：
 *   - ELF32（当前用户程序就是这种）：解析程序头，把每个 PT_LOAD 段按页
 *     分配物理页、复制文件内容、清零 .bss 部分，再映射进用户地址空间；
 *   - 裸二进制（早期内核内建 demo）：整段复制到 0x40000000 一页。
 *
 * 栈布局（ELF 进程启动约定）：
 *   [esp]   假返回地址 0
 *   [esp+4] argc
 *   [esp+8] argv[0] ... argv[argc-1], NULL
 * 这样 `_start(int argc, char **argv)` 用普通 cdecl 就能读到参数。
 * ============================================================================= */
#include "user_mode.h"
#include "include/mm.h"
#include "include/proc.h"
#include "../include/types.h"
#include "include/mm.h"
#include "include/mm.h"
#include "include/fs.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "include/proc.h"
#include "include/proc.h"
#include "include/security.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "include/kernel.h"

/* 用户地址空间布局由 user_layout.h 统一提供：
 *   0x40000000 .. 0x40100000   代码/数据
 *   0x40100000 .. 0x40200000   栈（向下增长，底部 guard）
 *   0x40200000 .. 0x40300000   堆（brk 向上增长）
 *   0x40300000                 信号蹦床
 * fork 的 COW 范围覆盖 [0x40000000, 0x40400000)，包含栈、堆、蹦床页。 */
#define USER_LOAD_MIN   USER_LAYOUT_CODE_BASE
#define USER_CODE_VA    USER_LAYOUT_CODE_BASE
#define USER_LOAD_MAX   USER_LAYOUT_CODE_END
#define USER_STACK_SIZE USER_LAYOUT_STACK_SIZE
#define USER_STACK_VA   USER_LAYOUT_STACK_INIT
#define USER_STACK_TOP  USER_LAYOUT_STACK_TOP
#define USER_TRAMPOLINE_VA USER_LAYOUT_TRAMPOLINE

extern uint8_t user_demo_start[];
extern uint8_t user_demo_end[];
extern uint8_t user_probe_start[];
extern uint8_t user_probe_end[];
extern uint8_t user_fsd_start[];
extern uint8_t user_fsd_end[];
extern uint8_t user_blkd_start[];
extern uint8_t user_blkd_end[];

/* ---------------- ELF32 解析（不依赖 struct 对齐） ---------------- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

#define PT_LOAD 1U
#define PF_X    1U
#define PF_W    2U
#define PF_R    4U

static int elf_load(const uint8_t *img, uint32_t size, uint32_t *entry)
{
    uint32_t e_entry, e_phoff;
    uint16_t e_phentsize, e_phnum;
    int saw_text = 0;
    int saw_data = 0;

    if (size < 52U) return -1;
    if (img[0] != 0x7FU || img[1] != 'E' || img[2] != 'L' || img[3] != 'F') return -1;
    if (img[4] != 1U || img[5] != 1U) return -1;      /* ELFCLASS32 / 小端 */

    e_entry     = rd32(img + 24);
    e_phoff     = rd32(img + 28);
    e_phentsize = rd16(img + 42);
    e_phnum     = rd16(img + 44);

    if (e_phnum == 0U || e_phnum > 16U || e_phentsize < 32U) return -1;
    if (e_phoff > size) return -1;
    if (e_phnum > (size - e_phoff) / e_phentsize) return -1;

    for (uint32_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = img + e_phoff + i * e_phentsize;
        uint32_t type   = rd32(ph + 0);
        uint32_t off    = rd32(ph + 4);
        uint32_t vaddr  = rd32(ph + 8);
        uint32_t filesz = rd32(ph + 16);
        uint32_t memsz  = rd32(ph + 20);
        uint32_t flags  = rd32(ph + 24);
        uint32_t first, pages;
        uint32_t map_flags;

        if (type != PT_LOAD || memsz == 0U) continue;
        if (memsz < filesz) return -1;
        if (vaddr < USER_LOAD_MIN || vaddr >= USER_LOAD_MAX) return -1;
        if (memsz > USER_LOAD_MAX - vaddr) return -1;
        if (off > size || filesz > size - off) return -1;

        /* 拒绝 W+X 段：P3 的加载器不把“可写代码页”交给 ring3。 */
        if ((flags & (PF_W | PF_X)) == (PF_W | PF_X)) return -1;

        if ((flags & PF_X) != 0U) saw_text = 1;
        if ((flags & PF_W) != 0U) saw_data = 1;

        /* 代码段只读；数据段可写。32 位无 NX 时 X 位无法在页表表达，
         * 但代码页不带 PTE_WRITE 已经能阻止用户改写自身代码。 */
        map_flags = PTE_PRESENT | PTE_USER;
        if ((flags & PF_W) != 0U) map_flags |= PTE_WRITE;
        if ((flags & PF_X) == 0U) map_flags |= PTE_NOEXEC;

        first = vaddr & 0xFFFU;
        pages = (first + memsz + 0xFFFU) >> 12;

        for (uint32_t pi = 0; pi < pages; pi++) {
            uint32_t va = (vaddr & 0xFFFFF000U) + (pi << 12);
            uint32_t copy_off = (pi == 0U) ? first : 0U;
            uint32_t seg_off = (va + copy_off) - vaddr;
            void *phys = pmm_alloc_page();

            if (phys == 0) return -1;
            memset(phys, 0, USER_STACK_SIZE);

            if (seg_off < filesz) {
                uint32_t n = USER_STACK_SIZE - copy_off;
                if (n > filesz - seg_off) n = filesz - seg_off;
                memcpy((uint8_t *)phys + copy_off, img + off + seg_off, n);
            }

            if (vmm_map_user_page(va, (uint32_t)(uintptr_t)phys,
                                  map_flags) != 0) {
                return -1;
            }
        }
    }

    if (e_entry < USER_LOAD_MIN || e_entry >= USER_LOAD_MAX) return -1;
    if (!saw_text) return -1;
    /* 最后一道运行时断言：入口页必须真的不可写。 */
    if ((vmm_user_flags(e_entry) & PTE_WRITE) != 0U) return -1;
    if (saw_text && saw_data) {
        kprintln("user: ELF text RO / data RW (W^X loader)");
    }
    *entry = e_entry;
    return 0;
}

/* ---------------- 用户栈 ---------------- */

static uint32_t build_stack(uint8_t *page, const char *args, uint32_t stack_va)
{
    uint32_t sp = USER_STACK_SIZE;
    uint32_t argc = 0;
    uint32_t argv_va[8];
    uint32_t i, start;

    memset(page, 0, USER_STACK_SIZE);

    if (args != 0 && args[0] != '\0') {
        uint32_t len = (uint32_t)strlen(args) + 1U;
        if (len > 256U) len = 256U;

        sp -= len;
        memcpy(page + sp, args, len - 1U);
        page[sp + len - 1U] = '\0';

        i = 0;
        while (page[sp + i] != '\0' && argc < 8U) {
            while (page[sp + i] == ' ') i++;
            if (page[sp + i] == '\0') break;

            start = i;
            argv_va[argc++] = stack_va + sp + start;
            while (page[sp + i] != '\0' && page[sp + i] != ' ') i++;
            if (page[sp + i] == ' ') page[sp + i++] = '\0';
        }
    }

    /* 先构造指针数组：[argv0][argv1]...[NULL] */
    sp &= ~3U;
    sp -= 4; *(uint32_t *)(page + sp) = 0;
    for (i = argc; i > 0U; i--) {
        sp -= 4; *(uint32_t *)(page + sp) = argv_va[i - 1U];
    }

    /* 再构造 cdecl 调用帧：[ret][argc][argv 指针] */
    {
        uint32_t argv_va_addr = stack_va + sp;

        sp -= 4; *(uint32_t *)(page + sp) = argv_va_addr;
        sp -= 4; *(uint32_t *)(page + sp) = argc;
        sp -= 4; *(uint32_t *)(page + sp) = 0;      /* 假返回地址 */
    }

    return sp;                                       /* 页内偏移 */
}

/* ---------------- 进入 ring3 ---------------- */

/* 信号蹦床页：handler 返回后跳到这里执行 int 0x81 (SYS_SIGRETURN)
 * 机器码：mov eax,31 ; int 0x81
 * 地址来自 user_layout.h，已移出栈增长区。 */

static int map_trampoline(uint32_t va)
{
    static const uint8_t code[] = { 0xB8, 0x1F, 0x00, 0x00, 0x00, 0xCD, 0x81 };
    void *page = pmm_alloc_page();
    uint32_t i;

    if (page == 0) return -1;
    memset(page, 0, PAGE_SIZE);
    for (i = 0; i < sizeof(code); i++) ((uint8_t *)page)[i] = code[i];

    int r = vmm_map_user_page(va, (uint32_t)(uintptr_t)page,
                              PTE_PRESENT | PTE_USER);
    if (r != 0) pmm_free_pages(page, 1);
    return r;
}

static int enter_user(uint32_t entry, void *stack_phys, const char *args, const char *what)
{
    proc_t *p = proc_current();
    uint32_t stack_va = (p != 0 && p->user_stack_va != 0U)
                            ? p->user_stack_va : USER_LAYOUT_STACK_INIT;
    uint32_t tramp_va = (p != 0 && p->user_trampoline != 0U)
                            ? p->user_trampoline : USER_LAYOUT_TRAMPOLINE;
    uint32_t off;

    off = build_stack((uint8_t *)stack_phys, args, stack_va);

    if (vmm_map_user_page(stack_va, (uint32_t)(uintptr_t)stack_phys,
                          PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
        return -1;
    }

    (void)map_trampoline(tramp_va);

    if (p != 0) {
        p->is_user = true;
        tss_set_esp0((uint32_t)(uintptr_t)p->stack_top);
    }

    kprintf("user: entering ring3 (%s, entry=%p pid=%u cpu=%u)\n",
            what, (void *)(uintptr_t)entry, (p != 0) ? p->pid : 0U,
            (unsigned int)smp_cpu_id());
    kprintf("user: layout code=%p..%p stack=%p..%p heap=%p..%p tramp=%p\n",
            (void *)(uintptr_t)USER_LAYOUT_CODE_BASE,
            (void *)(uintptr_t)USER_LAYOUT_CODE_END,
            (void *)(uintptr_t)USER_LAYOUT_STACK_LIMIT,
            (void *)(uintptr_t)USER_LAYOUT_STACK_TOP,
            (void *)(uintptr_t)USER_LAYOUT_HEAP_BASE,
            (void *)(uintptr_t)USER_LAYOUT_HEAP_END,
            (void *)(uintptr_t)tramp_va);
    if (p != 0) {
        kprintf("user: ASLR stack_top=%p stack_init=%p heap_base=%p tramp=%p pid=%u\n",
                (void *)(uintptr_t)p->user_stack_top,
                (void *)(uintptr_t)p->user_stack_va,
                (void *)(uintptr_t)p->user_heap_base,
                (void *)(uintptr_t)p->user_trampoline, p->pid);
    }

    user_enter(entry, stack_va + off);
    return 0;
}

/* ---------------- 加载映像 ---------------- */

static int user_load_program(const char *path, uint32_t *entry)
{
    fs_stat_t st;
    uint8_t *buf;
    uint32_t h;
    int n;

    if (fs_stat(path, &st) != E_OK || st.type != FS_TYPE_FILE) return -1;
    if (st.size == 0U || st.size > FS_FILE_MAX) return -1;

    buf = malloc(st.size);
    if (buf == 0) return -1;

    h = fs_open(path, FS_OPEN_READ);
    if (h == 0U) { free(buf); return -1; }
    n = fs_read(h, buf, st.size);
    fs_close(h);

    if (n != (int)st.size) { free(buf); return -1; }

    /* 签名验证必须在映射/执行之前。 */
    if (!trust_verify_shell(buf, st.size)) {
        kprintln("trust: rejected unsigned image");
        free(buf);
        return -1;
    }

    if (buf[0] == 0x7FU && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        int r = elf_load(buf, st.size, entry);
        free(buf);
        return r;
    }

    /* 裸二进制：整段放到 0x40000000 一页里 */
    {
        void *phys = pmm_alloc_page();
        if (phys == 0) { free(buf); return -1; }
        memset(phys, 0, USER_STACK_SIZE);
        memcpy(phys, buf, st.size);
        free(buf);

        if (vmm_map_user_page(USER_CODE_VA, (uint32_t)(uintptr_t)phys,
                              PTE_PRESENT | PTE_USER) != 0) {
            return -1;
        }
        *entry = USER_CODE_VA;
        return 0;
    }
}

int user_mode_run(void)
{
    uint32_t size = (uint32_t)(uintptr_t)(user_demo_end - user_demo_start);
    void *code_phys;
    void *stack_phys;

    if (size == 0U || size > USER_STACK_SIZE) return -1;

    code_phys  = pmm_alloc_page();
    stack_phys = pmm_alloc_page();
    if (code_phys == 0 || stack_phys == 0) return -1;

    memset(code_phys, 0, USER_STACK_SIZE);
    memcpy(code_phys, user_demo_start, size);

    if (vmm_map_user_page(USER_CODE_VA, (uint32_t)(uintptr_t)code_phys,
                          PTE_PRESENT | PTE_USER) != 0) {
        return -1;
    }

    return enter_user(USER_CODE_VA, stack_phys, 0, "builtin demo");
}

int user_probe_run(void)
{
    uint32_t size = (uint32_t)(uintptr_t)(user_probe_end - user_probe_start);
    void *code_phys;
    void *stack_phys;

    if (size == 0U || size > USER_STACK_SIZE) return -1;

    code_phys  = pmm_alloc_page();
    stack_phys = pmm_alloc_page();
    if (code_phys == 0 || stack_phys == 0) return -1;

    memset(code_phys, 0, USER_STACK_SIZE);
    memcpy(code_phys, user_probe_start, size);

    if (vmm_map_user_page(USER_CODE_VA, (uint32_t)(uintptr_t)code_phys,
                          PTE_PRESENT | PTE_USER) != 0) {
        return -1;
    }

    return enter_user(USER_CODE_VA, stack_phys, 0, "ring3 AP probe");
}

int user_fsd_run(void)
{
    uint32_t size = (uint32_t)(uintptr_t)(user_fsd_end - user_fsd_start);
    void *code_phys;
    void *stack_phys;

    if (size == 0U || size > USER_STACK_SIZE) return -1;

    code_phys  = pmm_alloc_page();
    stack_phys = pmm_alloc_page();
    if (code_phys == 0 || stack_phys == 0) return -1;

    memset(code_phys, 0, USER_STACK_SIZE);
    memcpy(code_phys, user_fsd_start, size);

    if (vmm_map_user_page(USER_CODE_VA, (uint32_t)(uintptr_t)code_phys,
                          PTE_PRESENT | PTE_USER) != 0) {
        return -1;
    }

    return enter_user(USER_CODE_VA, stack_phys, 0, "ring3 fsd");
}

int user_blkd_run(void)
{
    uint32_t size = (uint32_t)(uintptr_t)(user_blkd_end - user_blkd_start);
    void *code_phys;
    void *stack_phys;

    if (size == 0U || size > USER_STACK_SIZE) return -1;

    code_phys  = pmm_alloc_page();
    stack_phys = pmm_alloc_page();
    if (code_phys == 0 || stack_phys == 0) return -1;

    memset(code_phys, 0, USER_STACK_SIZE);
    memcpy(code_phys, user_blkd_start, size);

    if (vmm_map_user_page(USER_CODE_VA, (uint32_t)(uintptr_t)code_phys,
                          PTE_PRESENT | PTE_USER) != 0) {
        return -1;
    }
    return enter_user(USER_CODE_VA, stack_phys, 0, "ring3 blkd");
}

int user_exec(const char *path)
{
    uint32_t entry = 0;
    void *stack_phys;

    if (path == 0) return -1;
    if (user_load_program(path, &entry) != 0) return -1;

    stack_phys = pmm_alloc_page();
    if (stack_phys == 0) return -1;

    return enter_user(entry, stack_phys, 0, path);
}

/* 用户栈按需增长：栈区内缺页就补一页；底部 guard 页永不映射。 */
bool user_try_grow_stack(uint32_t fault_addr)
{
    proc_t *p = proc_current();
    uint32_t page = fault_addr & 0xFFFFF000U;
    void *phys;

    if (p == 0 || !p->is_user) return false;
    if (fault_addr >= p->user_stack_top) return false;
    if (fault_addr < USER_LAYOUT_STACK_GUARD_END) return false;
    if (vmm_get_phys(page) != 0U) return false;     /* 已映射，不是栈增长 */

    phys = pmm_alloc_page();
    if (phys == 0) return false;
    memset(phys, 0, USER_STACK_SIZE);

    if (vmm_map_user_page(page, (uint32_t)(uintptr_t)phys,
                          PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
        return false;
    }
    kprintf("user: grew stack page at %p (pid %u)\n",
            (void *)(uintptr_t)page, p->pid);
    return true;
}

int user_exec_replace(const char *path, const char *args, struct interrupt_frame *f)
{
    uint32_t entry = 0;
    proc_t *p = proc_current();
    uint32_t stack_va = (p != 0 && p->user_stack_va != 0U)
                            ? p->user_stack_va : USER_LAYOUT_STACK_INIT;
    uint32_t tramp_va = (p != 0 && p->user_trampoline != 0U)
                            ? p->user_trampoline : USER_LAYOUT_TRAMPOLINE;
    pde_t *old_dir;
    pde_t *new_dir;
    void *stack_phys = 0;
    uint32_t off;

    if (p == 0 || path == 0 || f == 0) return -1;

    /* 先在新地址空间里构造镜像；成功后才拆旧空间，失败则保留旧空间。 */
    old_dir = vmm_current_dir();
    new_dir = vmm_new_dir();
    if (new_dir == 0) return -1;
    vmm_switch_dir(new_dir);

    if (user_load_program(path, &entry) != 0) goto fail;

    stack_phys = pmm_alloc_page();
    if (stack_phys == 0) goto fail;

    off = build_stack((uint8_t *)stack_phys, args, stack_va);
    if (vmm_map_user_page(stack_va, (uint32_t)(uintptr_t)stack_phys,
                          PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
        stack_phys = 0;                 /* 已由 fail 的 clear 释放 */
        goto fail;
    }
    if (map_trampoline(tramp_va) != 0) goto fail;

    p->page_dir = new_dir;
    p->is_user = true;
    tss_set_esp0((uint32_t)(uintptr_t)p->stack_top);

    /* 新空间已就绪，回收旧用户区。 */
    vmm_destroy_dir(old_dir);

    /* P2：setuid/setgid 位（FAT16 卷按 nosuid 处理，不信任宿主 mode） */
    acct_exec_cred((struct process *)p, path);

    f->eip = entry;
    f->useresp = stack_va + off;
    f->eax = 0;

    kprintf("user: exec %s (entry=%p, args='%s')\n",
            path, (void *)(uintptr_t)entry, (args != 0) ? args : "");
    return 0;

fail:
    if (stack_phys != 0 && vmm_get_phys(stack_va) == 0U) {
        pmm_free_pages(stack_phys, 1);
    }
    vmm_clear_user_space(new_dir);
    vmm_switch_dir(old_dir);
    pmm_free_pages(new_dir, 1);
    return -1;
}
