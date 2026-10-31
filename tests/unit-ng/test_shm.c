/*
 * ng WP3-1 host 单测：共享内存对象表（src/ng/shm_core.c）。
 *
 * 用一块宿主内存当"物理页池"（env 注入），逐条验证：
 *   - handle = (gen<<16)|slot、槽位复用后 generation 递增、旧 handle 立即失效；
 *   - owner/root 权限（uid==0 是 root；当前内核所有进程都是 0，但表语义先测）；
 *   - refcount：每多一个映射 +1，最后一个映射解除才归还物理页；
 *   - release_pid / collect_pid（进程退出/被 kill 的回收路径）；
 *   - 分配失败回滚、表满、页数上限、abort 的前置条件；
 *   - 统计计数（allocs/maps/unmaps/perms/bad/full/busy/page_frees）。
 *
 * 页池不依赖 PMM：fake 页地址就是宿主缓冲区地址（shm_core 只把它们当不透明
 * 的 u64 传递）。
 */
#include <stdlib.h>
#include <string.h>

#include "shm.h"
#include "test_util.h"

#define FAKE_POOL_PAGES 64u

static unsigned char fake_pool[FAKE_POOL_PAGES][4096]
    __attribute__((aligned(4096)));
static unsigned char fake_used[FAKE_POOL_PAGES];
static u32 fake_frees;
static u32 fake_live_pages;

static void fake_pool_reset(void)
{
    memset(fake_used, 0, sizeof(fake_used));
    fake_frees = 0;
    fake_live_pages = 0;
}

static int fake_alloc_pages(u32 count, u64 *out, void *ctx)
{
    u32 i;
    u32 got = 0;

    (void)ctx;
    if (count == 0 || count > FAKE_POOL_PAGES) {
        return -1;
    }
    for (i = 0; i < FAKE_POOL_PAGES && got < count; i++) {
        if (fake_used[i]) {
            continue;
        }
        fake_used[i] = 1;
        out[got] = (u64)(usize)fake_pool[i];
        got++;
        fake_live_pages++;
    }
    if (got != count) {
        /* 回滚这一次已经标记的页。 */
        u32 k;

        for (k = 0; k < got; k++) {
            u32 idx = (u32)(((usize)out[k] - (usize)fake_pool) / 4096u);

            fake_used[idx] = 0;
            fake_live_pages--;
        }
        return -1;
    }
    return 0;
}

static void fake_free_page(u64 phys, void *ctx)
{
    usize off = (usize)phys - (usize)fake_pool;
    u32 idx;

    (void)ctx;
    if ((usize)phys < (usize)fake_pool) {
        return;
    }
    idx = (u32)(off / 4096u);
    if (idx >= FAKE_POOL_PAGES) {
        return;
    }
    if (!fake_used[idx]) {
        return;
    }
    fake_used[idx] = 0;
    fake_frees++;
    fake_live_pages--;
}

static void *fake_alloc(u64 size, void *ctx)
{
    (void)ctx;
    return malloc((size_t)size);
}

static void fake_free(void *p, void *ctx)
{
    (void)ctx;
    free(p);
}

static const struct shm_env g_env = {NULL, fake_alloc_pages, fake_free_page,
                                     fake_alloc, fake_free};

/* ------------------------------------------------------------------------- */

static void t_alloc_basic(void)
{
    struct shm_table t;
    struct shm_obj *o;
    u32 handle = 0;
    int err = 0;

    fake_pool_reset();
    shm_table_init(&t, &g_env);
    CHECK(shm_core_live(&t) == 0);
    CHECK(shm_core_stat(&t)->allocs == 0);

    CHECK(shm_core_alloc(&t, 10, 0, 4, 0x1000, &handle) == SHM_OK);
    CHECK(handle != 0);
    CHECK(shm_core_live(&t) == 1);
    CHECK(handle_gen_of(handle) == 1);
    CHECK(handle_slot_of(handle) == 0);
    CHECK(shm_core_refs(&t, handle) == 1);

    o = shm_core_find(&t, handle, &err);
    CHECK(o != NULL && err == SHM_OK);
    CHECK(o != NULL && o->owner_pid == 10);
    CHECK(o != NULL && o->pages == 4);
    CHECK(o != NULL && o->map_count == 1);
    CHECK(o != NULL && o->maps[0].pid == 10 && o->maps[0].va == 0x1000);
    CHECK(o != NULL && o->phys != NULL);
    CHECK(fake_live_pages == 4);
    CHECK(shm_core_stat(&t)->page_allocs == 4);
    CHECK(shm_core_stat(&t)->maps == 1);

    /* 非法页数 */
    CHECK(shm_core_alloc(&t, 10, 0, 0, 0, &handle) == SHM_EINVAL);
    CHECK(shm_core_alloc(&t, 10, 0, SHM_PAGES_MAX + 1u, 0, &handle) ==
          SHM_EINVAL);
    CHECK(shm_core_stat(&t)->bad >= 2);

    /* 无效 handle */
    CHECK(shm_core_find(&t, 0, &err) == NULL && err == SHM_EBADF);
    CHECK(shm_core_find(&t, 0x00010000u | 9u, &err) == NULL);
}

static void t_permission(void)
{
    struct shm_table t;
    u32 handle = 0;
    int err = 0;

    fake_pool_reset();
    shm_table_init(&t, &g_env);
    CHECK(shm_core_alloc(&t, 10, 0, 2, 0x1000, &handle) == SHM_OK);

    CHECK(shm_core_lookup(&t, handle, 10, 0, &err) != NULL);
    CHECK(shm_core_lookup(&t, handle, 11, 0, &err) != NULL); /* root */
    CHECK(shm_core_lookup(&t, handle, 11, 1000, &err) == NULL);
    CHECK(err == SHM_EPERM);
    CHECK(shm_core_stat(&t)->perms == 1);

    CHECK(shm_core_map(&t, handle, 11, 1000, 0x2000, NULL) == SHM_EPERM);
    CHECK(shm_core_map(&t, handle, 11, 0, 0x2000, NULL) == SHM_OK);
    CHECK(shm_core_refs(&t, handle) == 2);

    CHECK(shm_core_map(&t, handle, 11, 0, 0x2000, NULL) == SHM_EBUSY);
    CHECK(shm_core_stat(&t)->busy == 1);
}

static void t_refcount_free(void)
{
    struct shm_table t;
    u32 handle = 0;
    u32 other = 0;

    fake_pool_reset();
    shm_table_init(&t, &g_env);
    CHECK(shm_core_alloc(&t, 10, 0, 3, 0x1000, &handle) == SHM_OK);
    CHECK(shm_core_map(&t, handle, 11, 0, 0x2000, NULL) == SHM_OK);
    CHECK(shm_core_map(&t, handle, 12, 0, 0x3000, NULL) == SHM_OK);
    CHECK(shm_core_refs(&t, handle) == 3);
    CHECK(fake_live_pages == 3);

    CHECK(shm_core_unmap(&t, 11, 0x2000) == 0); /* 还有映射，不释放 */
    CHECK(shm_core_refs(&t, handle) == 2);
    CHECK(fake_live_pages == 3);
    CHECK(shm_core_unmap(&t, 11, 0x2000) == -1); /* 没这个映射 */
    CHECK(shm_core_unmap(&t, 12, 0x3000) == 0);
    CHECK(fake_live_pages == 3);
    CHECK(shm_core_refs(&t, handle) == 1);
    CHECK(shm_core_unmap(&t, 10, 0x1000) == 1); /* 最后一个：释放 */
    CHECK(shm_core_refs(&t, handle) == 0);
    CHECK(shm_core_live(&t) == 0);
    CHECK(fake_live_pages == 0);
    CHECK(fake_frees == 3);
    CHECK(shm_core_stat(&t)->page_frees == 3);
    CHECK(shm_core_stat(&t)->unmaps == 3);

    /* 槽位复用：generation 递增，旧 handle 失效。 */
    CHECK(shm_core_alloc(&t, 10, 0, 1, 0x4000, &other) == SHM_OK);
    CHECK(handle_slot_of(other) == handle_slot_of(handle));
    CHECK(handle_gen_of(other) != handle_gen_of(handle));
    CHECK(shm_core_find(&t, handle, NULL) == NULL);
    CHECK(shm_core_find(&t, other, NULL) != NULL);
}

static void t_release_pid(void)
{
    struct shm_table t;
    u32 a = 0;
    u32 b = 0;
    u64 vas[8];
    u32 n;

    fake_pool_reset();
    shm_table_init(&t, &g_env);
    CHECK(shm_core_alloc(&t, 10, 0, 2, 0x1000, &a) == SHM_OK);
    CHECK(shm_core_alloc(&t, 10, 0, 2, 0x3000, &b) == SHM_OK);
    CHECK(shm_core_map(&t, a, 11, 0, 0x5000, NULL) == SHM_OK);

    n = shm_core_collect_pid(&t, 10, vas, 8);
    CHECK(n == 2);
    CHECK((vas[0] == 0x1000 && vas[1] == 0x3000) ||
          (vas[0] == 0x3000 && vas[1] == 0x1000));
    CHECK(shm_core_collect_pid(&t, 10, NULL, 0) == 2);
    CHECK(shm_core_collect_pid(&t, 99, vas, 8) == 0);

    /* 回收 pid 10：a 还有 pid 11 的映射，所以 a 不释放；b 释放。 */
    CHECK(shm_core_release_pid(&t, 10) == 2);
    CHECK(shm_core_live(&t) == 1);
    CHECK(shm_core_refs(&t, a) == 1);
    CHECK(fake_live_pages == 2);
    CHECK(shm_core_release_pid(&t, 10) == 0); /* 幂等 */

    /* 回收 pid 11：a 的最后一个映射，释放。 */
    CHECK(shm_core_release_pid(&t, 11) == 1);
    CHECK(shm_core_live(&t) == 0);
    CHECK(fake_live_pages == 0);
    (void)b;
}

static void t_abort_and_limits(void)
{
    struct shm_table t;
    u32 handle = 0;
    u32 handles[SHM_MAX];
    u32 i;

    fake_pool_reset();
    shm_table_init(&t, &g_env);
    CHECK(shm_core_alloc(&t, 10, 0, 2, 0x1000, &handle) == SHM_OK);
    CHECK(shm_core_abort(&t, handle) == SHM_OK);
    CHECK(shm_core_live(&t) == 0);
    CHECK(fake_live_pages == 0);
    CHECK(shm_core_abort(&t, handle) == SHM_EBADF);

    CHECK(shm_core_alloc(&t, 10, 0, 2, 0x1000, &handle) == SHM_OK);
    CHECK(shm_core_map(&t, handle, 11, 0, 0x2000, NULL) == SHM_OK);
    CHECK(shm_core_abort(&t, handle) == SHM_EBUSY);
    CHECK(shm_core_release_pid(&t, 10) == 1);
    CHECK(shm_core_release_pid(&t, 11) == 1);
    CHECK(shm_core_live(&t) == 0);

    /* 表满：SHM_MAX 个对象后 ENOSPC。 */
    for (i = 0; i < SHM_MAX; i++) {
        handles[i] = 0;
        CHECK(shm_core_alloc(&t, 10, 0, 1, 0x1000 + i * 0x1000, &handles[i]) ==
              SHM_OK);
    }
    CHECK(shm_core_live(&t) == SHM_MAX);
    CHECK(shm_core_alloc(&t, 10, 0, 1, 0x9000, &handle) == SHM_ENOSPC);
    CHECK(shm_core_stat(&t)->full >= 1);

    /* 每个对象最多 SHM_MAPS_MAX 个映射。 */
    for (i = 1; i < SHM_MAPS_MAX; i++) {
        CHECK(shm_core_map(&t, handles[0], 100 + i, 0,
                           0x10000 + (u64)i * 0x1000, NULL) == SHM_OK);
    }
    CHECK(shm_core_map(&t, handles[0], 200, 0, 0x20000, NULL) == SHM_ENOSPC);
    CHECK(shm_core_refs(&t, handles[0]) == SHM_MAPS_MAX);
    CHECK(shm_core_release_pid(&t, 10) == SHM_MAX);
    for (i = 1; i < SHM_MAPS_MAX; i++) {
        CHECK(shm_core_release_pid(&t, 100 + i) == 1);
    }
    CHECK(shm_core_live(&t) == 0);
    CHECK(fake_live_pages == 0);
}

static void t_stats(void)
{
    struct shm_table t;
    u32 handle = 0;

    fake_pool_reset();
    shm_table_init(&t, &g_env);
    CHECK(shm_core_stat(&t)->peak == 0);
    CHECK(shm_core_alloc(&t, 10, 0, 2, 0x1000, &handle) == SHM_OK);
    CHECK(shm_core_alloc(&t, 10, 0, 1, 0x3000, &handle) == SHM_OK);
    CHECK(shm_core_stat(&t)->peak == 2);
    CHECK(shm_core_stat(&t)->page_allocs == 3);
    CHECK(shm_core_live(&t) == 2);
    CHECK(shm_core_release_pid(&t, 10) == 2);
    CHECK(shm_core_live(&t) == 0);
    CHECK(shm_core_stat(&t)->unmaps == 2);
    CHECK(shm_core_stat(&t)->page_frees == 3);
    CHECK(shm_core_stat(&t)->allocs == 2);
    CHECK(shm_core_live(NULL) == 0);
}

int test_shm(void)
{
    t_alloc_basic();
    t_permission();
    t_refcount_free();
    t_release_pid();
    t_abort_and_limits();
    t_stats();
    return 0;
}
