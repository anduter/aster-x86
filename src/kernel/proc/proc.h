#ifndef PROC_PROC_H
#define PROC_PROC_H

#include "../types.h"
#include "include/abi.h"      /* SIG_DFL/SIG_IGN/SIGCHLD：与用户态 ABI 单一来源 */
#include "include/security.h"

/* 进程表上限与线程栈大小 */
#define PROC_MAX          24
#define PROC_STACK_SIZE   65536U        /* exec + RSA + 中断嵌套需要更大的内核栈 */
#define PROC_STACK_CANARY 0x5354414BU   /* 'STAK'，检测内核栈向下溢出 */
#define PROC_TIMESLICE    5U        /* 每个时间片 5 个 tick = 50ms */

/* 调度优先级：数值越小优先级越高，0 = 最高，7 = 最低，默认 4。
 * 默认值 4 时所有进程同优先级，调度行为与旧的纯轮转等价。 */
#define PROC_PRIO_LEVELS   8
#define PROC_PRIO_DEFAULT  4

/* aging：READY 但一直没被调度的进程每等待这么多 tick 提升一级，
 * 防止低优先级线程被高优先级线程长期饿死（100Hz 下 20 tick = 200ms）。 */
#define SCHED_AGING_TICKS  20U

typedef enum {
    PROC_FREE = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,       /* 睡眠在某个等待通道上 */
    PROC_ZOMBIE
} proc_state_t;

/* 等待通道（任意 32 位标识；设备/事件各用一个） */
#define PROC_WAIT_KBD   0x4B424431U   /* "KBD1" */
#define PROC_WAIT_CHILD 0x43484C44U   /* "CHLD" */
#define PROC_WAIT_SLEEP 0x534C4550U   /* "SLEP" */
#define PROC_WAIT_PIPE  0x50495045U   /* "PIPE" */

/* 信号：0=SIG_DFL（默认终止），1=SIG_IGN（忽略），>1=用户处理函数地址。
 * SIG_DFL/SIG_IGN/SIGCHLD 由 include/abi.h 统一定义（内核与用户态同一来源）。 */
#define SIGINT   2
#define SIGKILL  9
#define SIGTERM  15
#define NSIG_MAX 32

/* 每进程文件描述符表：0=stdin, 1=stdout, 2=stderr */
#define PROC_FD_MAX 16

/* fd 类型 */
#define FD_KIND_FILE  0   /* handle = fs 句柄 */
#define FD_KIND_CONSOLE 1 /* 控制台（stdin/stdout/stderr） */
#define FD_KIND_PIPE_R  2 /* 管道读端，handle = 管道下标 */
#define FD_KIND_PIPE_W  3 /* 管道写端，handle = 管道下标 */
#define FD_KIND_FAT16   4 /* FAT16 交换盘文件，handle = fat16 句柄下标 */
#define FD_KIND_UDP     5 /* UDP socket，handle = 本地端口 */
#define FD_RIGHT_READ   0x1U
#define FD_RIGHT_WRITE  0x2U
#define PROC_MMAP_MAX   4U

typedef struct {
    uint32_t base;
    uint32_t len;
} proc_mmap_t;

typedef struct {
    bool     used;
    uint8_t  kind;        /* FD_KIND_* */
    uint8_t  fs_be;       /* 该 fd 属于哪个文件系统后端（FS_BE_*），仅 FD_KIND_FILE 有效 */
    uint32_t handle;      /* fs 句柄或管道下标 */
    uint32_t rights;      /* FD_RIGHT_*：per-process capability rights */
} proc_fd_t;

typedef struct process {
    uint32_t     pid;
    proc_state_t state;
    const char  *name;          /* 程序名，便于日志 */
    void       (*entry)(void);  /* 程序入口 */
    void        *stack_base;    /* 栈内存起点（用于回收） */
    void        *stack_top;     /* 栈顶：供 TSS.esp0 使用 */
    void        *esp;           /* 切换时保存的栈指针 */
    void        *page_dir;     /* 本进程页目录（PAE 下为 PDPT；切换时装入 CR3） */
    uint32_t     timeslice;     /* 剩余 tick 数 */
    uint8_t      base_prio;     /* 静态优先级 0..7（越小越高），默认 4 */
    uint8_t      eff_prio;      /* 当前有效优先级：aging 提升后暂时小于 base */
    uint16_t     wait_ticks;    /* 在 runqueue 上连续等待的 tick 数（aging 用） */
    bool         prio_set;      /* base_prio 是否已经初始化（新进程首次入队时置默认值） */
    uint32_t     runs;          /* 被调度上 CPU 的累计次数（诊断/自检用） */
    uint32_t     aged;          /* aging 提升过多少次（诊断/自检用） */
    unsigned int wait_chan;     /* 阻塞时睡在哪个通道 */
    bool         slept;         /* 是否曾经睡过（只打印一次日志） */
    bool         wake_pending;  /* 还在 block→switch 窗口里被唤醒：切走后补入队 */
    uint32_t     parent_pid;    /* 父进程 pid（wait 用） */
    char         cwd[192];      /* 当前目录（形如 "system:/docs"） */
    uint32_t     user_brk;      /* 用户堆当前边界（brk 系统调用） */
    uint32_t     user_stack_top;/* 本进程随机化后的栈顶（ASLR） */
    uint32_t     user_stack_va; /* 初始栈页虚拟地址（栈顶 - 4KB） */
    uint32_t     user_heap_base;/* 本进程随机化后的堆基址（ASLR） */
    uint32_t     user_trampoline;/* 本进程随机化后的信号蹦床页（ASLR） */
    proc_fd_t    fds[PROC_FD_MAX];  /* 文件描述符表 */
    cap_table_t  caps;              /* 通用对象 capability 表 */
    uint32_t     mmap_next;         /* 匿名 mmap 区当前偏移 */
    proc_mmap_t  mmaps[PROC_MMAP_MAX];
    uint32_t     user_eip;      /* fork 子进程返回到的用户地址 */
    uint32_t     user_esp;
    uint32_t     fork_regs[4];  /* fork 子进程恢复的 ebx/ebp/esi/edi */
    bool         is_fork_child;
    bool         is_user;       /* 是否真的进过 ring3（信号只发给用户进程） */
    uint32_t     wake_at;       /* 睡眠到期 tick */
    uint32_t     pending_sig;   /* 待处理信号 */
    uint32_t     sig_handlers[NSIG_MAX];  /* 0=SIG_DFL, 1=SIG_IGN, 其它=用户处理函数 */
    uint32_t     sig_frame;     /* 当前信号帧在用户栈的地址（sigreturn 用） */
    int          exit_code;
    uint32_t     uid;           /* 用户 ID（0 = root） */
    uint32_t     gid;           /* 组 ID */
    bool         sandboxed;     /* 是否启用 syscall allowlist 沙箱 */
    uint64_t     syscall_mask;  /* 允许的 legacy syscall 位图（bit = syscall 号） */
    uint32_t     native_mask;   /* native ABI 允许位图，见 native.c 的映射 */
    int32_t      cpu_pin;       /* -1 = 任意 CPU；>=0 = 亲和到该 CPU */
    uint32_t     last_cpu;      /* 最近一次在哪个 CPU 上运行（0xFFFFFFFF=从未） */
} proc_t;

/* 初始化进程表 */
void proc_init(void);

/* 从程序表启动一个程序，返回进程；失败返回 0 */
proc_t *proc_spawn(const char *program_name);

/* 当前进程退出（不返回） */
void proc_exit(int code) __attribute__((noreturn));

/* 主动让出 CPU */
void proc_yield(void);
/* AP timer / IPI 调用：没有 current 时从本 CPU runqueue 取活；有 current 时轮转。 */
void sched_ap_tick(void);

/* 在当前线程上阻塞，直到有人向 chan 调 proc_wakeup。
 * 调用后线程让出 CPU；被唤醒时从本函数返回。 */
void proc_block(unsigned int chan);

/* 唤醒所有睡在 chan 上的线程（可在中断里调用） */
void proc_wakeup(unsigned int chan);

/* ---- fork / wait（P3） ---- */
struct interrupt_frame;
/* 复制当前进程（用户页、fd 表、cwd），子进程从 fork 返回 0。
 * 返回子进程 pid；失败返回 0。 */
proc_t *proc_fork(struct interrupt_frame *f);

/* 等待子进程退出并回收；status 收退出码。返回子 pid，失败 -1。 */
int proc_wait(int pid, int *status);

/* 带 options 的等待（SYS_WAITPID 用）：
 *   - options == 0：同 proc_wait()；
 *   - options & PROC_WAIT_WNOHANG：没有僵尸可回收时立即返回 0（不阻塞），
 *     有僵尸则回收并返回其 pid；没有匹配的子进程也返回 0（见文档 §5.2）；
 *   - 参数非法/无当前进程返回 -1。 */
#define PROC_WAIT_WNOHANG 0x1U
int proc_wait_options(int pid, int *status, int options);

/* 当前进程的父 pid；内核线程/无父返回 0。 */
int proc_getppid(void);

/* 按 pid 查找活动进程；找不到返回 0。 */
proc_t *proc_find(uint32_t pid);

/* ---- 睡眠与信号 ---- */
/* 当前线程睡 ticks 个时钟 tick（100Hz 下 1 tick = 10ms） */
void proc_sleep_ticks(uint32_t ticks);

/* 时钟中断调用：唤醒到期睡眠者 */
void proc_wake_sleepers(uint32_t now);

/* 给进程投递一个信号 */
void proc_send_signal(proc_t *p, int sig);

/* 设置信号处理函数；返回旧值（0=默认，1=忽略，其它=用户地址） */
int proc_signal_set(int sig, uint32_t handler);

/* 返回用户态前检查待处理信号：投递给用户 handler 或按默认动作终止 */
struct interrupt_frame;
void proc_check_signals(struct interrupt_frame *f);

/* SYS_SIGRETURN：从用户栈信号帧恢复现场 */
int proc_sigreturn(struct interrupt_frame *f);

/* Ctrl+C：给 shell 的子进程发 SIGINT */
void proc_console_int(void);

proc_t *proc_current(void);
uint32_t proc_count(void);
const proc_t *proc_at(uint32_t index);

/* ---- 由调度器实现（sched.c） ---- */
/* 时钟中断里调用：递减时间片，到期就切换 */
void sched_tick(void);
/* 启动调度器：切到第一个可运行进程，正常情况下不返回 */
void sched_start(void) __attribute__((noreturn));

#endif
