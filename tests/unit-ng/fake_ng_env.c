/*
 * ng WP1-8 host 单测：fake 内核环境（见 fake_ng_env.h）。
 *
 * 这里定义的是 paging.c / serial.c / gdt.c / ipc.c 里那些被 syscall.c 和
 * uaccess.c 引用、但 host 单测不链接的符号。除 fake 页表外都只做记录。
 */
#include "fake_ng_env.h"

#include "gdt.h"
#include "ipc.h"
#include "paging.h"
#include "serial.h"
#include "wp23.h"

/* ------------------------------------------------------------------------- */
/* fake 4 KiB 页表                                                           */
/* ------------------------------------------------------------------------- */

struct fake_page {
    u64 page;
    u64 flags;
};

static struct fake_page fpages[FAKE_PAGE_MAX];
static u32 fpage_count;
static u64 fwalk_count;
static int fnull_pml4;
static u64 fake_pml4_sentinel[512];

void fake_pg_reset(void)
{
    u32 i;

    for (i = 0; i < FAKE_PAGE_MAX; i++) {
        fpages[i].page = 0;
        fpages[i].flags = 0;
    }
    fpage_count = 0;
    fwalk_count = 0;
    fnull_pml4 = 0;
    for (i = 0; i < 512; i++) {
        fake_pml4_sentinel[i] = 0;
    }
}

int fake_pg_map(u64 page, u64 flags)
{
    u32 i;

    if ((page & 0xFFFULL) != 0) {
        return PG_EINVAL;
    }
    for (i = 0; i < fpage_count; i++) {
        if (fpages[i].page == page) {
            fpages[i].flags = flags | PG_PRESENT;
            return PG_OK;
        }
    }
    if (fpage_count >= FAKE_PAGE_MAX) {
        return PG_ENOMEM;
    }
    fpages[fpage_count].page = page;
    fpages[fpage_count].flags = flags | PG_PRESENT;
    fpage_count++;
    return PG_OK;
}

int fake_pg_unmap(u64 page)
{
    u32 i;

    for (i = 0; i < fpage_count; i++) {
        if (fpages[i].page == page) {
            fpages[i] = fpages[fpage_count - 1];
            fpage_count--;
            return PG_OK;
        }
    }
    return PG_ENOENT;
}

void fake_pg_set_null_pml4(int on)
{
    fnull_pml4 = on;
}

u64 fake_pg_walk_count(void)
{
    return fwalk_count;
}

u64 *pg_current_pml4(void)
{
    return fnull_pml4 ? NULL : fake_pml4_sentinel;
}

u64 pg_walk_in(u64 *pml4, u64 va)
{
    u32 i;

    fwalk_count++;
    if (pml4 == NULL) {
        return 0;
    }
    for (i = 0; i < fpage_count; i++) {
        if (fpages[i].page == (va & ~0xFFFULL)) {
            return fpages[i].flags;
        }
    }
    return 0;
}

u64 pg_walk(u64 va)
{
    return pg_walk_in(fake_pml4_sentinel, va);
}

int pg_map(u64 va, u64 phys, u64 flags)
{
    (void)phys;
    return fake_pg_map(va & ~0xFFFULL, flags);
}

int pg_unmap(u64 va)
{
    return fake_pg_unmap(va & ~0xFFFULL);
}

int pg_protect(u64 va, u64 flags)
{
    return fake_pg_map(va & ~0xFFFULL, flags);
}

int pg_smap_on(void)
{
    /* host 用户态不能执行 stac/clac；SMAP 路径只由 QEMU 自检覆盖。 */
    return 0;
}

/* ------------------------------------------------------------------------- */
/* fake 串口（捕获 syscall write 的输出）                                     */
/* ------------------------------------------------------------------------- */

static char fserial[4096];
static u32 fserial_len;

void fake_serial_reset(void)
{
    fserial_len = 0;
    fserial[0] = '\0';
}

const char *fake_serial_text(void)
{
    fserial[fserial_len] = '\0';
    return fserial;
}

u32 fake_serial_len(void)
{
    return fserial_len;
}

void serial_putc(char c)
{
    if (fserial_len + 1 < sizeof(fserial)) {
        fserial[fserial_len++] = c;
        fserial[fserial_len] = '\0';
    }
}

void serial_puts(const char *s)
{
    while (s != NULL && *s != '\0') {
        serial_putc(*s++);
    }
}

void serial_puthex(u64 value)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[17];
    int n = 0;
    int i;

    serial_puts("0x");
    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value != 0 && n < 16) {
        tmp[n++] = digits[value & 0xFULL];
        value >>= 4;
    }
    for (i = n - 1; i >= 0; i--) {
        serial_putc(tmp[i]);
    }
}

void serial_putdec(u64 value)
{
    char tmp[21];
    int n = 0;
    int i;

    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value != 0 && n < 20) {
        tmp[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    for (i = n - 1; i >= 0; i--) {
        serial_putc(tmp[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* fake GDT                                                                   */
/* ------------------------------------------------------------------------- */

static u64 fgdt_rsp0;
static u32 fgdt_calls;

void fake_gdt_reset(void)
{
    fgdt_rsp0 = 0;
    fgdt_calls = 0;
}

u64 fake_gdt_rsp0(void)
{
    return fgdt_rsp0;
}

u32 fake_gdt_calls(void)
{
    return fgdt_calls;
}

void gdt_set_rsp0(u64 top)
{
    fgdt_rsp0 = top;
    fgdt_calls++;
}

/* ------------------------------------------------------------------------- */
/* fake ring3 入口 / 汇编符号                                                 */
/* ------------------------------------------------------------------------- */

static fake_ring3_hook f_hook;
static void *f_hook_ctx;

void fake_ring3_set(fake_ring3_hook fn, void *ctx)
{
    f_hook = fn;
    f_hook_ctx = ctx;
}

void ring3_enter(u64 user_rip, u64 user_rsp)
{
    (void)user_rip;
    (void)user_rsp;
    if (f_hook != NULL) {
        f_hook(f_hook_ctx);
    }
}

/* WP3-0：带 argc/argv 的入口在 host 上同样只触发 hook（不真的进 ring3）。 */
void ring3_enter_args(u64 user_rip, u64 user_rsp, u64 argc, u64 argv)
{
    (void)user_rip;
    (void)user_rsp;
    (void)argc;
    (void)argv;
    if (f_hook != NULL) {
        f_hook(f_hook_ctx);
    }
}

/* syscall.c 只把这两个当作“地址”用（LSTAR / boot 内核栈顶）。 */
char syscall_entry[16];
char syscall_stack_top[64];

/* WP1-3 静态用户页往返的汇编数据段（host 单测不执行 map_user_pages）。 */
char user_prog_start[16];
char user_prog_end[16];

void syscall_ring0_stub(void)
{
    /* host 单测直接调 syscall_dispatch()，不走汇编往返。 */
}

/* ------------------------------------------------------------------------- */
/* fake IPC 胶水（syscall dispatch 只测参数搬运/返回值，不测实现）             */
/* ------------------------------------------------------------------------- */

#define FAKE_IPC_MAX_CALLS 8u

static struct fake_ipc_call fipc_calls[FAKE_IPC_MAX_CALLS];
static u32 fipc_count;
static u64 fipc_ret;

void fake_ipc_reset(void)
{
    u32 i;

    for (i = 0; i < FAKE_IPC_MAX_CALLS; i++) {
        fipc_calls[i].fn = 0;
        fipc_calls[i].a = 0;
        fipc_calls[i].b = 0;
        fipc_calls[i].c = 0;
        fipc_calls[i].d = 0;
    }
    fipc_count = 0;
    fipc_ret = 0;
}

void fake_ipc_set_ret(u64 ret)
{
    fipc_ret = ret;
}

u32 fake_ipc_count(void)
{
    return fipc_count;
}

const struct fake_ipc_call *fake_ipc_get(u32 i)
{
    if (i >= fipc_count || i >= FAKE_IPC_MAX_CALLS) {
        return NULL;
    }
    return &fipc_calls[i];
}

static u64 fake_ipc_record(u32 fn, u64 a, u64 b, u64 c, u64 d)
{
    if (fipc_count < FAKE_IPC_MAX_CALLS) {
        fipc_calls[fipc_count].fn = fn;
        fipc_calls[fipc_count].a = a;
        fipc_calls[fipc_count].b = b;
        fipc_calls[fipc_count].c = c;
        fipc_calls[fipc_count].d = d;
    }
    fipc_count++;
    return fipc_ret;
}

u64 ipc_sys_create(u64 depth)
{
    return fake_ipc_record(FAKE_IPC_CREATE, depth, 0, 0, 0);
}

u64 ipc_sys_send(u64 handle, u64 user_msg, u64 len, u64 timeout)
{
    return fake_ipc_record(FAKE_IPC_SEND, handle, user_msg, len, timeout);
}

u64 ipc_sys_recv(u64 handle, u64 user_buf, u64 max, u64 timeout)
{
    return fake_ipc_record(FAKE_IPC_RECV, handle, user_buf, max, timeout);
}

u64 ipc_sys_reply(u64 ticket, u64 user_msg, u64 len, u64 timeout)
{
    return fake_ipc_record(FAKE_IPC_REPLY, ticket, user_msg, len, timeout);
}

u64 ipc_sys_close(u64 handle)
{
    return fake_ipc_record(FAKE_IPC_CLOSE, handle, 0, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* fake 进程 syscall 胶水（WP3-0：40..44）                                    */
/* ------------------------------------------------------------------------- */

#define FAKE_PROC_MAX 8u

static struct fake_proc_call fproc_calls[FAKE_PROC_MAX];
static u32 fproc_count;
static u64 fproc_ret;
static u64 fproc_pid = 33;

void fake_proc_reset(void)
{
    u32 i;

    for (i = 0; i < FAKE_PROC_MAX; i++) {
        fproc_calls[i].fn = 0;
        fproc_calls[i].a = 0;
        fproc_calls[i].b = 0;
        fproc_calls[i].c = 0;
    }
    fproc_count = 0;
    fproc_ret = 0;
    fproc_pid = 33;
}

void fake_proc_set_ret(u64 ret)
{
    fproc_ret = ret;
}

void fake_proc_set_pid(u64 pid)
{
    fproc_pid = pid;
}

u32 fake_proc_count(void)
{
    return fproc_count;
}

const struct fake_proc_call *fake_proc_get(u32 i)
{
    if (i >= fproc_count || i >= FAKE_PROC_MAX) {
        return NULL;
    }
    return &fproc_calls[i];
}

static u64 fake_proc_record(u32 fn, u64 a, u64 b, u64 c)
{
    if (fproc_count < FAKE_PROC_MAX) {
        fproc_calls[fproc_count].fn = fn;
        fproc_calls[fproc_count].a = a;
        fproc_calls[fproc_count].b = b;
        fproc_calls[fproc_count].c = c;
    }
    fproc_count++;
    return fproc_ret;
}

u64 proc_sys_spawn(u64 upath, u64 uargv, u64 upid)
{
    return fake_proc_record(FAKE_PROC_SPAWN, upath, uargv, upid);
}

u64 proc_sys_wait(u64 pid, u64 ustatus)
{
    return fake_proc_record(FAKE_PROC_WAIT, pid, ustatus, 0);
}

u64 proc_sys_getpid(void)
{
    (void)fake_proc_record(FAKE_PROC_GETPID, 0, 0, 0);
    return fproc_pid;
}

u64 proc_sys_kill(u64 pid, u64 sig)
{
    return fake_proc_record(FAKE_PROC_KILL, pid, sig, 0);
}

u64 proc_sys_getppid(void)
{
    return fake_proc_record(FAKE_PROC_GETPPID, 0, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* fake 共享内存 / FB syscall 胶水（WP3-1：45..47）                           */
/* ------------------------------------------------------------------------- */

#define FAKE_MM_MAX 8u

static struct fake_mm_call fmm_calls[FAKE_MM_MAX];
static u32 fmm_count;
static u64 fmm_ret;

void fake_mm_reset(void)
{
    u32 i;

    for (i = 0; i < FAKE_MM_MAX; i++) {
        fmm_calls[i].fn = 0;
        fmm_calls[i].a = 0;
        fmm_calls[i].b = 0;
        fmm_calls[i].c = 0;
    }
    fmm_count = 0;
    fmm_ret = 0;
}

void fake_mm_set_ret(u64 ret)
{
    fmm_ret = ret;
}

u32 fake_mm_count(void)
{
    return fmm_count;
}

const struct fake_mm_call *fake_mm_get(u32 i)
{
    if (i >= fmm_count || i >= FAKE_MM_MAX) {
        return NULL;
    }
    return &fmm_calls[i];
}

static u64 fake_mm_record(u32 fn, u64 a, u64 b, u64 c)
{
    if (fmm_count < FAKE_MM_MAX) {
        fmm_calls[fmm_count].fn = fn;
        fmm_calls[fmm_count].a = a;
        fmm_calls[fmm_count].b = b;
        fmm_calls[fmm_count].c = c;
    }
    fmm_count++;
    return fmm_ret;
}

u64 shm_sys_alloc(u64 pages, u64 uva_out, u64 uhandle_out)
{
    return fake_mm_record(FAKE_MM_SHM_ALLOC, pages, uva_out, uhandle_out);
}

u64 shm_sys_map(u64 handle, u64 uva_out)
{
    return fake_mm_record(FAKE_MM_SHM_MAP, handle, uva_out, 0);
}

u64 fbmap_sys_map(u64 uva_out, u64 uinfo_out)
{
    return fake_mm_record(FAKE_MM_FB_MAP, uva_out, uinfo_out, 0);
}

/* ------------------------------------------------------------------------- */
/* fake readkey（WP2-3：syscall 30 的分发只在 host 上验证“透传 + 返回值”）    */
/* ------------------------------------------------------------------------- */

static u64 freadkey_value = (u64)(i64)NG_EAGAIN;
static u32 freadkey_calls;

void fake_input_reset(void)
{
    freadkey_value = (u64)(i64)NG_EAGAIN;
    freadkey_calls = 0;
}

void fake_input_set_readkey(u64 value)
{
    freadkey_value = value;
}

u32 fake_input_readkey_calls(void)
{
    return freadkey_calls;
}

u64 ng_input_readkey(void)
{
    freadkey_calls++;
    return freadkey_value;
}

/* ------------------------------------------------------------------------- */
/* fake 96/97：输入事件与所有权（WP5-0h）                                    */
/* ------------------------------------------------------------------------- */

static int fevent_rc = -11;
static struct input_packet fevent_pkt;
static u32 fevent_calls;

void fake_input_event_reset(void)
{
    fevent_rc = -11;
    fevent_calls = 0;
    fevent_pkt.type = 0;
    fevent_pkt.key = 0;
    fevent_pkt.x = 0;
    fevent_pkt.y = 0;
    fevent_pkt.buttons = 0;
    fevent_pkt.scancode = 0;
    fevent_pkt.flags = 0;
}

void fake_input_event_set(int rc, const struct input_packet *pk)
{
    fevent_rc = rc;
    if (pk != NULL) {
        fevent_pkt = *pk;
    }
}

u32 fake_input_event_calls(void)
{
    return fevent_calls;
}

int ng_input_event(struct input_packet *out)
{
    fevent_calls++;
    if (fevent_rc == 0 && out != NULL) {
        *out = fevent_pkt;
    }
    return fevent_rc;
}

static i64 fclaim_rc;
static u32 fclaim_calls;
static u64 fclaim_pid;
static u64 fclaim_flags;

void fake_input_claim_reset(void)
{
    fclaim_rc = 0;
    fclaim_calls = 0;
    fclaim_pid = 0;
    fclaim_flags = 0;
}

void fake_input_claim_set(i64 rc)
{
    fclaim_rc = rc;
}

u32 fake_input_claim_calls(void)
{
    return fclaim_calls;
}

u64 fake_input_claim_last_pid(void)
{
    return fclaim_pid;
}

u64 fake_input_claim_last_flags(void)
{
    return fclaim_flags;
}

i64 ng_input_claim(u64 pid, u64 flags)
{
    fclaim_calls++;
    fclaim_pid = pid;
    fclaim_flags = flags;
    return fclaim_rc;
}

/* ------------------------------------------------------------------------- */
/* fake FS 数据页池（WP2-1）                                                  */
/* ------------------------------------------------------------------------- */

static u8 ffs_pages[FAKE_FS_PAGES][4096] __attribute__((aligned(4096)));
static u8 ffs_used[FAKE_FS_PAGES];

void fake_fs_reset(void)
{
    u32 i;

    for (i = 0; i < FAKE_FS_PAGES; i++) {
        ffs_used[i] = 0;
    }
}

static void *fake_fs_page_alloc(void *ctx)
{
    u32 i;

    (void)ctx;
    for (i = 0; i < FAKE_FS_PAGES; i++) {
        if (ffs_used[i] == 0) {
            u32 k;

            ffs_used[i] = 1;
            for (k = 0; k < 4096; k++) {
                ffs_pages[i][k] = 0; /* 与 PMM_ZERO_ON_ALLOC 一致 */
            }
            return ffs_pages[i];
        }
    }
    return NULL;
}

static void fake_fs_page_free(void *ctx, void *page)
{
    u32 i;

    (void)ctx;
    for (i = 0; i < FAKE_FS_PAGES; i++) {
        if ((void *)ffs_pages[i] == page) {
            ffs_used[i] = 0;
            return;
        }
    }
}

void fake_fs_env(struct fs_env *env)
{
    if (env == NULL) {
        return;
    }
    env->page_alloc = fake_fs_page_alloc;
    env->page_free = fake_fs_page_free;
    env->ctx = NULL;
}

u32 fake_fs_pages_live(void)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < FAKE_FS_PAGES; i++) {
        if (ffs_used[i] != 0) {
            n++;
        }
    }
    return n;
}
