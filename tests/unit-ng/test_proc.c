/*
 * ng WP3-0 host 单测：进程表纯状态机 + 每进程句柄命名空间。
 *
 * 被测代码是 src/ng/proc_table.c（不依赖线程/页表/ELF/串口）与
 * src/ng/handle.c。这里直接构造 struct proc_table 驱动完整的
 * 分配 -> 运行 -> 退出 -> 僵尸 -> 回收 状态机，以及 wait/kill 的语义。
 */
#include <string.h>

#include "handle.h"
#include "proc.h"
#include "test_util.h"

/* ------------------------------------------------------------------------- */
/* 辅助                                                                       */
/* ------------------------------------------------------------------------- */

static int g_obj_buf[8];

static void init(void)
{
    /* 每个子用例自己 proc_table_init；这里只做静态对象准备。 */
    memset(g_obj_buf, 0, sizeof(g_obj_buf));
}

/* ------------------------------------------------------------------------- */
/* 1. 分配：pid 单调、唯一、上限 32、槽位复用不改 pid                         */
/* ------------------------------------------------------------------------- */

static void t_alloc(void)
{
    struct proc_table pt;
    struct proc *p;
    u32 pids[PROC_MAX];
    u32 i;
    u32 j;

    init();
    proc_table_init(&pt);
    CHECK(proc_live(&pt) == 0);
    CHECK(pt.next_pid == 1);
    CHECK(proc_find(&pt, 1) == NULL);
    CHECK(pt.bad_pids == 1);
    CHECK(proc_find(&pt, 0) == NULL);

    p = proc_alloc(&pt, 0, 0);
    CHECK(p != NULL);
    CHECK(p->pid == 1);
    CHECK(p->ppid == 0);
    CHECK(p->state == PROC_STATE_READY);
    CHECK(p->exit_kind == PROC_EXIT_NONE);
    CHECK(p->uid == 0);
    CHECK(p->cwd[0] == '/' && p->cwd[1] == '\0');
    CHECK(handle_live(&p->handles) == 0);
    CHECK(proc_live(&pt) == 1);
    CHECK(pt.spawned == 1);

    /* 存活的 32 个 pid 两两不同且严格递增。 */
    pids[0] = p->pid;
    for (i = 1; i < PROC_MAX; i++) {
        p = proc_alloc(&pt, 1, 0);
        CHECK(p != NULL);
        CHECK(p->pid == i + 1);
        pids[i] = p->pid;
    }
    CHECK(proc_live(&pt) == PROC_MAX);
    CHECK(pt.spawned == PROC_MAX);
    for (i = 0; i < PROC_MAX; i++) {
        for (j = i + 1; j < PROC_MAX; j++) {
            CHECK(pids[i] != pids[j]);
        }
    }
    /* 表满：第 33 个被拒，full_rejects 记一次。 */
    CHECK(proc_alloc(&pt, 0, 0) == NULL);
    CHECK(pt.full_rejects == 1);
    CHECK(proc_live(&pt) == PROC_MAX);

    /* 回收一个槽位后可以再分配，但 pid 继续单调（不复用）。 */
    CHECK(proc_mark_exit(&pt, 5, PROC_EXIT_NORMAL, 0, NULL) == 1);
    CHECK(proc_reap(&pt, 5) == PROC_OK);
    CHECK(proc_live(&pt) == PROC_MAX - 1);
    p = proc_alloc(&pt, 1, 0);
    CHECK(p != NULL);
    CHECK(p->pid == PROC_MAX + 1);
    CHECK(p->pid != 5);
    CHECK(proc_find(&pt, 5) == NULL);
}

/* ------------------------------------------------------------------------- */
/* 2. 查询：find / child_count / child_at / is_child                           */
/* ------------------------------------------------------------------------- */

static void t_query(void)
{
    struct proc_table pt;
    struct proc *p;
    struct proc *c1;
    struct proc *c2;
    struct proc *c3;

    init();
    proc_table_init(&pt);
    p = proc_alloc(&pt, 0, 0); /* pid 1 */
    c1 = proc_alloc(&pt, p->pid, 0);
    c2 = proc_alloc(&pt, p->pid, 0);
    c3 = proc_alloc(&pt, c1->pid, 0);

    CHECK(proc_find(&pt, p->pid) == p);
    CHECK(proc_find(&pt, c1->pid) == c1);
    CHECK(proc_find(&pt, 9999) == NULL);
    CHECK(proc_child_count(&pt, p->pid) == 2);
    CHECK(proc_child_count(&pt, c1->pid) == 1);
    CHECK(proc_child_count(&pt, c3->pid) == 0);
    CHECK(proc_child_at(&pt, p->pid, 0) == c1);
    CHECK(proc_child_at(&pt, p->pid, 1) == c2);
    CHECK(proc_child_at(&pt, p->pid, 2) == NULL);
    CHECK(proc_child_at(&pt, c1->pid, 0) == c3);
    CHECK(proc_is_child(&pt, c1->pid, p->pid) == 1);
    CHECK(proc_is_child(&pt, c1->pid, c2->pid) == 0);
    CHECK(proc_is_child(&pt, 9999, p->pid) == 0);
    CHECK(proc_child_count(&pt, 0) == 1); /* 只有 init */

    /* 回收子进程后父子关系消失。 */
    CHECK(proc_mark_exit(&pt, c2->pid, PROC_EXIT_NORMAL, 0, NULL) == 1);
    CHECK(proc_reap(&pt, c2->pid) == PROC_OK);
    CHECK(proc_child_count(&pt, p->pid) == 1);
    CHECK(proc_child_at(&pt, p->pid, 1) == NULL);
}

/* ------------------------------------------------------------------------- */
/* 3. 退出 / 僵尸 / 回收                                                      */
/* ------------------------------------------------------------------------- */

static void t_exit(void)
{
    struct proc_table pt;
    struct proc *parent;
    struct proc *child;
    u32 wake = 0;
    int rc;

    init();
    proc_table_init(&pt);
    parent = proc_alloc(&pt, 0, 0);
    child = proc_alloc(&pt, parent->pid, 0);

    /* 还活着的进程不能回收。 */
    CHECK(proc_reap(&pt, child->pid) == PROC_EBUSY);
    CHECK(proc_mark_exit(&pt, child->pid, PROC_EXIT_NORMAL, 7, &wake) == 1);
    CHECK(wake == parent->pid); /* 父进程是应该被唤醒的那个 */
    CHECK(child->state == PROC_STATE_ZOMBIE);
    CHECK(child->exit_kind == PROC_EXIT_NORMAL);
    CHECK(child->exit_status == 7);
    CHECK(child->exit_code == 7);
    CHECK(child->exited == 1);
    CHECK(pt.exited == 1);

    /* 重复退出：EBUSY（等回收）。 */
    CHECK(proc_mark_exit(&pt, child->pid, PROC_EXIT_NORMAL, 0, NULL) ==
          PROC_EBUSY);
    /* 不存在的 pid：ESRCH。 */
    CHECK(proc_mark_exit(&pt, 9999, PROC_EXIT_NORMAL, 0, NULL) == PROC_ESRCH);
    CHECK(proc_reap(&pt, 9999) == PROC_ESRCH);
    CHECK(proc_reap(&pt, 0) == PROC_ESRCH);

    /* 回收：槽位释放、live--、pid 不再有效。 */
    CHECK(proc_reap(&pt, child->pid) == PROC_OK);
    CHECK(pt.reaped == 1);
    CHECK(pt.live == 1);
    CHECK(proc_find(&pt, child->pid) == NULL);
    CHECK(proc_reap(&pt, child->pid) == PROC_ESRCH);
    CHECK(proc_mark_exit(&pt, child->pid, PROC_EXIT_NORMAL, 0, NULL) ==
          PROC_ESRCH);

    /* 正常退出码只占低 8 位（状态编码）。 */
    {
        struct proc *q = proc_alloc(&pt, 1, 0);

        rc = proc_mark_exit(&pt, q->pid, PROC_EXIT_NORMAL, 300, NULL);
        CHECK(rc == 1);
        CHECK(q->exit_status == 300); /* 纯状态机保存原值，胶水层做截断 */
        CHECK(PROC_STATUS_EXIT(300) == 44);
    }
}

/* ------------------------------------------------------------------------- */
/* 4. wait：阻塞条件（僵尸）/ 唤醒目标 / 幂等                                 */
/* ------------------------------------------------------------------------- */

static void t_wait(void)
{
    struct proc_table pt;
    struct proc *p1;
    struct proc *p2;
    struct proc *c;
    u32 wake = 0;

    init();
    proc_table_init(&pt);
    p1 = proc_alloc(&pt, 0, 0);
    p2 = proc_alloc(&pt, 0, 0);
    c = proc_alloc(&pt, p1->pid, 0);

    /* wait 的"可以立刻返回"条件：子进程已经是僵尸。 */
    CHECK(c->state != PROC_STATE_ZOMBIE);
    CHECK(proc_is_child(&pt, c->pid, p1->pid) == 1);
    CHECK(proc_is_child(&pt, c->pid, p2->pid) == 0); /* 非父子 -> ECHILD */

    /* 胶水层 wait 前会记录 wait_tid；退出时父 pid 回给胶水去 thread_wake。 */
    c->wait_tid = 42;
    p1->state = PROC_STATE_BLOCKED;
    CHECK(proc_mark_exit(&pt, c->pid, PROC_EXIT_NORMAL, 0, &wake) == 1);
    CHECK(wake == p1->pid);
    CHECK(c->state == PROC_STATE_ZOMBIE);
    CHECK(c->wait_tid == 42);
    CHECK(c->wait_count == 0);

    /* 回收后父进程的子进程数减一，wait 计数由胶水层在返回前加。 */
    c->wait_count++;
    CHECK(proc_reap(&pt, c->pid) == PROC_OK);
    CHECK(proc_child_count(&pt, p1->pid) == 0);
    p1->state = PROC_STATE_RUNNING;
    pt.wait_calls = 3;
    CHECK(pt.wait_calls == 3);

    /* 没有父进程的进程退出：wake=0（没人可叫）。 */
    wake = 123;
    CHECK(proc_mark_exit(&pt, p2->pid, PROC_EXIT_NORMAL, 0, &wake) == 1);
    CHECK(wake == 0);
    CHECK(proc_reap(&pt, p2->pid) == PROC_OK);
}

/* ------------------------------------------------------------------------- */
/* 5. kill：状态编码 / 幂等 / 错误码                                          */
/* ------------------------------------------------------------------------- */

static void t_kill(void)
{
    struct proc_table pt;
    struct proc *p;
    struct proc *c;
    u32 wake = 0;

    init();
    proc_table_init(&pt);
    p = proc_alloc(&pt, 0, 0);
    c = proc_alloc(&pt, p->pid, 0);

    CHECK(PROC_STATUS_KILLED(9) == 0x109);
    CHECK(PROC_STATUS_KILLED(9) == 265);
    CHECK(PROC_STATUS_IS_KILLED(PROC_STATUS_KILLED(15)) == 1);
    CHECK(PROC_STATUS_IS_KILLED(PROC_STATUS_EXIT(3)) == 0);
    CHECK(PROC_STATUS_SIG(PROC_STATUS_KILLED(15)) == 15);
    CHECK(PROC_STATUS_SIG(PROC_STATUS_EXIT(3)) == 3);

    CHECK(proc_kill_mark(&pt, c->pid, 9, &wake) == 1);
    CHECK(wake == p->pid);
    CHECK(c->state == PROC_STATE_ZOMBIE);
    CHECK(c->exit_kind == PROC_EXIT_KILLED);
    CHECK(c->exit_status == PROC_STATUS_KILLED(9));
    CHECK(c->killed_sig == 9);
    CHECK(pt.killed == 1);

    /* 僵尸再次 kill：EBUSY（胶水层对 zombie 直接返回 0，表状态不再变）。 */
    CHECK(proc_kill_mark(&pt, c->pid, 15, NULL) == PROC_EBUSY);
    CHECK(c->exit_status == PROC_STATUS_KILLED(9));
    CHECK(proc_kill_mark(&pt, 9999, 9, NULL) == PROC_ESRCH);
    CHECK(proc_kill_mark(&pt, 0, 9, NULL) == PROC_ESRCH);

    /* 信号只保留低 8 位。 */
    {
        struct proc *q = proc_alloc(&pt, p->pid, 0);

        CHECK(proc_kill_mark(&pt, q->pid, 0x1FF, NULL) == 1);
        CHECK(q->killed_sig == 0xFF);
        CHECK(q->exit_status == PROC_STATUS_KILLED(0xFF));
        CHECK(proc_reap(&pt, q->pid) == PROC_OK);
    }
    CHECK(proc_reap(&pt, c->pid) == PROC_OK);
}

/* ------------------------------------------------------------------------- */
/* 6. 孤儿重挂                                                                */
/* ------------------------------------------------------------------------- */

static void t_orphan(void)
{
    struct proc_table pt;
    struct proc *a;
    struct proc *b;
    struct proc *c1;
    struct proc *c2;

    init();
    proc_table_init(&pt);
    a = proc_alloc(&pt, 0, 0);  /* 1 */
    b = proc_alloc(&pt, a->pid, 0); /* 2 */
    c1 = proc_alloc(&pt, b->pid, 0);
    c2 = proc_alloc(&pt, b->pid, 0);

    CHECK(proc_child_count(&pt, b->pid) == 2);
    CHECK(proc_reparent_children(&pt, b->pid, 1) == 2);
    CHECK(c1->ppid == 1);
    CHECK(c2->ppid == 1);
    CHECK(proc_child_count(&pt, b->pid) == 0);
    CHECK(proc_child_count(&pt, 1) == 3); /* a 自己 + 两个孙子 */
    CHECK(proc_reparent_children(&pt, b->pid, 1) == 0);
    CHECK(proc_reparent_children(&pt, 9999, 1) == 0);
}

/* ------------------------------------------------------------------------- */
/* 7. pid 回绕：跳过 0                                                        */
/* ------------------------------------------------------------------------- */

static void t_pid_wrap(void)
{
    struct proc_table pt;
    struct proc *p;

    init();
    proc_table_init(&pt);
    pt.next_pid = 0xFFFFFFFFu;
    p = proc_alloc(&pt, 0, 0);
    CHECK(p != NULL);
    CHECK(p->pid == 0xFFFFFFFFu);
    CHECK(pt.next_pid == 1); /* 跳过 0 */
    p = proc_alloc(&pt, 0, 0);
    CHECK(p->pid == 1);
    p = proc_alloc(&pt, 0, 0);
    CHECK(p->pid == 2);

    /* next_pid == 0 的脏状态也要被纠正成 1。 */
    proc_table_init(&pt);
    pt.next_pid = 0;
    p = proc_alloc(&pt, 0, 0);
    CHECK(p->pid == 1);
}

/* ------------------------------------------------------------------------- */
/* 8. 每进程句柄表：命名空间独立                                               */
/* ------------------------------------------------------------------------- */

static void t_handle_ns(void)
{
    struct proc_table pt;
    struct proc *p1;
    struct proc *p2;
    u32 h1 = 0;
    u32 h2 = 0;
    u32 h1b = 0;
    void *out = NULL;
    u32 type = 0;
    u32 i;

    init();
    proc_table_init(&pt);
    p1 = proc_alloc(&pt, 0, 0);
    p2 = proc_alloc(&pt, 0, 0);

    /* 两张表各自从 (gen=1, slot=0) 开始：句柄数值相同、对象不同。 */
    CHECK(handle_alloc(&p1->handles, HANDLE_TYPE_PORT, &g_obj_buf[0], &h1) ==
          HANDLE_OK);
    CHECK(handle_alloc(&p2->handles, HANDLE_TYPE_PORT, &g_obj_buf[1], &h2) ==
          HANDLE_OK);
    CHECK(h1 == h2);
    CHECK(h1 == handle_make(1, 0));
    CHECK(handle_live(&p1->handles) == 1);
    CHECK(handle_live(&p2->handles) == 1);
    CHECK(handle_lookup(&p1->handles, h1, HANDLE_TYPE_PORT, &out) == HANDLE_OK);
    CHECK(out == &g_obj_buf[0]);
    CHECK(handle_lookup(&p2->handles, h2, HANDLE_TYPE_PORT, &out) == HANDLE_OK);
    CHECK(out == &g_obj_buf[1]);

    /* 关 p1 的句柄：p1 的表失效、p2 不受影响。 */
    CHECK(handle_close(&p1->handles, h1, &type, &out) == HANDLE_OK);
    CHECK(type == HANDLE_TYPE_PORT);
    CHECK(out == &g_obj_buf[0]);
    CHECK(handle_live(&p1->handles) == 0);
    CHECK(handle_live(&p2->handles) == 1);
    CHECK(handle_lookup(&p1->handles, h1, HANDLE_TYPE_PORT, &out) ==
          HANDLE_EBADF);
    CHECK(handle_lookup(&p2->handles, h2, HANDLE_TYPE_PORT, &out) == HANDLE_OK);
    CHECK(out == &g_obj_buf[1]);

    /* 槽位复用后 p1 的 generation 递增，p2 仍是 1。 */
    CHECK(handle_alloc(&p1->handles, HANDLE_TYPE_PORT, &g_obj_buf[2], &h1b) ==
          HANDLE_OK);
    CHECK(h1b == handle_make(2, 0));
    CHECK(handle_gen_of(h2) == 1);
    CHECK(handle_lookup(&p2->handles, h1b, HANDLE_TYPE_PORT, &out) ==
          HANDLE_EBADF); /* 陈旧/别的进程的句柄 */

    /* 一张表满不影响另一张：p2 还能继续分配。 */
    for (i = 0; i < HANDLE_MAX - 1; i++) {
        u32 h = 0;

        CHECK(handle_alloc(&p1->handles, HANDLE_TYPE_FILE, &g_obj_buf[3], &h) ==
              HANDLE_OK);
    }
    CHECK(handle_live(&p1->handles) == HANDLE_MAX);
    CHECK(handle_alloc(&p1->handles, HANDLE_TYPE_FILE, &g_obj_buf[3], &h1) ==
          HANDLE_ENOSPC);
    CHECK(p1->handles.full_rejects == 1);
    CHECK(handle_alloc(&p2->handles, HANDLE_TYPE_FILE, &g_obj_buf[4], &h1) ==
          HANDLE_OK);
    CHECK(handle_live(&p2->handles) == 2);

    /* 两张表之间没有任何共享：p2 的 live 仍为 2，p1 满 64。 */
    CHECK(handle_live(&p1->handles) == HANDLE_MAX);
    CHECK(handle_live(&p2->handles) == 2);
}

/* ------------------------------------------------------------------------- */
/* 9. handle_find_obj（退出时把端口映射回自己的句柄）                         */
/* ------------------------------------------------------------------------- */

static void t_find_obj(void)
{
    struct handle_table t;
    u32 h = 0;
    u32 found = 0;

    init();
    handle_table_init(&t);
    CHECK(handle_find_obj(&t, HANDLE_TYPE_PORT, &g_obj_buf[0], &found) ==
          HANDLE_ENOENT);
    CHECK(handle_find_obj(NULL, HANDLE_TYPE_PORT, &g_obj_buf[0], &found) ==
          HANDLE_EINVAL);
    CHECK(handle_find_obj(&t, HANDLE_TYPE_PORT, NULL, &found) == HANDLE_EINVAL);

    CHECK(handle_alloc(&t, HANDLE_TYPE_PORT, &g_obj_buf[0], &h) == HANDLE_OK);
    CHECK(handle_find_obj(&t, HANDLE_TYPE_PORT, &g_obj_buf[0], &found) ==
          HANDLE_OK);
    CHECK(found == h);
    CHECK(handle_find_obj(&t, HANDLE_TYPE_NONE, &g_obj_buf[0], &found) ==
          HANDLE_OK); /* 任意类型 */
    CHECK(handle_find_obj(&t, HANDLE_TYPE_THREAD, &g_obj_buf[0], &found) ==
          HANDLE_ENOENT); /* 类型不匹配 */
    CHECK(handle_find_obj(&t, HANDLE_TYPE_PORT, &g_obj_buf[1], &found) ==
          HANDLE_ENOENT);
    CHECK(handle_close(&t, h, NULL, NULL) == HANDLE_OK);
    CHECK(handle_find_obj(&t, HANDLE_TYPE_PORT, &g_obj_buf[0], &found) ==
          HANDLE_ENOENT);
}

/* ------------------------------------------------------------------------- */
/* 10. 统计                                                                   */
/* ------------------------------------------------------------------------- */

static void t_stats(void)
{
    struct proc_table pt;
    struct proc *a;
    struct proc *b;
    u32 i;
    u32 wake = 0;

    init();
    proc_table_init(&pt);
    for (i = 2; i <= 6; i++) {
        CHECK(proc_alloc(&pt, 1, 0) != NULL);
    }
    a = proc_find(&pt, 2);
    b = proc_find(&pt, 3);
    CHECK(a != NULL && b != NULL);
    CHECK(pt.spawned == 5);
    CHECK(proc_mark_exit(&pt, a->pid, PROC_EXIT_NORMAL, 0, &wake) == 1);
    CHECK(proc_kill_mark(&pt, b->pid, 9, &wake) == 1);
    CHECK(pt.exited == 2);
    CHECK(pt.killed == 1);
    CHECK(proc_reap(&pt, a->pid) == PROC_OK);
    CHECK(proc_reap(&pt, b->pid) == PROC_OK);
    CHECK(pt.reaped == 2);
    CHECK(pt.live == 3);
    CHECK(pt.full_rejects == 0);
    /* bad_pids 来自 find 的失败查询。 */
    CHECK(proc_find(&pt, 4242) == NULL);
    CHECK(pt.bad_pids == 1);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int test_proc(void)
{
    t_alloc();
    t_query();
    t_exit();
    t_wait();
    t_kill();
    t_orphan();
    t_pid_wrap();
    t_handle_ns();
    t_find_obj();
    t_stats();
    return ut_failures;
}
