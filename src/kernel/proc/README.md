# proc/ - 进程与调度模块

这个目录专门放“进程”相关代码。所有文件都遵循一条约束：
**进程是内核对象，shell 等应用只是它的使用者。**

## 当前状态：P1 已完成（内核线程 + 抢占调度）

| 文件 | 职责 |
|---|---|
| `proc.h` | 对外接口：`proc_spawn` / `proc_exit` / `proc_yield` / `proc_current` |
| `proc_internal.h` | process.c 与 sched.c 共享的进程表、当前进程、切换函数 |
| `process.c` | 进程表、线程创建（栈与初始上下文）、退出、上下文切换封装 |
| `sched.c` | 环形轮转：`sched_next` / `sched_tick` / `sched_start` |
| `switch.asm` | `context_switch`（保存/恢复 callee-saved + esp）与 `thread_trampoline` |

行为：

- `proc_spawn("shell")` 从**程序表**取入口，分配 8KB 栈，摆好初始上下文（edi/esi/ebx/ebp + 返回地址）；
- 调度器 `sched_start()` 切到第一个 READY 进程，之后不再返回内核；
- 每个 tick（10ms）递减时间片，5 个 tick（50ms）到期就在时钟中断里抢占切换；
- 线程可以 `proc_yield()` 主动让出；`proc_exit()` 置 ZOMBIE 后切走，永不返回；
- 切换在关中断下进行，恢复时按各线程自己的 EFLAGS 还原 IF。

实测：`shell`（pid 1）与 `heartbeat`（pid 2）交替运行——心跳每秒打印一行，
同时 shell 仍能响应键盘命令。

## 计划放置的文件

| 文件 | 职责 |
|---|---|
| `proc.h` | 对外接口：`proc_spawn` / `proc_exit` / `proc_wait` / `proc_info` |
| `process.c` | 进程表、状态机（READY / RUNNING / BLOCKED / ZOMBIE）、生命周期 |
| `sched.c` | 调度器：就绪队列、时间片、抢占决策（时钟中断里调用） |
| `switch.asm` | 上下文切换（保存/恢复 callee-saved 寄存器与栈指针） |
| `thread.c` | 内核线程（先于用户进程实现，用来验证调度） |

## 与现有模块的关系

```
core/program.c   程序注册表：proc_spawn 的参数是 program 句柄，不是文件路径
mm/pmm.c         物理页：进程页目录、内核栈、用户栈都从这里分配
mm/vmm.c         地址空间：每个进程一套页目录（P1 先共享内核页表）
intr/irq.c       时钟中断：驱动时间片与抢占（现在只做 ticks++）
apps/shell.c     第一个使用者：将来用 proc_spawn 启动程序，而不是 prog_run
```

## 里程碑

### P1：内核线程 + 调度（不涉及用户态）

- `proc_t` 结构：pid、状态、内核栈、上下文（esp/eip/callee-saved）、优先级、时间片；
- `proc_create_kernel(entry, arg)`：在内核地址空间里创建一个可运行实体；
- 时钟中断里递减时间片，到期调用 `sched_tick()` 选择下一个；
- `switch.asm` 完成栈切换；
- 验收：两个内核线程交替打印各自计数，串口日志能看到交替输出。

### P2：用户态进程 + 系统调用

- 每个进程独立页目录（复制内核页表项，用户区独立）；
- `int 0x80` 系统调用门 + 参数校验（句柄类型检查已有 `obj_deref`）；
- `proc_spawn(program_id, ...)`：映射程序映像（只读共享）、建用户栈、进 ring3；
- 异常处理分级：用户态异常终止进程，内核态异常仍 panic；
- shell 搬到用户态，`prog_run` 替换为 `proc_spawn` + `proc_wait`。

## 接口草案

```c
typedef enum {
    PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE
} proc_state_t;

typedef struct process {
    uint32_t      pid;
    proc_state_t  state;
    uint32_t      *page_dir;        /* P2 起每进程一套 */
    void          *kstack;          /* 内核栈顶 */
    void          *context;         /* 切换时保存的寄存器现场 */
    uint32_t      timeslice;        /* 剩余时间片（tick） */
    int           exit_code;
    struct process *next;           /* 就绪/阻塞队列链表 */
} proc_t;

proc_t *proc_spawn(const char *program_name, const char *args);
void    proc_exit(int code);
int     proc_wait(uint32_t pid, int *code);
void    proc_yield(void);
void    sched_init(void);
void    sched_start(void);          /* 启动第一个进程后不再返回 */
```

## 注意

- 进程表要有上限（参考对象表的 `OBJ_MAX`），避免动态分配失败时无路可退；
- 上下文切换必须保存 FPU 状态（或约定内核不使用浮点，先不保存）；
- 调度器不要在中断上下文里做耗时操作（输出一律留给进程自己）；
- 退出进程的资源回收（页目录、内核栈、句柄表）要有明确的所有者，避免泄漏。
