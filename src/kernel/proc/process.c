#include "proc.h"
#include "proc_internal.h"
#include "include/proc.h"
#include "include/userland.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/proc.h"
#include "include/mm.h"
#include "include/mm.h"
#include "include/proc.h"
#include "include/proc.h"
#include "include/fs.h"
#include "include/fs.h"
#include "include/proc.h"
#include "include/proc.h"
#include "include/mm.h"
#include "include/kernel.h"

/* 进程表；当前进程改为 per-CPU（this_cpu()->current）。 */
proc_t  proc_table[PROC_MAX];

static uint32_t next_pid = 1;
static char proc_roots[PROC_MAX][192];

/* 用户栈/堆/蹦床页 ASLR：每个进程在各自区域内选择随机地址。 */
static uint32_t aslr_state;

static uint32_t aslr_next(void)
{
    uint32_t x = aslr_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    aslr_state = x;
    return x;
}

/* 活动进程的 ASLR 槽位去重：避免回归断言被合法碰撞误伤。 */
static bool aslr_slot_used(proc_t *self, uint32_t slot, int kind)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        proc_t *q = &proc_table[i];
        uint32_t used;

        if (q == self || q->state == PROC_FREE) continue;

        if (kind == 0) {
            if (q->user_stack_top == 0U) continue;
            used = (USER_LAYOUT_STACK_TOP - q->user_stack_top) >> 12;
        } else if (kind == 1) {
            if (q->user_heap_base < USER_LAYOUT_HEAP_BASE) continue;
            used = (q->user_heap_base - USER_LAYOUT_HEAP_BASE) >> 12;
        } else {
            if (q->user_trampoline < USER_LAYOUT_TRAMPOLINE) continue;
            used = (q->user_trampoline - USER_LAYOUT_TRAMPOLINE) >> 12;
        }
        if (used == slot) return true;
    }
    return false;
}

static uint32_t aslr_pick_slot(proc_t *self, uint32_t count, int kind)
{
    uint32_t start = aslr_next() % count;

    for (uint32_t n = 0; n < count; n++) {
        uint32_t slot = (start + n) % count;
        if (!aslr_slot_used(self, slot, kind)) return slot;
    }
    return start;                   /* 槽位耗尽时允许碰撞 */
}

void proc_init(void)
{
    memset(proc_table, 0, sizeof(proc_table));
    memset(proc_roots, 0, sizeof(proc_roots));
    this_cpu()->current = 0;
    this_cpu()->bootstrap_esp = 0;
    next_pid = 1;

    uint32_t tsc_lo, tsc_hi;

    /* TSC 提供跨启动的时序熵；timer_ticks() 在启动早期可能为 0。 */
    __asm__ volatile ("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    aslr_state = timer_ticks() ^ tsc_lo ^ (tsc_hi * 0x9E3779B9U) ^
                 0xA5A5A5A5U ^ (next_pid * 0x9E3779B9U);
    if (aslr_state == 0U) aslr_state = 0x12345678U;
}

/* 为新线程准备初始栈：
 *   [esp] edi  [esp+4] esi  [esp+8] ebx  [esp+12] ebp  [esp+16] 返回地址
 * 这样 context_switch 第一次切进来时，pop 完四个寄存器后 ret 就进 trampoline。 */
static void *build_stack(void *stack_base)
{
    uintptr_t top = ((uintptr_t)stack_base + PROC_STACK_SIZE) & ~(uintptr_t)0xFU;
    uintptr_t sp = top;

    /* 栈向下增长；最底部放 canary，溢出时最先覆盖它。 */
    *(uint32_t *)stack_base = PROC_STACK_CANARY;

    sp -= 4; *(uint32_t *)sp = (uint32_t)(uintptr_t)thread_trampoline;
    sp -= 4; *(uint32_t *)sp = 0;   /* ebp */
    sp -= 4; *(uint32_t *)sp = 0;   /* ebx */
    sp -= 4; *(uint32_t *)sp = 0;   /* esi */
    sp -= 4; *(uint32_t *)sp = 0;   /* edi */

    return (void *)sp;
}

proc_t *proc_spawn(const char *program_name)
{
    const program_t *prog;
    proc_t *p = 0;

    prog = prog_lookup(program_name);

    if (prog == 0 || prog->entry == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_FREE) {
            p = &proc_table[i];
            break;
        }
    }
    if (p == 0) return 0;           /* 进程表满 */

    void *stack = malloc(PROC_STACK_SIZE);
    if (stack == 0) return 0;

    p->pid        = next_pid++;
    p->state      = PROC_READY;
    p->name       = prog->name;
    p->entry      = prog->entry;
    p->cpu_pin    = -1;             /* 默认允许在任意 CPU 上运行 */
    p->last_cpu   = 0xFFFFFFFFU;    /* 还没跑过 */
    p->stack_base = stack;
    p->stack_top  = (void *)((uintptr_t)stack + PROC_STACK_SIZE);
    p->esp        = build_stack(stack);
    p->page_dir   = vmm_new_dir();     /* 独立地址空间（内核映射共享） */
    if (p->page_dir == 0) {
        free(stack);
        sched_clear_slot(p);
        return 0;
    }
    /* 用户栈/堆/蹦床页 ASLR：只随机起始地址，边界与 guard 不变。 */
    {
        uint32_t slot = aslr_pick_slot(p, USER_LAYOUT_STACK_ASLR_SLOTS, 0);
        uint32_t hslot = aslr_pick_slot(p, USER_LAYOUT_HEAP_ASLR_SLOTS, 1);
        uint32_t tslot = aslr_pick_slot(p, USER_LAYOUT_TRAMPOLINE_ASLR_SLOTS, 2);

        p->user_stack_top = USER_LAYOUT_STACK_TOP - (slot << 12);
        p->user_stack_va  = p->user_stack_top - USER_LAYOUT_STACK_SIZE;
        p->user_heap_base = USER_LAYOUT_HEAP_BASE + hslot * 4096U;
        p->user_trampoline = USER_LAYOUT_TRAMPOLINE + tslot * 4096U;
    }
    p->timeslice  = PROC_TIMESLICE;
    p->exit_code  = 0;
    p->parent_pid = 0;
    p->user_brk = 0;
    p->uid = 0;                     /* 内核创建的第一个进程是 root */
    p->gid = 0;
    p->euid = 0;
    p->egid = 0;
    p->suid = 0;
    p->sgid = 0;
    p->ngroups = 0;
    p->umask = 022U;                /* 默认 umask：新建文件 0644，目录 0755 */
    p->login_required = false;
    p->is_login = false;
    strcpy(p->cwd, "system:/");     /* 默认工作目录：system 驱动器根 */
    memset(p->fds, 0, sizeof(p->fds));
    cap_init(&p->caps);
    p->mmap_next = 0;
    memset(p->mmaps, 0, sizeof(p->mmaps));
    p->fds[0].used = true; p->fds[0].kind = 1; p->fds[0].rights = FD_RIGHT_READ;  /* stdin  */
    p->fds[1].used = true; p->fds[1].kind = 1; p->fds[1].rights = FD_RIGHT_WRITE; /* stdout */
    p->fds[2].used = true; p->fds[2].kind = 1; p->fds[2].rights = FD_RIGHT_WRITE; /* stderr */

    /* 最小沙箱：userdemo 只能输出/退出，不能打开文件。 */
    p->sandboxed = false;
    p->syscall_mask = 0xFFFFFFFFFFFFFFFFULL;
    p->native_mask = 0xFFFFFFFFU;
    if (strcmp(prog->name, "userdemo") == 0) {
        p->sandboxed = true;
        p->syscall_mask = (1ULL << SYS_WRITE) | (1ULL << SYS_EXIT) |
                          (1ULL << SYS_GETPID) | (1ULL << SYS_YIELD) |
                          (1ULL << SYS_SLEEP) | (1ULL << SYS_FORK) |
                          (1ULL << SYS_WAIT);
        /* native: ABI_VERSION(0), EXIT(1), YIELD(2), DEBUG_WRITE(6),
         * PROCESS_CREATE(7), PROCESS_WAIT(8)。 */
        p->native_mask = (1U << 0) | (1U << 1) | (1U << 2) |
                         (1U << 6) | (1U << 7) | (1U << 8) |
                         (1U << 11) | (1U << 12) | (1U << 13) |
                         (1U << 14) | (1U << 15) | (1U << 16) |
                         (1U << 17) | (1U << 18) |
                         (1U << 19) | (1U << 20);
    }

    native_proc_init(p);                        /* Native ABI self handle */

    /* 亲和：带 PROG_F_PIN_AP 的探针程序固定到 AP，其余按负载放置 */
    if ((prog->flags & PROG_F_PIN_AP) != 0U &&
        smp_cpu_count > 1U && per_cpu[1].ready) {
        p->cpu_pin = 1;
    }
    /* 复杂 native/fork 回归固定 BSP；AP ring3 由 ring3probe/heartbeat 覆盖。 */
    if (strcmp(prog->name, "userdemo") == 0) {
        p->cpu_pin = 0;
    }

    /* 按当前负载选 CPU：不再按程序名写死，哪颗核闲就放哪颗。
     * 用户进程和内核线程走同一条路径，是否安全由各子系统的锁保证。 */
    sched_enqueue(p, sched_place(p));

    kprintf("proc: spawned pid %u (%s@%u)\n", p->pid, prog->name, prog->version);
    return p;
}

/* 新线程真正开始执行时从这里进入 C 世界 */
void thread_entry_c(void)
{
    proc_t *p = proc_current();

    /* 第一次运行的线程没有“从 context_switch 返回”的恢复点，在这里清掉
     * 本 CPU 的 switch_from 并补投迟到唤醒。 */
    sched_end_switch();

    if (p != 0 && p->entry != 0) {
        p->entry();
    }
    proc_exit(0);
}

void proc_switch_to(proc_t *next)
{
    proc_t *prev = proc_current();
    unsigned int flags;

    if (next == 0 || next == prev) return;
    this_cpu()->idle_active = false;

    /* 内核栈向下溢出会最先覆盖 stack_base 处的 canary。 */
    if (prev != 0 && prev->stack_base != 0 &&
        *(volatile uint32_t *)prev->stack_base != PROC_STACK_CANARY) {
        panic("kernel stack canary corrupted");
    }
    if (next->stack_base != 0 &&
        *(volatile uint32_t *)next->stack_base != PROC_STACK_CANARY) {
        panic("kernel stack canary corrupted");
    }

    /* next 必须来自 sched_next()（已置 RUNNING），且 esp 落在自己的内核栈里。
     * 以前出现过 esp/返回地址被踩后 ret 到 0x1b（用户 cs 值）的 oops；
     * 在切换点断言能把现场钉在“哪个 pid、哪段栈”。 */
    if (next->state != PROC_RUNNING) {
        panic("switch: next pid %u state %u not RUNNING",
              next->pid, (unsigned int)next->state);
    }
    if (next->stack_base != 0) {
        uintptr_t lo = (uintptr_t)next->stack_base;
        uintptr_t hi = lo + PROC_STACK_SIZE;
        uintptr_t sp = (uintptr_t)next->esp;

        if (sp < lo || sp >= hi) {
            panic("switch: pid %u (%s) esp %p outside kernel stack [%p,%p)",
                  next->pid, (next->name != 0) ? next->name : "?",
                  (void *)sp, (void *)lo, (void *)hi);
        }
    }

    /* 保存中断状态并在切换期间关中断，避免切到一半被时钟打断 */
    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
    __asm__ volatile ("cli");

    if (prev != 0 && prev->state == PROC_RUNNING) {
        prev->state = PROC_READY;
    }
    next->state = PROC_RUNNING;
    sched_begin_switch(prev);     /* 直到恢复点前 prev 的栈仍在用 */
    this_cpu()->current = next;

    /* ring3 进 ring0 时 CPU 从 TSS.esp0 取内核栈，必须跟当前线程一致 */
    if (next->stack_top != 0) {
        tss_set_esp0((uint32_t)(uintptr_t)next->stack_top);
    }

    /* 切换地址空间：每个进程有自己的页目录，内核映射是共享的 */
    if (next->page_dir != 0) {
        vmm_switch_dir((pde_t *)next->page_dir);
    }

    context_switch(prev != 0 ? &prev->esp : &this_cpu()->bootstrap_esp, next->esp);

    /* 这里已经在本进程（next）的栈上：prev 的栈不再被使用，可以补投
     * 之前因为“还在 CPU 上”而暂存的唤醒。 */
    sched_end_switch();

    /* 恢复到本进程时，恢复它自己的中断状态 */
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

void proc_yield(void)
{
    proc_t *next = sched_next(proc_current());

    if (next != 0 && next != proc_current()) {
        proc_switch_to(next);
    }
}

/* fork 出来的子进程从这里开始：直接 iret 回用户态，eax=0 */
static void fork_child_entry(void)
{
    proc_t *p = proc_current();

    kprintf("proc: child pid %u returns from fork at %p\n",
            p->pid, (void *)(uintptr_t)p->user_eip);

    p->is_user = true;
    user_enter_regs(p->user_eip, p->user_esp, 0, p->fork_regs);
}

/* 复制一个进程：用户页、fd 表、cwd 全部复制一份 */
proc_t *proc_fork(struct interrupt_frame *f)
{
    proc_t *parent = proc_current();
    proc_t *child = 0;
    void *stack;

    if (parent == 0 || f == 0) return 0;
    if ((f->cs & 3U) != 3U) return 0;       /* 只支持用户态进程 fork */

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_FREE) {
            child = &proc_table[i];
            break;
        }
    }
    if (child == 0) return 0;

    stack = malloc(PROC_STACK_SIZE);
    if (stack == 0) return 0;

    child->pid        = next_pid++;
    child->state      = PROC_READY;
    child->name       = parent->name;
    child->entry      = fork_child_entry;
    child->stack_base = stack;
    child->stack_top  = (void *)((uintptr_t)stack + PROC_STACK_SIZE);
    child->esp        = build_stack(stack);
    child->page_dir   = vmm_new_dir();
    child->timeslice  = PROC_TIMESLICE;
    child->exit_code  = 0;
    child->cpu_pin    = parent->cpu_pin;   /* 继承亲和（默认 -1 = 任意 CPU） */
    child->parent_pid = parent->pid;
    child->user_eip   = f->eip;
    child->user_esp   = f->useresp;
    child->fork_regs[0] = f->ebx;
    child->fork_regs[1] = f->ebp;
    child->fork_regs[2] = f->esi;
    child->fork_regs[3] = f->edi;
    child->is_fork_child = true;
    child->user_stack_top = parent->user_stack_top;
    child->user_stack_va  = parent->user_stack_va;
    child->user_heap_base = parent->user_heap_base;
    child->user_trampoline = parent->user_trampoline;

    strcpy(child->cwd, parent->cwd);
    proc_root_copy(parent, child);
    child->uid = parent->uid;               /* fork 继承身份 */
    child->gid = parent->gid;
    /* 凭据整组继承（POSIX）：euid/egid/suid/sgid + 附加组 + umask */
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->suid = parent->suid;
    child->sgid = parent->sgid;
    child->ngroups = parent->ngroups;
    memcpy(child->groups, parent->groups, sizeof(child->groups));
    child->umask = parent->umask;
    child->login_required = false;          /* 子进程不再重复 login */
    child->is_login = parent->is_login;
    memcpy(child->fds, parent->fds, sizeof(child->fds));
    child->mmap_next = parent->mmap_next;
    memcpy(child->mmaps, parent->mmaps, sizeof(child->mmaps));
    cap_fork(&child->caps, &parent->caps);
    memcpy(child->sig_handlers, parent->sig_handlers, sizeof(child->sig_handlers));
    native_proc_copy(parent, child);

    /* fork 复制了 fd 表：文件句柄和管道写端都要加引用 */
    for (int i = 0; i < PROC_FD_MAX; i++) {
        if (!child->fds[i].used) continue;
        if (child->fds[i].kind == FD_KIND_FILE) {
            if (fs_ref_ex(child->fds[i].fs_be, (int)child->fds[i].handle) != E_OK) {
                /* 后端不支持共享（FAT16）：子进程不继承这个 fd */
                memset(&child->fds[i], 0, sizeof(child->fds[i]));
            }
        }
    }
    pipe_fork_inherit(child);

    if (child->page_dir == 0 ||
        vmm_clone_user_pages((pde_t *)child->page_dir) != 0) {
        /* 失败回滚：上面已经复制了 fd/管道写端/capability/native 句柄的引用，
         * 必须逐个释放，不能只 memset——否则文件句柄和管道写端会泄漏
         * （写端永远不减到 0，读端就可能永远等不到 EOF）。
         * 注意读端在 fork 时没有加引用，这里不能 unref，否则会把父进程
         * 还在用的空管道提前释放。 */
        for (int i = 0; i < PROC_FD_MAX; i++) {
            if (!child->fds[i].used) continue;
            if (child->fds[i].kind == FD_KIND_FILE) {
                fs_close_ex(child->fds[i].fs_be, (int)child->fds[i].handle);
            } else if (child->fds[i].kind == FD_KIND_PIPE_W) {
                pipe_unref_write((int)child->fds[i].handle);
            }
        }
        cap_release_all(&child->caps);
        native_proc_free(child);

        if (child->page_dir != 0) vmm_destroy_dir((pde_t *)child->page_dir);
        free(stack);
        sched_clear_slot(child);
        return 0;
    }

    kprintf("proc: forked pid %u from %u (eip=%p)\n",
            child->pid, parent->pid, (void *)(uintptr_t)f->eip);
    sched_enqueue(child, sched_place(child));
    return child;
}

static void proc_reap(proc_t *p)
{
    void *stack = p->stack_base;
    void *dir = p->page_dir;

    sched_dequeue(p);
    if (stack != 0) free(stack);
    if (dir != 0) vmm_destroy_dir((pde_t *)dir);
    native_proc_free(p);
    /* 清零放进 sched_lock：避免另一个 CPU 的 sched_next 看到半清零的 proc_t
     * （pid=0/name=NULL 但 state 还是旧值）。 */
    sched_clear_slot(p);
}

int proc_wait_options(int pid, int *status, int options)
{
    proc_t *me = proc_current();

    if (me == 0) return -1;

    for (;;) {
        proc_t *child = 0;

        for (uint32_t i = 0; i < PROC_MAX; i++) {
            proc_t *p = &proc_table[i];
            if (p->state == PROC_FREE || p->parent_pid != me->pid) continue;
            if (pid > 0 && p->pid != (uint32_t)pid) continue;
            child = p;
            break;
        }

        /* 没有匹配的子进程：阻塞版沿用 -1；WNOHANG 按约定立即返回 0 */
        if (child == 0) {
            return ((options & PROC_WAIT_WNOHANG) != 0U) ? 0 : -1;
        }

        if (child->state == PROC_ZOMBIE) {
            int code = child->exit_code;
            uint32_t cpid = child->pid;

            proc_reap(child);
            if (status != 0) *status = code;
            return (int)cpid;
        }

        /* 有子进程但都活着：WNOHANG 不阻塞，直接告诉调用方“还没好” */
        if ((options & PROC_WAIT_WNOHANG) != 0U) return 0;

        proc_block(PROC_WAIT_CHILD);
    }
}

int proc_wait(int pid, int *status)
{
    return proc_wait_options(pid, status, 0);
}

/* 阻塞：把当前线程标记为 BLOCKED 并切走，被唤醒后从这里返回。
 * 关中断完成“改状态 + 切走”，避免在两步之间被时钟中断看到不一致的状态。 */
void proc_block(unsigned int chan)
{
    proc_t *p = proc_current();
    proc_t *next;
    unsigned int flags;

    if (p == 0) return;

    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
    __asm__ volatile ("cli");

    p->wait_chan = chan;
    p->state = PROC_BLOCKED;

    if (!p->slept) {
        p->slept = true;
        kprintf("proc: pid %u (%s) went to sleep on %p\n",
                p->pid, p->name, (void *)(uintptr_t)chan);
    }

    next = sched_next(p);
    if (next != 0) {
        proc_switch_to(next);       /* 被唤醒后回到这里 */
    } else {
        /* 没有别的可运行线程：睡到真正被唤醒为止。
         * 不能只执行一次 hlt 就返回，否则 timer/NMI 等任意中断都会让
         * 阻塞进程“假醒”，在 accept/recv 里反复空转。
         * wake_pending 覆盖“还在本 CPU 上就被 sched_wake 叫醒”的窗口：
         * 那时 sched_wake 不会入队，只置这个标记。 */
        while (p->state == PROC_BLOCKED) {
            if (p->wake_pending) {
                p->wake_pending = false;
                break;
            }
            __asm__ volatile ("sti; hlt");
        }
    }

    p->state = PROC_RUNNING;
    p->wait_chan = 0;

    __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

void proc_wakeup(unsigned int chan)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        proc_t *p = &proc_table[i];
        if (p->state == PROC_BLOCKED && p->wait_chan == chan) {
            sched_wake(p);
        }
    }
}

/* ---------------- 睡眠 ---------------- */

void proc_sleep_ticks(uint32_t ticks)
{
    proc_t *p = proc_current();

    if (p == 0) return;
    if (ticks == 0U) { proc_yield(); return; }

    p->wake_at = timer_ticks() + ticks;
    proc_block(PROC_WAIT_SLEEP);
    p->wake_at = 0;
}

void proc_wake_sleepers(uint32_t now)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        proc_t *p = &proc_table[i];
        if (p->state == PROC_BLOCKED && p->wait_chan == PROC_WAIT_SLEEP) {
            if ((int32_t)(now - p->wake_at) >= 0) {
                sched_wake(p);
            }
        }
    }
}

/* ---------------- 信号 ---------------- */

/* 用户态信号蹦床：handler 返回后跳到这里，执行 int 0x81 (SYS_SIGRETURN)
 * 地址来自 user_layout.h；已移出栈增长区。 */

void proc_send_signal(proc_t *p, int sig)
{
    if (p == 0) return;
    if (p->state == PROC_FREE || p->state == PROC_ZOMBIE) return;
    if (!p->is_user) return;            /* 内核线程不接信号 */
    if (sig <= 0 || sig >= NSIG_MAX) return;

    p->pending_sig = (uint32_t)sig;

    /* 被阻塞的进程要醒过来才有机会处理信号 */
    if (p->state == PROC_BLOCKED) {
        sched_wake(p);
    }
}

int proc_signal_set(int sig, uint32_t handler)
{
    proc_t *p = proc_current();
    uint32_t old;

    if (p == 0 || sig <= 0 || sig >= NSIG_MAX) return -1;

    old = p->sig_handlers[sig];
    p->sig_handlers[sig] = handler;
    return (int)old;
}

/* 确保 [start, end) 覆盖的每个用户页对当前进程可写（COW 先私有化）。
 * 失败返回 0：调用方必须放弃写用户内存，不能让它变成内核态 #PF。 */
static int user_range_writable(uint32_t start, uint32_t end)
{
    if (end <= start) return 0;

    for (uint32_t va = start & ~0xFFFU; va < end; va += PAGE_SIZE) {
        if (vmm_make_writable(va) != 0) return 0;
    }
    return 1;
}

/* 在用户栈上压一帧：信号帧放在高地址，handler 的新栈在其下方，
 * 这样 handler 的栈增长不会踩到保存的现场。
 *
 * 注意：这是内核替用户写栈。fork 之后父进程的栈页是 COW 只读页，
 * 直接 memcpy 会以内核态写触发 #PF(err=3) → oops（曾把 shell 杀掉）。
 * 所以写帧前先把覆盖到的栈页私有化成可写；失败就不写、返回 0，
 * 由 proc_check_signals() 丢弃这次投递，绝不 oops。 */
static int deliver_signal(struct interrupt_frame *f, int sig, uint32_t handler)
{
    proc_t *p = proc_current();
    uint32_t frame_addr = (f->useresp - (uint32_t)sizeof(*f)) & ~3U;
    uint32_t sp;

    if (p == 0) return 0;
    /* 返回地址/sig 参数还要在 frame 下面 8 字节，guard 页不参与 */
    if (frame_addr < USER_LAYOUT_STACK_GUARD_END + 8U) return 0;
    if (frame_addr + (uint32_t)sizeof(*f) > p->user_stack_top) return 0;

    sp = (frame_addr - 8U) & ~3U;

    if (!user_range_writable(sp, frame_addr + (uint32_t)sizeof(*f))) return 0;

    user_access_begin();
    memcpy((void *)(uintptr_t)frame_addr, f, sizeof(*f));

    *(uint32_t *)(uintptr_t)sp = (p->user_trampoline != 0U)
                                     ? p->user_trampoline
                                     : USER_LAYOUT_TRAMPOLINE; /* handler 的返回地址 */
    *(uint32_t *)(uintptr_t)(sp + 4U) = (uint32_t)sig; /* 参数 */
    user_access_end();

    p->sig_frame = frame_addr;                          /* 供 sigreturn 恢复 */

    f->eip = handler;
    f->useresp = sp;
    f->eax = 0;
    return 1;
}

void proc_check_signals(struct interrupt_frame *f)
{
    proc_t *p = proc_current();
    uint32_t sig;
    uint32_t handler;

    if (p == 0 || p->pending_sig == 0U) return;

    sig = p->pending_sig;
    handler = (sig < NSIG_MAX) ? p->sig_handlers[sig] : 0U;

    if (handler == SIG_IGN) {
        p->pending_sig = 0U;            /* 忽略 */
        return;
    }

    /* SIGCHLD 的默认动作是“忽略”，不能走下面通用的“默认终止”：
     * 否则子进程正常退出会把没装 handler 的父进程（shell）杀掉。 */
    if (sig == SIGCHLD && handler == SIG_DFL) {
        p->pending_sig = 0U;
        return;
    }

    if (handler > SIG_IGN) {
        if (f != 0 && (f->cs & 3U) == 3U &&
            deliver_signal(f, (int)sig, handler)) {
            p->pending_sig = 0U;        /* 已投递：跳到用户 handler */
            return;
        }

        /* 自定义 handler 无法投递（用户栈不可写/OOM/非用户上下文）：
         * 丢弃这次信号并继续跑，绝不能落到下面的“默认终止”——投递失败
         * 不能当成致命错误（SIGCHLD 曾因此把 shell 杀掉）。 */
        p->pending_sig = 0U;
        kprintf("proc: pid %u signal %u dropped (handler not deliverable)\n",
                p->pid, sig);
        return;
    }

    p->pending_sig = 0U;
    kprintf("proc: pid %u got signal %u (default: terminate)\n", p->pid, sig);
    proc_exit((int)(128U + sig));
}

/* SYS_SIGRETURN：从用户栈上的信号帧恢复现场 */
int proc_sigreturn(struct interrupt_frame *f)
{
    proc_t *p = proc_current();

    if (p == 0 || p->sig_frame == 0U) return -1;
    if (p->sig_frame < USER_LAYOUT_STACK_GUARD_END) return -1;
    if (p->sig_frame + (uint32_t)sizeof(*f) > p->user_stack_top) return -1;

    user_access_begin();
    memcpy(f, (const void *)(uintptr_t)p->sig_frame, sizeof(*f));
    user_access_end();
    p->sig_frame = 0U;
    return 0;
}

void proc_console_int(void)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        proc_t *p = &proc_table[i];
        /* 简化：shell（pid 1）的子进程算前台任务 */
        if (p->state != PROC_FREE && p->state != PROC_ZOMBIE &&
            p->parent_pid == 1U && p->is_user) {
            proc_send_signal(p, SIGINT);
        }
    }
}

/* ==================== 退出线程的专用栈 ====================
 * 退出线程必须先离开自己的内核栈、并把 CR3 切到下一个进程，之后才能发布
 * ZOMBIE。否则在 SMP 上，父进程可能在另一颗 CPU 上立刻回收它的内核栈和
 * 地址空间，而它还在那块栈上跑最后几步——表现为偶发跳进 0x0/0x18 附近取指
 * （#PF err=0x11，cr2==eip，shell 被杀）。
 *
 * 每 CPU 一块静态退出栈：它属于内核 BSS，所有地址空间共享映射，不会被进程
 * 回收路径释放。退出线程切过来之后再发布 ZOMBIE，父进程无论在哪颗 CPU 上
 * 回收都不会踩到正在用的现场。 */
#define PROC_EXIT_STACK_SIZE 8192U

static uint8_t  proc_exit_stack[SMP_MAX_CPUS][PROC_EXIT_STACK_SIZE]
                    __attribute__((aligned(16)));
static uint32_t proc_exit_old_esp[SMP_MAX_CPUS];
static proc_t  *proc_exit_self[SMP_MAX_CPUS];
static proc_t  *proc_exit_next[SMP_MAX_CPUS];
static int      proc_exit_code[SMP_MAX_CPUS];

/* switch.asm：切到本 CPU 的专用退出栈后调用 proc_exit_finish，不返回 */
void proc_exit_on_stack(void *stack_top) __attribute__((noreturn));

/* 已经在退出栈上（IF=0）：先把 CPU 交给 next，再发布 ZOMBIE 让父进程回收 */
void proc_exit_finish(void)
{
    uint32_t cpu = smp_cpu_id();
    proc_t *p = proc_exit_self[cpu];
    proc_t *next = proc_exit_next[cpu];
    uint32_t ppid = (p != 0) ? p->parent_pid : 0U;

    if (next != 0) {
        this_cpu()->idle_active = false;
        if (next->stack_top != 0) {
            tss_set_esp0((uint32_t)(uintptr_t)next->stack_top);
        }
        this_cpu()->current = next;
        if (next->page_dir != 0) {
            vmm_switch_dir((pde_t *)next->page_dir);
        }
    } else {
        /* 没有下一个进程：必须先离开 p 的地址空间再发布 ZOMBIE。
         * 否则父进程在另一颗 CPU 上 proc_reap() → vmm_destroy_dir() 会释放
         * 本 CPU 仍在使用的 PDPT/页表；PMM 毒化后这些页变成 0xAA...
         * 下一次取指就是 eip/cr2 在 0x0/0x18/0x1b 附近的内核 #PF。
         * 切到内核自己的地址空间（只含内核映射）后再 idle。
         * 先切 CR3 再置 idle_active，保证“idle CPU 一定在 kernel dir”这个
         * 不变量；sched_tick 的 idle 路径会断言它。 */
        if (vmm_kernel_dir() != 0) {
            vmm_switch_dir(vmm_kernel_dir());
        }
        this_cpu()->current = 0;
        this_cpu()->idle_active = true;
    }

    /* 本线程的内核栈和页目录已经不再被本 CPU 使用，此刻发布 ZOMBIE 才安全：
     * 父进程收到唤醒后可以立刻 free 栈、vmm_destroy_dir，不会踩到正在用的现场。
     * 注意顺序：先摘 runqueue、写好退出码，再置 ZOMBIE，最后才唤醒。 */
    sched_dequeue(p);
    p->exit_code = proc_exit_code[cpu];
    p->state = PROC_ZOMBIE;
    proc_wakeup(PROC_WAIT_CHILD);

    /* 通知父进程子进程已退出：SIGCHLD 默认动作是“忽略”（见
     * proc_check_signals 的 SIG_DFL 特例），所以不装 handler 的父进程不会
     * 被杀；装了 handler 的会收到，SIG_IGN 的由 proc_send_signal 丢弃。
     * ppid 在发布 ZOMBIE 之前就取好（发布后父进程可能立刻 reap 并清空 p）。 */
    if (ppid != 0U) {
        proc_t *parent = proc_find(ppid);

        if (parent != 0 && parent != p) {
            proc_send_signal(parent, SIGCHLD);
        }
    }

    if (next == 0) {
        /* 本 CPU 没有可运行线程：在退出栈上进入 idle。
         * 必须开中断睡眠——timer/IPI 进来后 sched_tick() 会从 runqueue 取活。 */
        if (smp_cpu_id() == 0U) {
            kprintln("sched: system idle");
        }
        for (;;) {
            __asm__ volatile ("sti; hlt");
        }
    }

    /* 本线程不会再被调度：把当前（退出栈上的）上下文扔掉即可 */
    context_switch((void **)&proc_exit_old_esp[cpu], next->esp);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void proc_exit(int code)
{
    proc_t *p = proc_current();
    uint32_t cpu;

    if (p == 0) {
        /* 没有当前线程（启动早期/异常路径）：按 sched_start 的方式让出 CPU */
        for (;;) {
            proc_t *next = sched_next(0);

            if (next == 0) {
                for (;;) {
                    __asm__ volatile ("sti; hlt");
                }
            }
            proc_switch_to(next);
        }
    }

    /* 收尾阶段仍在自己的内核栈上：此时 p 还不是 ZOMBIE，父进程不会来回收。 */
    /* 释放 fd：文件句柄减引用，管道写端减引用（读端看到 EOF） */
    for (int i = 0; i < PROC_FD_MAX; i++) {
        if (!p->fds[i].used) continue;
        if (p->fds[i].kind == FD_KIND_FILE) {
            fs_close_ex(p->fds[i].fs_be, (int)p->fds[i].handle);
        } else if (p->fds[i].kind == FD_KIND_PIPE_W) {
            pipe_unref_write((int)p->fds[i].handle);
        } else if (p->fds[i].kind == FD_KIND_PIPE_R) {
            pipe_unref_read((int)p->fds[i].handle);
        }
        memset(&p->fds[i], 0, sizeof(p->fds[i]));
    }

    kprintf("proc: pid %u (%s) exited with %d\n", p->pid, p->name, code);
    cap_release_all(&p->caps);

    /* 从这里开始不再使用 p 的内核栈：关中断、记下切换目标，
     * 切到本 CPU 的专用退出栈完成“切 CR3 + 发布 ZOMBIE + 切到 next”。 */
    cpu = smp_cpu_id();
    __asm__ volatile ("cli");
    proc_exit_self[cpu] = p;
    proc_exit_next[cpu] = sched_next(p);
    proc_exit_code[cpu] = code;
    proc_exit_on_stack(&proc_exit_stack[cpu][PROC_EXIT_STACK_SIZE]);

    for (;;) {
        __asm__ volatile ("cli; hlt");      /* proc_exit_on_stack 不返回 */
    }
}

/* VFS namespace 侧表：按进程槽位保存 root，避免动 proc_t 布局。 */
static int proc_slot(proc_t *p)
{
    if (p == 0 || p < proc_table || p >= proc_table + PROC_MAX) return -1;
    return (int)(p - proc_table);
}

const char *proc_root_of(proc_t *p)
{
    int i = proc_slot(p);

    return (i >= 0) ? proc_roots[i] : "";
}

int proc_root_set(proc_t *p, const char *root)
{
    int i = proc_slot(p);

    if (i < 0 || root == 0) return -1;
    strncpy(proc_roots[i], root, sizeof(proc_roots[i]) - 1U);
    proc_roots[i][sizeof(proc_roots[i]) - 1U] = '\0';
    return 0;
}

void proc_root_copy(proc_t *parent, proc_t *child)
{
    int pi = proc_slot(parent);
    int ci = proc_slot(child);

    if (pi < 0 || ci < 0) return;
    memcpy(proc_roots[ci], proc_roots[pi], sizeof(proc_roots[ci]));
}

proc_t *proc_current(void)
{
    return (proc_t *)this_cpu()->current;
}

/* 当前进程的父 pid：内核线程/启动早期没有父进程，返回 0。
 * fork 时已写入 child->parent_pid，这里只做读取。 */
int proc_getppid(void)
{
    proc_t *p = proc_current();

    return (p != 0) ? (int)p->parent_pid : 0;
}

uint32_t proc_count(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].state != PROC_FREE) n++;
    }
    return n;
}

proc_t *proc_find(uint32_t pid)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        proc_t *p = &proc_table[i];
        if (p->state != PROC_FREE && p->pid == pid) return p;
    }
    return 0;
}

const proc_t *proc_at(uint32_t index)
{
    return (index < PROC_MAX) ? &proc_table[index] : 0;
}

/* ==================== 凭据（P1 身份，ABI v1.3） ====================
 * 所有权限判定都从这里取"生效身份"：p == 0（内核线程/启动早期）按 root。
 * root 的 CAP_DAC_OVERRIDE 语义在 fs 层实现（euid == 0 直接放行）。 */

uint32_t proc_euid_of(const proc_t *p)
{
    return (p != 0) ? p->euid : 0U;
}

uint32_t proc_egid_of(const proc_t *p)
{
    return (p != 0) ? p->egid : 0U;
}

bool proc_is_root(const proc_t *p)
{
    return (p == 0) || (p->euid == 0U);
}

bool proc_in_group(const proc_t *p, uint32_t gid)
{
    if (p == 0) return true;               /* 内核线程：视为 root 组 */

    if (p->egid == gid) return true;
    for (uint32_t i = 0; i < (uint32_t)p->ngroups && i < ABI_NGROUPS_MAX; i++) {
        if (p->groups[i] == gid) return true;
    }
    return false;
}

void proc_fill_cred(const proc_t *p, struct fs_cred *out)
{
    uint32_t n = 0;

    if (out == 0) return;

    out->euid = (p != 0) ? p->euid : 0U;
    out->egid = (p != 0) ? p->egid : 0U;
    if (p != 0) {
        for (uint32_t i = 0; i < (uint32_t)p->ngroups && i < ABI_NGROUPS_MAX; i++) {
            out->groups[n++] = p->groups[i];
        }
    }
    out->ngroups = n;
}

void proc_set_cred(proc_t *p, uint32_t uid, uint32_t gid)
{
    if (p == 0) return;

    p->uid = uid;
    p->euid = uid;
    p->suid = uid;
    p->gid = gid;
    p->egid = gid;
    p->sgid = gid;
    p->ngroups = 0;
    memset(p->groups, 0, sizeof(p->groups));
}

void proc_set_groups(proc_t *p, const uint32_t *gids, uint32_t n)
{
    if (p == 0) return;

    if (n > ABI_NGROUPS_MAX) n = ABI_NGROUPS_MAX;
    memset(p->groups, 0, sizeof(p->groups));
    for (uint32_t i = 0; i < n; i++) p->groups[i] = gids[i];
    p->ngroups = (uint8_t)n;
}

