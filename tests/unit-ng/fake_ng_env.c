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
