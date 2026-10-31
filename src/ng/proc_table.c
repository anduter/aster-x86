/*
 * ng WP3-0: 进程表的纯状态机（见 proc.h）。
 *
 * 这里不 include 线程/页表/ELF/串口，也不分配内存：host 单测直接构造
 * struct proc_table 驱动，内核用同一份代码保证语义一致。
 *
 * 状态迁移：
 *
 *   FREE --proc_alloc--> READY --(内核胶水/调度)--> RUNNING
 *                          |                          |
 *                          |                  --block--> BLOCKED
 *                          |                          |
 *                          +<------- 唤醒 ------------+
 *                                                     |
 *                                --exit/kill--> ZOMBIE --proc_reap--> FREE
 *
 * 关键语义（都有 host 单测）：
 *   - pid 从 1 单调递增，16 位/32 位回绕时跳过 0；槽位复用不影响 pid；
 *   - 只有父进程能 wait 自己的孩子（否则 ECHILD）；wait 一个还活着的孩子时
 *     把 wait_tid 记下来，退出时 proc_mark_exit 把它交回去让胶水 thread_wake；
 *   - 僵尸只能回收一次；回收后 pid 不再有效（ESRCH）；
 *   - 父进程退出时孩子重挂到 init(1)，避免孤儿永远僵尸；
 *   - kill 只做状态迁移（ZOMBIE + exit_status=0x100|sig），真正的线程/AS/端口
 *     回收由 proc.c 做。
 */
#include "proc.h"

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void proc_memzero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static struct proc *proc_slot_of(struct proc_table *pt, u32 index)
{
    if (pt == NULL || index >= PROC_MAX) {
        return NULL;
    }
    return &pt->procs[index];
}

/* ------------------------------------------------------------------------- */
/* 初始化 / 分配 / 查找                                                       */
/* ------------------------------------------------------------------------- */

void proc_table_init(struct proc_table *pt)
{
    if (pt == NULL) {
        return;
    }
    proc_memzero(pt, sizeof(*pt));
    pt->next_pid = 1;
}

struct proc *proc_alloc(struct proc_table *pt, u32 ppid, u32 uid)
{
    u32 i;
    struct proc *p = NULL;
    u32 pid;

    if (pt == NULL) {
        return NULL;
    }
    for (i = 0; i < PROC_MAX; i++) {
        if (pt->procs[i].pid == 0) {
            p = &pt->procs[i];
            break;
        }
    }
    if (p == NULL) {
        pt->full_rejects++;
        return NULL;
    }
    /* pid 单调递增；回绕跳过 0，并且不与存活 pid 冲突时直接复用。 */
    pid = pt->next_pid;
    if (pid == 0) {
        pid = 1;
    }
    pt->next_pid = pid + 1;
    if (pt->next_pid == 0) {
        pt->next_pid = 1;
    }

    proc_memzero(p, sizeof(*p));
    p->pid = pid;
    p->ppid = ppid;
    p->uid = uid;
    p->state = PROC_STATE_READY;
    p->exit_kind = PROC_EXIT_NONE;
    p->exit_status = 0;
    handle_table_init(&p->handles);
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    pt->live++;
    pt->spawned++;
    return p;
}

struct proc *proc_find(struct proc_table *pt, u32 pid)
{
    u32 i;

    if (pt == NULL || pid == 0) {
        return NULL;
    }
    for (i = 0; i < PROC_MAX; i++) {
        if (pt->procs[i].pid == pid) {
            return &pt->procs[i];
        }
    }
    if (pt->bad_pids < 0xFFFFFFFFu) {
        pt->bad_pids++;
    }
    return NULL;
}

u32 proc_live(const struct proc_table *pt)
{
    return (pt == NULL) ? 0 : pt->live;
}

u32 proc_child_count(struct proc_table *pt, u32 ppid)
{
    u32 i;
    u32 n = 0;

    if (pt == NULL) {
        return 0;
    }
    for (i = 0; i < PROC_MAX; i++) {
        if (pt->procs[i].pid != 0 && pt->procs[i].ppid == ppid) {
            n++;
        }
    }
    return n;
}

struct proc *proc_child_at(struct proc_table *pt, u32 ppid, u32 index)
{
    u32 i;
    u32 n = 0;

    if (pt == NULL) {
        return NULL;
    }
    for (i = 0; i < PROC_MAX; i++) {
        if (pt->procs[i].pid == 0 || pt->procs[i].ppid != ppid) {
            continue;
        }
        if (n == index) {
            return &pt->procs[i];
        }
        n++;
    }
    return NULL;
}

int proc_is_child(struct proc_table *pt, u32 pid, u32 ppid)
{
    struct proc *p = proc_find(pt, pid);

    if (p == NULL) {
        return 0;
    }
    return p->ppid == ppid;
}

/* ------------------------------------------------------------------------- */
/* 退出 / 回收 / kill                                                         */
/* ------------------------------------------------------------------------- */

int proc_mark_exit(struct proc_table *pt, u32 pid, u32 kind, int status,
                   u32 *out_parent)
{
    struct proc *p;

    if (out_parent != NULL) {
        *out_parent = 0;
    }
    if (pt == NULL) {
        return PROC_EINVAL;
    }
    p = proc_find(pt, pid);
    if (p == NULL) {
        return PROC_ESRCH;
    }
    if (p->state == PROC_STATE_ZOMBIE) {
        return PROC_EBUSY; /* 已经退出过，等父进程回收 */
    }
    p->state = PROC_STATE_ZOMBIE;
    p->exit_kind = kind;
    p->exit_status = status;
    p->exit_code = (u64)status;
    p->exited = 1;
    pt->exited++;

    /* 父进程（如果还活着）是唯一需要唤醒的线程；由胶水查 parent->tid。 */
    if (p->ppid != 0 && proc_find(pt, p->ppid) != NULL) {
        if (out_parent != NULL) {
            *out_parent = p->ppid;
        }
    }
    return 1;
}

int proc_reap(struct proc_table *pt, u32 pid)
{
    struct proc *p;

    if (pt == NULL) {
        return PROC_EINVAL;
    }
    p = proc_find(pt, pid);
    if (p == NULL) {
        return PROC_ESRCH;
    }
    if (p->state != PROC_STATE_ZOMBIE) {
        return PROC_EBUSY;
    }
    p->pid = 0;
    p->state = PROC_STATE_FREE;
    if (pt->live > 0) {
        pt->live--;
    }
    pt->reaped++;
    return PROC_OK;
}

int proc_kill_mark(struct proc_table *pt, u32 pid, u32 sig, u32 *out_parent)
{
    struct proc *p;

    if (pt == NULL) {
        return PROC_EINVAL;
    }
    p = proc_find(pt, pid);
    if (p == NULL) {
        return PROC_ESRCH;
    }
    if (p->state == PROC_STATE_ZOMBIE) {
        return PROC_EBUSY;
    }
    p->killed_sig = sig & 0xFFu;
    pt->killed++;
    return proc_mark_exit(pt, pid, PROC_EXIT_KILLED,
                          PROC_STATUS_KILLED(sig & 0xFFu), out_parent);
}

u32 proc_reparent_children(struct proc_table *pt, u32 pid, u32 new_ppid)
{
    u32 i;
    u32 n = 0;

    if (pt == NULL) {
        return 0;
    }
    for (i = 0; i < PROC_MAX; i++) {
        struct proc *c = &pt->procs[i];

        if (c->pid == 0 || c->ppid != pid) {
            continue;
        }
        c->ppid = new_ppid;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* 诊断                                                                       */
/* ------------------------------------------------------------------------- */

struct proc *proc_slot_of_index(struct proc_table *pt, u32 index)
{
    return proc_slot_of(pt, index);
}
