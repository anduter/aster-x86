/*
 * ng WP3-1: 共享内存（数据面）+ syscall 45/46 的内核机制。
 *
 * 为什么需要它：display server 的 surface 像素是"数据面"，不能走 4 KiB IPC
 * 消息（architecture.md 不变量 7）。客户端和 displayd 必须映射**同一批物理
 * 页**，控制面仍然只发 `{surface_id, shm_handle}` 这种小消息。
 *
 * 分两层（与 ipc/sched/proc 同一套路）：
 *   1. `shm_core.c`：**纯状态机**。对象表、handle=((gen<<16)|slot)、refcount、
 *      owner/root 权限、mapping 记录（pid+VA）、页分配/释放都通过
 *      `struct shm_env` 注入，所以 host 单测可以用 fake 页池直接驱动
 *      （tests/unit-ng/test_shm.c）。
 *   2. `shm.c`：内核胶水。PMM 页、`as_map` 到调用者用户 AS、uaccess 校验、
 *      进程退出/被 kill 时先 `pg_unmap_in`（**不归还物理页**）再让 core 按
 *      refcount 决定是否 free —— 这一点必须和 `as_destroy` 的顺序配合：
 *      as_destroy 会 free 每个 present 的叶子页，共享页若还映射着就会被
 *      double free。
 *
 * 语义（WP3-1 边界）：
 *   - `SYS_SHM_ALLOC(pages, &va, &handle)`：分配 pages 个清零页，映射进调用者
 *     AS（RW+NX），返回 VA 与 handle。handle 与端口 ticket 同构
 *     `(gen<<16)|slot`，但属于**独立命名空间**，只能经 IPC 传给别的进程。
 *   - `SYS_SHM_MAP(handle, &va)`：把同一批页映射进调用者 AS。权限：调用者是
 *     owner，或 uid==0（root）。cap 授权后置（ABI 1.0 / WP3-3）。
 *   - refcount = 当前映射数；最后一个映射解除时归还物理页。进程退出/被 kill
 *     时按 pid 回收自己的映射（core 的 release_pid）。
 *   - VA 由每个进程的 bump 分配器给（`proc->shm_next_va`，起点
 *     SHM_VA_BASE，每个对象之间留一页守护空洞），所以同一个对象在不同进程
 *     里可以落在不同 VA —— 完全正常。
 *   - 没有 SYS_SHM_FREE：surface DESTROY 后 displayd 的映射要等进程退出才
 *     释放（WP3-3 的 surface 生命周期再补）。
 */
#ifndef NG_SHM_H
#define NG_SHM_H

#include "handle.h"
#include "types.h"

/* ---- 上限 / 窗口 ---------------------------------------------------------- */
#define SHM_MAX 8u
#define SHM_PAGES_MAX 1024u
#define SHM_MAPS_MAX 8u
#define SHM_PAGE_SIZE 4096ULL
#define SHM_VA_BASE 0x0000000050000000ULL /* 1.25 GiB，用户窗口内、ELF/栈之外 */
#define SHM_VA_LIMIT 0x000000005F000000ULL

/* ---- 错误码（负 errno 风格） --------------------------------------------- */
#define SHM_OK 0
#define SHM_EPERM (-1)
#define SHM_ENOENT (-2)
#define SHM_EBADF (-9)
#define SHM_ENOMEM (-12)
#define SHM_EFAULT (-14)
#define SHM_EBUSY (-16)
#define SHM_EINVAL (-22)
#define SHM_ENOSPC (-28)

/* ---- 映射记录（pid + VA） ------------------------------------------------- */
struct shm_map {
    u32 used;
    u32 pid;
    u64 va;
};

/* ---- 共享内存对象 --------------------------------------------------------- */
struct shm_obj {
    u32 used;
    u32 slot; /* 对象槽位（handle 低 16 位） */
    u32 gen;  /* generation（handle 高 16 位；复用槽位时 +1） */
    u32 owner_pid;
    u32 owner_uid;
    u32 pages;
    u32 refcnt; /* 当前映射数 */
    u64 *phys;  /* pages 个物理页（从 env 分配；不占 .bss） */
    struct shm_map maps[SHM_MAPS_MAX];
    u32 map_count;
};

/* ---- 环境钩子（内核绑 PMM；host 单测绑 fake 页池） ----------------------- */
struct shm_env {
    void *ctx;
    /* 分配 count 个页并把物理地址写进 out[]；成功 0，失败 -1（必须回滚）。 */
    int (*alloc_pages)(u32 count, u64 *out, void *ctx);
    void (*free_page)(u64 phys, void *ctx);
    /* 对象的 phys[] 数组本身（pages * 8 字节）；内核绑 kmalloc/kfree。 */
    void *(*alloc)(u64 size, void *ctx);
    void (*free)(void *p, void *ctx);
};

/* ---- 纯状态机 ------------------------------------------------------------- */
struct shm_stats {
    u32 live;      /* 存活对象数 */
    u32 peak;
    u64 allocs;
    u64 maps;
    u64 unmaps;
    u64 page_allocs;
    u64 page_frees;
    u32 perms; /* 权限拒绝数 */
    u32 bad;   /* 无效 handle 数 */
    u32 full;  /* 表满拒绝数 */
    u32 busy;  /* 重复 map / 非法 abort */
};

struct shm_table {
    struct shm_obj objs[SHM_MAX];
    u32 next_gen;
    struct shm_stats stat;
    const struct shm_env *env;
};

void shm_table_init(struct shm_table *t, const struct shm_env *env);

/*
 * 分配对象：pages 个页 + 首个映射 (owner_pid, owner_va)。
 * 成功返回 SHM_OK 并写 *out_handle；失败返回负错误码（已分配页已回滚）。
 */
int shm_core_alloc(struct shm_table *t, u32 owner_pid, u32 owner_uid, u32 pages,
                   u64 owner_va, u32 *out_handle);

/* 解析 handle；无效返回 NULL 并写 *err（SHM_EBADF）。 */
struct shm_obj *shm_core_find(struct shm_table *t, u32 handle, int *err);

/* handle + owner/root 权限校验；失败返回 NULL 并写 *err（EBADF/EPERM）。 */
struct shm_obj *shm_core_lookup(struct shm_table *t, u32 handle, u32 pid,
                                u32 uid, int *err);

/* 把对象映射进 pid（VA 由调用者选好并记录）。成功返回 SHM_OK。 */
int shm_core_map(struct shm_table *t, u32 handle, u32 pid, u32 uid, u64 va,
                 struct shm_obj **out);

/* 解除 (pid,va) 的映射；refcnt 归零时归还物理页。返回 1=对象已释放。 */
int shm_core_unmap(struct shm_table *t, u32 pid, u64 va);

/* 收集 pid 的所有映射 VA（最多 max 个），返回条数。 */
u32 shm_core_collect_pid(struct shm_table *t, u32 pid, u64 *out_va, u32 max);

/* 解除 pid 的所有映射并递减 refcount；返回解除的映射数。 */
u32 shm_core_release_pid(struct shm_table *t, u32 pid);

/* 分配失败回滚：只有 refcnt==1 且只有一个映射时允许。 */
int shm_core_abort(struct shm_table *t, u32 handle);

u32 shm_core_live(const struct shm_table *t);
u32 shm_core_refs(struct shm_table *t, u32 handle);
const struct shm_stats *shm_core_stat(const struct shm_table *t);

/* ---- 内核胶水 ------------------------------------------------------------- */
void shm_init(void);
struct shm_table *shm_global(void);
/* syscall 45：size_pages -> 调用者 AS 的 VA + 可跨进程的 handle。 */
u64 shm_sys_alloc(u64 pages, u64 uva_out, u64 uhandle_out);
/* syscall 46：handle -> 调用者 AS 的 VA。 */
u64 shm_sys_map(u64 handle, u64 uva_out);
/* 进程退出/被 kill：先从 AS 摘掉共享页（不 free），再按 refcount 回收。 */
struct proc;
void shm_release_proc(struct proc *p);
u32 shm_live(void);

#endif /* NG_SHM_H */
