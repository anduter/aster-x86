/*
 * ng WP1-8 host 单测：syscall.c / uaccess.c 的“内核环境”替身。
 *
 * 这两个文件本身不依赖 CPU 指令（除了 stac/clac 在 SMAP=on 时），但会调用
 * 页表（pg_*）、串口（serial_*）、GDT（gdt_set_rsp0）、IPC 胶水（ipc_sys_*）
 * 和汇编符号（syscall_entry / ring3_enter / ...）。host 单测不链接 paging.c /
 * serial.c / gdt.c / ipc.c，所以由 fake_ng_env.c 提供同名的 fake 实现：
 *
 *   - fake 页表：测试把 host 上的一块静态缓冲区“登记”成用户页（U=1/W=1），
 *     uaccess.c 的逐页校验走 fake 表；拷贝仍然真的读写那块宿主内存，所以
 *     copyin/copyout 的字节语义和页边界都是真测的。
 *   - fake 串口：捕获 syscall write 的输出。
 *   - fake ring3 入口：syscall_run_user() 调 ring3_enter() 时执行测试注册的
 *     hook；hook 往 syscall_dispatch() 里灌 frame，等价于“用户态发了 syscall”。
 *
 * 注意：fake 页表只认 4 KiB 页；SMAP 默认 off（host 用户态执行 stac/clac
 * 会 #UD，所以 copyin/copyout 的 SMAP=on 路径只能由 QEMU 覆盖）。
 */
#ifndef NG_TEST_FAKE_ENV_H
#define NG_TEST_FAKE_ENV_H

#include "types.h"

#define FAKE_PAGE_MAX 32u

/* ---- fake 地址空间 -------------------------------------------------------- */
void fake_pg_reset(void);
/* 登记一页（page 必须 4 KiB 对齐）；flags 用 PG_*，PRESENT 自动补。 */
int fake_pg_map(u64 page, u64 flags);
int fake_pg_unmap(u64 page);
void fake_pg_set_null_pml4(int on);
u64 fake_pg_walk_count(void);

/* ---- fake 串口 ------------------------------------------------------------ */
void fake_serial_reset(void);
const char *fake_serial_text(void);
u32 fake_serial_len(void);

/* ---- fake GDT（TSS.RSP0 记录）-------------------------------------------- */
void fake_gdt_reset(void);
u64 fake_gdt_rsp0(void);
u32 fake_gdt_calls(void);

/* ---- fake ring3 入口 ------------------------------------------------------ */
typedef void (*fake_ring3_hook)(void *ctx);
void fake_ring3_set(fake_ring3_hook fn, void *ctx);

/* ---- fake IPC syscall 胶水 ------------------------------------------------ */
enum fake_ipc_fn {
    FAKE_IPC_CREATE = 10,
    FAKE_IPC_SEND = 11,
    FAKE_IPC_RECV = 12,
    FAKE_IPC_REPLY = 13,
    FAKE_IPC_CLOSE = 14
};

struct fake_ipc_call {
    u32 fn;
    u64 a;
    u64 b;
    u64 c;
    u64 d;
};

void fake_ipc_reset(void);
void fake_ipc_set_ret(u64 ret);
u32 fake_ipc_count(void);
const struct fake_ipc_call *fake_ipc_get(u32 i);

/* ---- fake 进程 syscall 胶水（WP3-0：40..44）------------------------------ */
/*
 * syscall.c 现在会调 proc_sys_*()；host 单测只验证分发/参数搬运，所以这里
 * 记录调用与参数，返回固定值（不建真进程/真线程）。
 */
enum fake_proc_fn {
    FAKE_PROC_SPAWN = 40,
    FAKE_PROC_WAIT = 41,
    FAKE_PROC_GETPID = 42,
    FAKE_PROC_KILL = 43,
    FAKE_PROC_GETPPID = 44
};

struct fake_proc_call {
    u32 fn;
    u64 a;
    u64 b;
    u64 c;
};

void fake_proc_reset(void);
void fake_proc_set_ret(u64 ret);
void fake_proc_set_pid(u64 pid);
u32 fake_proc_count(void);
const struct fake_proc_call *fake_proc_get(u32 i);

/* ---- fake 共享内存 / FB syscall 胶水（WP3-1：45..47）---------------------- */
/* syscall.c 会调 shm_sys_alloc/shm_sys_map/fbmap_sys_map；host 单测只验证
 * 分发与参数搬运，所以这里记录调用并返回固定值（不分配页/不映射 AS）。 */
enum fake_mm_fn {
    FAKE_MM_SHM_ALLOC = 45,
    FAKE_MM_SHM_MAP = 46,
    FAKE_MM_FB_MAP = 47
};

struct fake_mm_call {
    u32 fn;
    u64 a;
    u64 b;
    u64 c;
};

void fake_mm_reset(void);
void fake_mm_set_ret(u64 ret);
u32 fake_mm_count(void);
const struct fake_mm_call *fake_mm_get(u32 i);

/* ---- fake readkey（WP2-3：syscall 30 胶水）--------------------------------- */
/* 默认返回 NG_EAGAIN；fake_input_set_readkey 设定下一次（以及之后）的返回值。 */
void fake_input_reset(void);
void fake_input_set_readkey(u64 value);
u32 fake_input_readkey_calls(void);

/* ---- fake FS 数据页池（WP2-1：ramfs 的 fs_env） --------------------------- */
/* 8 个 4 KiB 页；page_alloc 返回清零页，page_free 归还。fake_fs_reset 全归还。 */
#include "ramfs.h"

#define FAKE_FS_PAGES 8u

void fake_fs_reset(void);
void fake_fs_env(struct fs_env *env);
u32 fake_fs_pages_live(void);

#endif /* NG_TEST_FAKE_ENV_H */
