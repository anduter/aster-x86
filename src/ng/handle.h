/*
 * ng WP1-6: 每进程句柄表（slot + generation，类型化对象引用）。
 *
 * 句柄是 32 位不透明值：`(generation << 16) | slot`。
 *   - slot 是表内下标（0..HANDLE_MAX-1），generation 每关闭一次 +1，
 *     16 位回绕时跳过 0（0 永远表示“无效”）。
 *   - 关闭只“失效 + 递增 generation”，不压缩槽位；槽位复用后旧句柄因为
 *     generation 不匹配会被拒绝（stale handle），这是本 WP 的核心语义。
 *   - 类型（PORT / THREAD / FILE）在分配时写入、查找时校验；类型不匹配
 *     与坏句柄同样返回 HANDLE_EBADF，避免把 THREAD 句柄当 PORT 用。
 *
 * 本文件是纯数据结构（不依赖 heap/串口/调度），host 单测直接构造
 * struct handle_table 驱动（tests/unit-ng/test_ipc.c）。
 *
 * 作用域：ng 现在只有“一个内核地址空间 + 内核线程”，所以 handle_table 由
 * ipc.c 全局持有（所有内核线程共享）。等有进程/WP1-7 之后改成每进程一张表，
 * 本文件的 API 不变（TODO，见 docs/claims/ng-WP1-6.md“遗留”）。
 */
#ifndef NG_HANDLE_H
#define NG_HANDLE_H

#include "types.h"

/* 表大小：64 槽。port 池只有 16 个，多出来的槽留给 THREAD/FILE 预留类型。 */
#define HANDLE_MAX 64u

/* 句柄编码：低 16 位槽位，高 16 位 generation。 */
#define HANDLE_SLOT_MASK 0xFFFFu
#define HANDLE_GEN_SHIFT 16

/* 对象类型。0 = 空槽；THREAD/FILE 本 WP 只保留类型、不实现对象语义。 */
#define HANDLE_TYPE_NONE 0u
#define HANDLE_TYPE_PORT 1u
#define HANDLE_TYPE_THREAD 2u
#define HANDLE_TYPE_FILE 3u
#define HANDLE_TYPE_LAST HANDLE_TYPE_FILE

/* 错误码（数值与 IPC_* 共用一套 POSIX 风格负值，见 ipc.h） */
#define HANDLE_OK 0
#define HANDLE_EPERM (-1)
#define HANDLE_ENOENT (-2)
#define HANDLE_EBADF (-9)
#define HANDLE_ENOMEM (-12)
#define HANDLE_EINVAL (-22)
#define HANDLE_ENOSPC (-28)

struct handle_slot {
    u32 gen;  /* 0 = 从未使用；关闭后 +1（跳过 0） */
    u32 type; /* HANDLE_TYPE_* */
    void *obj;
};

struct handle_table {
    struct handle_slot slots[HANDLE_MAX];
    u32 live;      /* 已分配（type != NONE）的槽数 */
    u32 peak;      /* 历史最高 live */
    u64 allocs;    /* 成功分配次数 */
    u64 closes;    /* 成功关闭次数 */
    u32 bad_lookups;   /* generation 不匹配 / 槽位越界 / gen=0 */
    u32 type_errors;   /* 类型不匹配 */
    u32 full_rejects;  /* 表满被拒次数 */
};

void handle_table_init(struct handle_table *t);

/* 分配一个句柄；out_handle 非空时写入。表满返回 HANDLE_ENOSPC。 */
int handle_alloc(struct handle_table *t, u32 type, void *obj, u32 *out_handle);

/* 按句柄 + 期望类型取对象。gen/槽位/类型任一不符返回 HANDLE_EBADF。 */
int handle_lookup(struct handle_table *t, u32 handle, u32 type, void **out_obj);

/* 不校验类型的查找（close/诊断用）；out_type 可空。 */
int handle_lookup_any(struct handle_table *t, u32 handle, u32 *out_type,
                      void **out_obj);

/* 关闭句柄：递增 generation、清空槽位。out_type/out_obj 可空。 */
int handle_close(struct handle_table *t, u32 handle, u32 *out_type,
                 void **out_obj);

/* 诊断：live 计数、高水位、是否合法句柄（只查槽位/gen，不查类型）。 */
u32 handle_live(const struct handle_table *t);
u32 handle_peak(const struct handle_table *t);
int handle_is_valid(struct handle_table *t, u32 handle);

/*
 * ng WP3-0：按 (type, obj) 找表内句柄（type=HANDLE_TYPE_NONE 表示任意类型）。
 * 找到返回 HANDLE_OK 并写 *out_handle；没有返回 HANDLE_ENOENT。
 */
int handle_find_obj(struct handle_table *t, u32 type, const void *obj,
                    u32 *out_handle);

/* 编码/解码 helper（ticket 与 handle 同构：ticket = (gen<<16)|slot）。 */
static inline u32 handle_make(u32 gen, u32 slot)
{
    return ((gen & HANDLE_SLOT_MASK) << HANDLE_GEN_SHIFT) |
           (slot & HANDLE_SLOT_MASK);
}

static inline u32 handle_slot_of(u32 handle)
{
    return handle & HANDLE_SLOT_MASK;
}

static inline u32 handle_gen_of(u32 handle)
{
    return (handle >> HANDLE_GEN_SHIFT) & HANDLE_SLOT_MASK;
}

static inline int handle_type_valid(u32 type)
{
    return type >= HANDLE_TYPE_PORT && type <= HANDLE_TYPE_LAST;
}

#endif /* NG_HANDLE_H */
