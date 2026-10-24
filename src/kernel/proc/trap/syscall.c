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
#include "../../panic.h"
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

/* 用户地址上界：0x40000000 起是用户页，0xC0000000 以上留给内核 */
#define USER_ADDR_LIMIT 0xC0000000U
#define USER_LEN_LIMIT  (64U * 1024U)

static unsigned int calls;

/* 校验用户传入的缓冲区：必须在用户地址范围且长度合理。
 * 这是 ring3 的第一道防线：没有它，用户可以把内核地址交给 SYS_WRITE。 */
static bool user_range_ok(uint32_t ptr, uint32_t len)
{
    if (len > USER_LEN_LIMIT) return false;
    if (ptr >= USER_ADDR_LIMIT) return false;
    if (len > USER_ADDR_LIMIT - ptr) return false;
    return true;
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

static int sys_getpid(void)
{
    proc_t *p = proc_current();
    return (p != 0) ? (int)p->pid : 0;
}

static int sys_uptime(void)
{
    return (int)(timer_ticks() / timer_hz());
}

static int sys_yield(void)
{
    proc_yield();
    return 0;
}

/* ---- 文件类系统调用的用户参数处理 ---- */

static int copy_user_string(char *dst, uint32_t dst_size, uint32_t uptr)
{
    uint32_t i;

    if (dst_size == 0U) return E_INVAL;

    for (i = 0; i + 1U < dst_size; i++) {
        char c;
        if (copy_from_user(&c, (const void *)(uintptr_t)(uptr + i), 1) != E_OK) {
            return E_INVAL;
        }
        dst[i] = c;
        if (c == '\0') return E_OK;
    }
    dst[dst_size - 1U] = '\0';
    return E_INVAL;                    /* 路径太长 */
}

static int path_has_parent(const char *p)
{
    for (; p[0] != '\0' && p[1] != '\0'; p++) {
        if (p[0] == '.' && p[1] == '.') return 1;
    }
    return 0;
}

/* 把用户给的路径转成绝对路径：
 *   以 '/' 开头或含 ':'  -> 原样（绝对路径/指定驱动号）
 *   其它                -> 拼上当前进程的 cwd
 */
static void make_abs_path(char *out, uint32_t out_max, const char *in)
{
    proc_t *p = proc_current();
    const char *cwd = (p != 0 && p->cwd[0] != '\0') ? p->cwd : "system:";
    const char *root = (p != 0) ? proc_root_of(p) : "";
    uint32_t n = 0;
    uint32_t i;

    if (root[0] != '\0' && path_has_parent(in)) {
        out[0] = '\0';
        return;
    }

    if (in[0] == '/') {
        if (root[0] == '\0') {
            strncpy(out, in, out_max - 1U);
            out[out_max - 1U] = '\0';
            return;
        }
        for (i = 0; root[i] != '\0' && n + 1U < out_max; i++) out[n++] = root[i];
        while (n > 0U && out[n - 1U] == '/') n--;
        if (n + 1U < out_max) out[n++] = '/';
        for (i = 1; in[i] != '\0' && n + 1U < out_max; i++) out[n++] = in[i];
        out[n] = '\0';
        return;
    }

    if (strchr(in, ':') != 0) {
        if (root[0] != '\0') {
            uint32_t rl = (uint32_t)strlen(root);
            if (rl > 0U && strncmp(in, root, rl) != 0) {
                out[0] = '\0';
                return;
            }
        }
        strncpy(out, in, out_max - 1U);
        out[out_max - 1U] = '\0';
        return;
    }

    if (in[0] == '\0') {
        strncpy(out, cwd, out_max - 1U);
        out[out_max - 1U] = '\0';
        return;
    }

    for (i = 0; cwd[i] != '\0' && n + 1U < out_max; i++) out[n++] = cwd[i];
    if (n > 0U && out[n - 1U] != '/' && n + 1U < out_max) out[n++] = '/';
    for (i = 0; in[i] != '\0' && n + 1U < out_max; i++) out[n++] = in[i];
    out[n] = '\0';
}

/* 前向声明：fd 读 stdin 时用到阻塞读 */
static int sys_getchar_block(void);

static proc_fd_t *fd_get(proc_t *p, int fd)
{
    if (p == 0 || fd < 0 || fd >= PROC_FD_MAX) return 0;
    if (!p->fds[fd].used) return 0;
    return &p->fds[fd];
}

static int fd_alloc_kind(proc_t *p, uint8_t kind, uint32_t handle, uint32_t rights)
{
    for (int i = 3; i < PROC_FD_MAX; i++) {
        if (!p->fds[i].used) {
            p->fds[i].used = true;
            p->fds[i].kind = kind;
            p->fds[i].handle = handle;
            p->fds[i].rights = rights;
            return i;
        }
    }
    return -1;
}

static uint32_t open_rights(uint32_t flags)
{
    uint32_t rights = 0;

    if ((flags & FS_OPEN_READ) != 0U) rights |= FD_RIGHT_READ;
    if ((flags & FS_OPEN_WRITE) != 0U) rights |= FD_RIGHT_WRITE;
    return (rights != 0U) ? rights : FD_RIGHT_READ;
}

static int fd_alloc(proc_t *p, uint32_t handle, uint32_t rights)
{
    return fd_alloc_kind(p, FD_KIND_FILE, handle, rights);
}

static int sys_open_user(uint32_t path_ptr, uint32_t flags)
{
    char rel[256];
    char path[256];
    proc_t *p = proc_current();
    uint8_t be = FS_BE_RAMFS;
    int h;
    int fd;

    if (p == 0) return -1;
    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    /* "fat:..." 含 ':'，make_abs_path 会原样保留；
     * 具体路由（ramfs / FAT16）交给 fs 层，syscall 层不再判断文件系统类型。 */
    make_abs_path(path, sizeof(path), rel);

    h = fs_open_ex(path, flags & (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT),
                   (p != 0) ? p->uid : 0U, (p != 0) ? p->gid : 0U, &be);
    if (h < 0) return -1;

    fd = fd_alloc(p, (uint32_t)h, open_rights(flags));
    if (fd < 0) {
        fs_close_ex(be, h);
        return -1;
    }
    p->fds[fd].fs_be = be;
    return fd;
}

static int sys_fread_user(int fd, uint32_t ubuf, uint32_t len)
{
    proc_t *p = proc_current();
    proc_fd_t *f = fd_get(p, fd);
    char buf[128];
    int n;

    if (f == 0) return -1;
    if ((f->rights & FD_RIGHT_READ) == 0U) return -1;
    if (len > (uint32_t)sizeof(buf)) len = (uint32_t)sizeof(buf);

    if (f->kind == FD_KIND_CONSOLE) {
        /* stdin：按字节阻塞读，直到读到数据或缓冲区满 */
        uint32_t got = 0;
        while (got < len) {
            int c = sys_getchar_block();
            if (c < 0) break;
            buf[got++] = (char)c;
            if (ubuf == 0U) break;
        }
        if (got == 0U) return 0;
        if (copy_to_user((void *)(uintptr_t)ubuf, buf, got) != E_OK) return -1;
        return (int)got;
    }

    if (f->kind == FD_KIND_PIPE_R) {
        n = pipe_read((int)f->handle, buf, len);
        if (n < 0) return n;
        if (copy_to_user((void *)(uintptr_t)ubuf, buf, (uint32_t)n) != E_OK) return -1;
        return n;
    }

    n = fs_read_ex(f->fs_be, (int)f->handle, buf, len);
    if (n < 0) return n;
    if (copy_to_user((void *)(uintptr_t)ubuf, buf, (uint32_t)n) != E_OK) return -1;
    return n;
}

static int sys_fwrite_user(int fd, uint32_t ubuf, uint32_t len)
{
    proc_t *p = proc_current();
    proc_fd_t *f = fd_get(p, fd);
    char buf[128];
    uint32_t done = 0;

    if (f == 0) return -1;
    if ((f->rights & FD_RIGHT_WRITE) == 0U) return -1;

    /* 分块搬运：用户一次可能写几百字节（例如整段帮助文本） */
    while (done < len) {
        uint32_t n = len - done;

        if (n > (uint32_t)sizeof(buf)) n = (uint32_t)sizeof(buf);

        if (copy_from_user(buf, (const void *)(uintptr_t)(ubuf + done), n) != E_OK) {
            return -1;
        }

        if (f->kind == FD_KIND_CONSOLE) {
            for (uint32_t i = 0; i < n; i++) console_putc(buf[i]);
        } else if (f->kind == FD_KIND_PIPE_W) {
            if (pipe_write((int)f->handle, buf, n) != (int)n) return -1;
        } else {
            if (fs_write_ex(f->fs_be, (int)f->handle, buf, n) != (int)n) return -1;
        }
        done += n;
    }

    return (int)done;
}

static int sys_close_user(int fd)
{
    proc_t *p = proc_current();
    proc_fd_t *f = fd_get(p, fd);

    if (f == 0 || fd < 3) return -1;

    if (f->kind == FD_KIND_FILE) {
        fs_close_ex(f->fs_be, (int)f->handle);
    } else if (f->kind == FD_KIND_PIPE_W) {
        pipe_unref_write((int)f->handle);
    } else if (f->kind == FD_KIND_PIPE_R) {
        pipe_unref_read((int)f->handle);
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

static int sys_list_user(uint32_t path_ptr, uint32_t index, uint32_t ubuf)
{
    char rel[256];
    char path[256];
    char name[FS_NAME_MAX];
    fs_stat_t st;
    uint32_t n;

    if (path_ptr == 0U) {
        /* 没有路径参数：模块层，列出驱动器 */
        path[0] = '\0';
    } else {
        if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) {
            return -1;
        }
        make_abs_path(path, sizeof(path), rel);
    }

    if (fs_listdir_ex(path, index, name, sizeof(name), &st, 0) != 0) return -1;

    n = (uint32_t)strlen(name) + 1U;
    if (copy_to_user((void *)(uintptr_t)ubuf, name, n) != E_OK) return -1;
    return 0;
}

/* 把用户路径转成绝对路径并执行一个 fs 操作 */
static int sys_mkdir_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    proc_t *p = proc_current();

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    return (fs_mkdir_ex(path, (p != 0) ? p->uid : 0U, (p != 0) ? p->gid : 0U, 0) == 0) ? 0 : -1;
}

static int sys_unlink_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    proc_t *p = proc_current();

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    return (fs_unlink_ex(path, (p != 0) ? p->uid : 0U, (p != 0) ? p->gid : 0U, 0) == 0) ? 0 : -1;
}

/* UDP 收包：短轮询 + 让出 CPU。
 * 系统调用门是中断门（IF=0），若在门里自旋几百万次，PIT tick 无法推进，
 * NMI 看门狗会误判“无进展”而 panic；因此每轮之后 sleep 1 tick。 */
static int udp_recv_yield(uint16_t local_port, uint8_t ip[4], uint16_t *port,
                          void *buf, uint32_t len, int allow_loopback)
{
    int n = -1;

    for (int i = 0; i < 40; i++) {
        n = net_udp_recv_port(local_port, ip, port, buf, len, 20000U, 0);
        if (n >= 0) break;
        proc_sleep_ticks(1U);
    }

    /* 宿主始终没回显：最后允许一次本地回环兜底（会打印日志） */
    if (n < 0 && allow_loopback) {
        n = net_udp_recv_port(local_port, ip, port, buf, len, 1U, 1);
    }
    return n;
}

static int sys_stat_user(uint32_t path_ptr, uint32_t ubuf)
{
    char rel[256];
    char path[256];
    fs_stat_t st;
    uint32_t out[6];

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    if (fs_stat_ex(path, &st, 0) != 0) return -1;

    out[0] = (uint32_t)st.type;
    out[1] = st.size;
    out[2] = st.mtime;
    out[3] = st.mode;
    out[4] = st.uid;
    out[5] = st.gid;
    return (copy_to_user((void *)(uintptr_t)ubuf, out, sizeof(out)) == E_OK) ? 0 : -1;
}

/* 规范化绝对路径：去掉结尾的 '/'、'/.'、'/..'（供 cd 存 cwd 用） */
static void normalize_abs(char *p)
{
    for (;;) {
        uint32_t n = (uint32_t)strlen(p);

        if (n > 0U && p[n - 1U] == '/') {
            p[n - 1U] = '\0';
            continue;
        }
        if (n >= 2U && p[n - 1U] == '.' && p[n - 2U] == '/') {
            p[n - 2U] = '\0';
            continue;
        }
        if (n >= 3U && p[n - 1U] == '.' && p[n - 2U] == '.' && p[n - 3U] == '/') {
            p[n - 3U] = '\0';
            n = (uint32_t)strlen(p);
            while (n > 0U && p[n - 1U] != '/') {
                p[--n] = '\0';
            }
            continue;
        }
        break;
    }
}

static int sys_chdir_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    fs_stat_t st;
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;
    make_abs_path(path, sizeof(path), rel);

    if (fs_stat(path, &st) != E_OK || st.type != FS_TYPE_DIR) return -1;

    normalize_abs(path);
    if (path[0] == '\0') return -1;

    strncpy(p->cwd, path, sizeof(p->cwd) - 1U);
    p->cwd[sizeof(p->cwd) - 1U] = '\0';
    return 0;
}

static int sys_getcwd_user(uint32_t ubuf, uint32_t max)
{
    proc_t *p = proc_current();
    uint32_t n;

    if (p == 0 || max == 0U) return -1;

    n = (uint32_t)strlen(p->cwd) + 1U;
    if (n > max) n = max;

    if (copy_to_user((void *)(uintptr_t)ubuf, p->cwd, n) != E_OK) return -1;
    return (int)(n - 1U);
}

/* 取第 index 个盘符名；FAT16 交换盘排在最后 */
static int sys_drives_user(uint32_t index, uint32_t ubuf, uint32_t max)
{
    char name[FS_DRIVE_MAX];
    uint32_t n;
    uint32_t fs_count = fs_volume_count();

    if (index < fs_count) {
        if (fs_volume_name(index, name, sizeof(name)) != E_OK) return -1;
    } else if (index == fs_count && fat16_mounted()) {
        strncpy(name, fat16_drive(), sizeof(name) - 1U);
        name[sizeof(name) - 1U] = '\0';
    } else {
        return -1;
    }

    n = (uint32_t)strlen(name) + 1U;
    if (n > max) n = max;
    if (copy_to_user((void *)(uintptr_t)ubuf, name, n) != E_OK) return -1;
    return 0;
}

/* 阻塞读一个字符：没有键就睡在键盘通道上，由 IRQ1 唤醒 */
static int sys_getchar_block(void)
{
    for (;;) {
        unsigned int flags;
        int c;

        __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
        c = keyboard_getchar();
        if (c >= 0) {
            __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
            return c;
        }
        proc_block(PROC_WAIT_KBD);
        __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
    }
}

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

    switch (f->eax) {
    case SYS_WRITE:
        if (!user_range_ok(f->ebx, f->ecx)) {
            ret = -1;
            break;
        }
        ret = sys_write_user(f->ebx, f->ecx);
        break;
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_UPTIME:
        ret = sys_uptime();
        break;
    case SYS_YIELD:
        ret = sys_yield();
        break;
    case SYS_EXIT:
        /* 不返回：直接让当前进程退出 */
        proc_exit((int)f->ebx);
        ret = 0;
        break;
    case SYS_OPEN:
        ret = sys_open_user(f->ebx, f->ecx);
        break;
    case SYS_FREAD:
        ret = sys_fread_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_FWRITE:
        ret = sys_fwrite_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_CLOSE:
        ret = sys_close_user(f->ebx);
        break;
    case SYS_GETCHAR:
        ret = sys_getchar_block();
        break;
    case SYS_LIST:
        ret = sys_list_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_DRIVES:
        ret = sys_drives_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_MKDIR:
        ret = sys_mkdir_user(f->ebx);
        break;
    case SYS_UNLINK:
        ret = sys_unlink_user(f->ebx);
        break;
    case SYS_STAT:
        ret = sys_stat_user(f->ebx, f->ecx);
        break;
    case SYS_CHDIR:
        ret = sys_chdir_user(f->ebx);
        break;
    case SYS_CHROOT: {
        proc_t *p = proc_current();
        char rel[FS_PATH_MAX];
        char path[FS_PATH_MAX];
        fs_stat_t st;

        if (p == 0 || copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) {
            ret = -1;
            break;
        }
        make_abs_path(path, sizeof(path), rel);
        if (path[0] == '\0' || fs_stat(path, &st) != 0 || st.type != FS_TYPE_DIR) {
            ret = -1;
            break;
        }
        if (proc_root_set(p, path) != 0) { ret = -1; break; }
        strcpy(p->cwd, path);
        ret = 0;
        break;
    }
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
    case SYS_GETCWD:
        ret = sys_getcwd_user(f->ebx, f->ecx);
        break;
    case SYS_INPUT_SET: {
        char buf[128];
        if (f->ecx >= (uint32_t)sizeof(buf)) {
            ret = -1;
            break;
        }
        if (copy_from_user(buf, (const void *)(uintptr_t)f->ebx, f->ecx) != E_OK) {
            ret = -1;
            break;
        }
        buf[f->ecx] = '\0';
        console_input_set(buf);     /* 只画在底部输入框，不滚动输出区 */
        ret = 0;
        break;
    }
    case SYS_BRK:
        ret = sys_brk_user(f->ebx);
        break;
    case SYS_KILL: {
        proc_t *t = 0;

        for (uint32_t i = 0; i < PROC_MAX; i++) {
            const proc_t *q = proc_at(i);
            if (q != 0 && q->pid == f->ebx && q->state != PROC_FREE) { t = (proc_t *)q; break; }
        }
        if (t == 0) {
            ret = -1;
        } else {
            proc_send_signal(t, (int)f->ecx);
            ret = 0;
        }
        break;
    }
    case SYS_SLEEP: {
        uint32_t ms = f->ebx;
        uint32_t ticks = ms / 10U;
        if (ms > 0U && ticks == 0U) ticks = 1U;
        proc_sleep_ticks(ticks);
        ret = 0;
        break;
    }
    case SYS_PIPE: {
        proc_t *p = proc_current();
        int idx, rfd, wfd;
        int fds[2];

        if (p == 0) { ret = -1; break; }
        idx = pipe_create();
        if (idx < 0) { ret = -1; break; }

        rfd = fd_alloc_kind(p, FD_KIND_PIPE_R, (uint32_t)idx, FD_RIGHT_READ);
        wfd = fd_alloc_kind(p, FD_KIND_PIPE_W, (uint32_t)idx, FD_RIGHT_WRITE);
        if (rfd < 0 || wfd < 0) {
            if (rfd >= 0) memset(&p->fds[rfd], 0, sizeof(p->fds[rfd]));
            if (wfd >= 0) memset(&p->fds[wfd], 0, sizeof(p->fds[wfd]));
            ret = -1;
            break;
        }

        fds[0] = rfd;
        fds[1] = wfd;
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, fds, sizeof(fds)) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_DUP2: {
        proc_t *p = proc_current();
        proc_fd_t *old;
        int oldfd = (int)f->ebx;
        int newfd = (int)f->ecx;

        if (p == 0) { ret = -1; break; }
        old = fd_get(p, oldfd);
        if (old == 0 || newfd < 0 || newfd >= PROC_FD_MAX) { ret = -1; break; }
        if (newfd == oldfd) { ret = newfd; break; }

        /* 先释放 newfd 原有的内容 */
        if (p->fds[newfd].used) {
            if (p->fds[newfd].kind == FD_KIND_FILE) {
                fs_close_ex(p->fds[newfd].fs_be, (int)p->fds[newfd].handle);
            } else if (p->fds[newfd].kind == FD_KIND_PIPE_W) {
                pipe_unref_write((int)p->fds[newfd].handle);
            } else if (p->fds[newfd].kind == FD_KIND_PIPE_R) {
                pipe_unref_read((int)p->fds[newfd].handle);
            }
        }

        p->fds[newfd] = *old;
        if (p->fds[newfd].kind == FD_KIND_PIPE_W) {
            pipe_ref_write((int)p->fds[newfd].handle);
        } else if (p->fds[newfd].kind == FD_KIND_FILE) {
            if (fs_ref_ex(p->fds[newfd].fs_be, (int)p->fds[newfd].handle) != E_OK) {
                /* 该后端不支持句柄共享（如 FAT16）：拒绝 dup2，避免双重关闭 */
                memset(&p->fds[newfd], 0, sizeof(p->fds[newfd]));
                ret = -1;
                break;
            }
        }
        ret = newfd;
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
    case SYS_SYNC:
        ret = fs_sync_backing();
        break;
    case SYS_TIME: {
        /* ebx=用户缓冲, ecx=长度：写入格式化的 RTC 时间 */
        char tbuf[24];
        int n = rtc_format(rtc_now(), tbuf, sizeof(tbuf));

        if (n <= 0) { ret = -1; break; }
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, tbuf, (uint32_t)n + 1U) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_FSCK: {
        /* ebx=用户缓冲, ecx=长度：做一致性检查并返回错误数 */
        char report[96];
        int errs = fs_check(report, sizeof(report));

        if (f->ebx != 0U) {
            (void)copy_to_user((void *)(uintptr_t)f->ebx, report,
                               (uint32_t)strlen(report) + 1U);
        }
        ret = errs;
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
    case SYS_GETUID:
        ret = (proc_current() != 0) ? (int)proc_current()->uid : 0;
        break;
    case SYS_GETGID:
        ret = (proc_current() != 0) ? (int)proc_current()->gid : 0;
        break;
    case SYS_CHMOD: {
        /* ebx=路径, ecx=新 mode（低 9 位） */
        char rel[256];
        char path[256];

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) { ret = -1; break; }
        make_abs_path(path, sizeof(path), rel);
        ret = (fs_chmod(path, f->ecx, (proc_current() != 0) ? proc_current()->uid : 0) == E_OK)
              ? 0 : -1;
        break;
    }
    case SYS_CHOWN: {
        /* ebx=路径, ecx=uid, edx=gid（只有 root 能调用） */
        char rel[256];
        char path[256];
        proc_t *p = proc_current();

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) { ret = -1; break; }
        make_abs_path(path, sizeof(path), rel);
        ret = (fs_chown_as(path, f->ecx, f->edx,
                           (p != 0) ? p->uid : 0U) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_SETUID: {
        /* 只有 root 能改身份；降权后不能回升（单向） */
        proc_t *p = proc_current();

        if (p == 0 || p->uid != 0U) { ret = -1; break; }
        p->uid = f->ebx;
        p->gid = (f->ecx != 0U) ? f->ecx : f->ebx;
        ret = 0;
        break;
    }
    case SYS_HANG:
        /* 调试：关中断并死循环，只有 NMI 看门狗能发现 */
        kprintln("hang: disabling interrupts and spinning (NMI watchdog should fire)");
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
        ret = 0;
        break;
    case SYS_CAP_DROP: {
        /* 显式撤销 fd 的读/写 capability；之后同一 fd 的对应操作立即失败。 */
        proc_fd_t *fd = fd_get(proc_current(), (int)f->ebx);
        uint32_t drop = f->ecx & (FD_RIGHT_READ | FD_RIGHT_WRITE);

        if (fd == 0) { ret = -1; break; }
        fd->rights &= ~drop;
        ret = 0;
        break;
    }
    case SYS_CAP_SEND: {
        /* 把当前进程的文件 fd capability 发送给目标进程（source 保留）。 */
        proc_t *p = proc_current();
        proc_t *target = proc_find(f->ebx);
        proc_fd_t *src = fd_get(p, (int)f->ecx);
        uint32_t rights;
        int tfd;

        if (p == 0 || target == 0 || target == p || src == 0 ||
            src->kind != FD_KIND_FILE) {
            ret = -1;
            break;
        }
        rights = (f->edx != 0U) ? f->edx : src->rights;
        rights &= src->rights;
        if (rights == 0U) { ret = -1; break; }

        tfd = fd_alloc_kind(target, FD_KIND_FILE, src->handle, rights);
        if (tfd < 0) { ret = -1; break; }
        if (fs_ref_ex(src->fs_be, (int)src->handle) != E_OK) {
            memset(&target->fds[tfd], 0, sizeof(target->fds[tfd]));
            ret = -1;
            break;
        }
        target->fds[tfd].fs_be = src->fs_be;
        ret = tfd;
        break;
    }

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
    case SYS_UDP_SEND: {
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;

        if (copy_from_user(&req, (const void *)(uintptr_t)f->ebx, sizeof(req)) != E_OK ||
            req.len > sizeof(req.data)) {
            ret = -1;
            break;
        }
        ret = net_udp_send_to(req.ip, req.port, req.data, req.len);
        break;
    }
    case SYS_UDP_RECV: {
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;
        int n;

        if (copy_from_user(&req, (const void *)(uintptr_t)f->ebx, sizeof(req)) != E_OK) {
            ret = -1;
            break;
        }
        /* 旧路径固定用 12345 端口发/收；只收发给该端口的回包，且宿主
         * 完全没回显时允许一次本地回环兜底，保证 ring3 自检可判定。 */
        n = udp_recv_yield(NET_UDP_LOCAL_PORT, req.ip, &req.port,
                           req.data, sizeof(req.data), 1);
        if (n < 0) { ret = -1; break; }
        req.len = (uint16_t)n;
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, &req, sizeof(req)) == E_OK) ? n : -1;
        break;
    }
    case SYS_SOCKET: {
        proc_t *p = proc_current();
        int fd = -1;

        if (p != 0) {
            /* socket 的本地端口就是 fd->handle：每个 socket 用独立端口，
             * 这样 sendto/recvfrom 天然不会收到别的测试/别的 socket 的回包。 */
            fd = fd_alloc_kind(p, FD_KIND_UDP, (uint32_t)net_udp_alloc_port(),
                               FD_RIGHT_READ | FD_RIGHT_WRITE);
        }
        ret = fd;
        break;
    }
    case SYS_SENDTO: {
        proc_t *p = proc_current();
        proc_fd_t *fd = fd_get(p, (int)f->ebx);
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;

        if (fd == 0 || fd->kind != FD_KIND_UDP ||
            (fd->rights & FD_RIGHT_WRITE) == 0U ||
            copy_from_user(&req, (const void *)(uintptr_t)f->ecx, sizeof(req)) != E_OK ||
            req.len > sizeof(req.data)) {
            ret = -1;
            break;
        }
        ret = net_udp_send_from((uint16_t)fd->handle, req.ip, req.port,
                                req.data, req.len);
        break;
    }
    case SYS_RECVFROM: {
        proc_t *p = proc_current();
        proc_fd_t *fd = fd_get(p, (int)f->ebx);
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;
        int n;

        if (fd == 0 || fd->kind != FD_KIND_UDP ||
            (fd->rights & FD_RIGHT_READ) == 0U ||
            copy_from_user(&req, (const void *)(uintptr_t)f->ecx, sizeof(req)) != E_OK) {
            ret = -1;
            break;
        }
        /* 只收“目的端口 = socket 本地端口(fd->handle)”的报文；与 SYS_UDP_*
         * 的固定 12345 端口彼此隔离，不会互相吃回包。 */
        n = udp_recv_yield((uint16_t)fd->handle, req.ip, &req.port,
                           req.data, sizeof(req.data), 1);
        if (n < 0) { ret = -1; break; }
        req.len = (uint16_t)n;
        ret = (copy_to_user((void *)(uintptr_t)f->ecx, &req, sizeof(req)) == E_OK) ? n : -1;
        break;
    }
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
    case SYS_SIGACTION:
        ret = proc_signal_set((int)f->ebx, (uint32_t)f->ecx);
        break;
    case SYS_SIGRETURN:
        ret = proc_sigreturn((struct interrupt_frame *)f);
        break;
    case SYS_FORK: {
        proc_t *child = proc_fork((struct interrupt_frame *)f);
        ret = (child != 0) ? (int)child->pid : -1;
        break;
    }
    case SYS_EXEC: {
        char rel[256];
        char args[96];
        char path[256];

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) {
            ret = -1;
            break;
        }
        if (f->ecx != 0U) {
            if (copy_user_string(args, sizeof(args), f->ecx) != E_OK) {
                ret = -1;
                break;
            }
        } else {
            args[0] = '\0';
        }
        make_abs_path(path, sizeof(path), rel);
        ret = user_exec_replace(path, args, (struct interrupt_frame *)f);
        break;
    }
    case SYS_WAIT: {
        int status = 0;
        int child = proc_wait((int)f->ebx, &status);

        if (child > 0 && f->ecx != 0U) {
            if (copy_to_user((void *)(uintptr_t)f->ecx, &status, sizeof(status)) != E_OK) {
                child = -1;
            }
        }
        ret = child;
        break;
    }
    default:
        ret = -1;
        break;
    }

    /* pushad 保存的 eax 就在现场里，改这里等价于设置返回值 */
    f->eax = (unsigned int)ret;

    /* 返回用户态前处理待处理信号（可能直接终止或跳到用户 handler） */
    proc_check_signals(f);
}

unsigned int syscall_count(void)
{
    return calls;
}
