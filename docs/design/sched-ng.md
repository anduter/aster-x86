# ng 调度/线程/上下文切换（WP1-5）

> 配套 `docs/claims/ng-WP1-5.md`（证据）与 `docs/design/decisions.md` ADR-017。
> 代码：`src/ng/thread.h`、`src/ng/sched.c`、`src/ng/sched_arch.[ch]`、
> `src/ng/switch.S`、`src/ng/sched_selftest.[ch]`；host 单测在
> `tests/unit-ng/test_sched.c`。

## 1. 范围与边界

WP1-5 只做**单核、协作式**调度：上下文切换只发生在显式调用
`sched_yield()` / `thread_sleep()` / `thread_exit()` 的地方，没有 IRQ tick、
没有抢占、没有 SMP、没有优先级、没有 IPC（IPC 在 WP1-6）。

| 要做 | 不做（本 WP） |
|---|---|
| 线程 TCB、16 KiB 内核栈、READY/RUNNING/BLOCKED/DEAD | 抢占式时间片 / IRQ tick 切换 |
| 汇编 `switch_context`（rbx/rbp/r12–r15/rsp/rip） | 内核栈守护页（unmap 一页） |
| round-robin runqueue、idle、exit 延迟回收 | 优先级、公平份额、CPU 亲和 |
| TSC 定时睡眠 + 显式唤醒 | 等待队列/IPC 阻塞、超时重启 |
| 自检 + host 单测（fake 环境） | ELF64 / ring3 线程、用户态线程 |

接口向 tick 预留：`sched_tick()` 只做到期唤醒并记数；将来 IRQ handler 里
调用它，再由返回路径决定是否 yield（WP1-6 的抢占式调度）。

## 2. 线程模型

```c
struct thread_context {          /* 64 字节，偏移被 switch.S 硬编码 */
    u64 rbx, rbp, r12, r13, r14, r15, rsp, rip;
};

typedef struct thread {
    u32 tid, state, flags, wake_reason;
    struct thread_context ctx;
    void (*start)(void *arg);
    void *arg;
    void *stack_base, *stack_top;   /* heap 分配，16 KiB */
    u64 stack_size, wake_tick, switches;
    struct thread *next;            /* ready / blocked / zombie 链 */
    struct thread *all_next;        /* 所有存活线程（按 tid 查） */
} thread_t;
```

- **TCB**：静态池 `thread_pool[NG_THREAD_MAX=16]`；0 号槽是 boot 线程
  （kmain 的引导栈，不回收）。tid 从 1 递增。
- **栈**：`thread_create()` 从 heap `kmalloc(16 KiB)`（heap payload 16 字节
  对齐，所以栈顶也 16 对齐）。栈底 16 字节写 canary，`thread_exit()` 校验，
  溢出计入 `sched_stack_corrupt_count()`。
- **状态机**：

```text
UNUSED --create--> READY --pick--> RUNNING --yield--> READY
                     ^               |   \--sleep--> BLOCKED
                     |               |                |
                     +------wake-----+                |
                                     \--exit--> DEAD --reap--> UNUSED
```

- **守护页**：没有实现。内核 `[2 MiB, 1 GiB)` 是 2 MiB 恒等大页，要 unmap
  一页守护页必须先把对应 2 MiB 拆成 4 KiB 页表，属于页表工作；本 WP 用
  栈底 canary 作低成本替代，作为 WP 边界记录。

## 3. 上下文切换（`switch.S`）

```asm
switch_context:                 # rdi = old, rsi = new
    movq %rbx,0(%rdi) ... movq %r15,40(%rdi)
    leaq 8(%rsp),%rax           # 保存“ret 之后”的 rsp
    movq %rax,48(%rdi)
    movq (%rsp),%rax            # 返回地址 = 恢复后的 rip
    movq %rax,56(%rdi)
    movq 0(%rsi),%rbx ... movq 56(%rsi),%rax
    movq 48(%rsi),%rsp
    jmp *%rax
```

要点：

- 保存的 `rsp` 是 `ret` 之后的值（`leaq 8(%rsp)`），`rip` 是 return address；
  恢复时用 `jmp` 而不是 `ret`，因此**不需要在栈上伪造返回地址**，栈状态与
  “那次调用正常返回”完全一致。
- 调用点遵守 SysV：`call` 前 rsp 16 对齐，函数入口 rsp%16==8；保存的 rsp
  在恢复后 %16==0，随后 `jmp` 进入 C 调用者，对齐天然正确。
- 只保存 callee-saved（rbx/rbp/r12–r15）+ rsp/rip；rax/rcx/rdx/rsi/rdi/
  r8–r11 是 caller-saved，C 编译器本来就假定调用会破坏它们。
- 新线程第一次被调度：`ctx.rip = thread_entry`、`ctx.rsp = stack_top`
  （16 对齐）。`thread_entry` 先 `cld`、`andq $-16,%rsp`，再
  `call thread_bootstrap`；`thread_bootstrap()` 跑 `start(arg)`，返回后调
  `thread_exit()`（不返回；真回来则 `ud2`）。
- `struct thread_context` 的 8 个偏移在 `thread.h` 里用
  `_Static_assert(__builtin_offsetof(...))` 钉死，改布局会编译失败。

## 4. 调度器

### 4.1 数据结构与规则

`struct sched_core` 是纯状态机（不碰栈/时钟/上下文）：

| 链 | 用途 |
|---|---|
| `ready_head/ready_tail` | FIFO runqueue；pick 队头、yield 放队尾 = round-robin |
| `blocked_head` | `thread_sleep()` 的线程；`wake_tick` 是 TSC 到期值（0=只等显式唤醒） |
| `zombie_head` | 已 DEAD、等回收的线程 |
| `all_head` | 所有存活线程，`thread_wake(tid)`/诊断用 |

计数：`created/switches/picks/yields/blocks/wakes_timer/wakes_explicit/
exits/reaps/idle_runs`，以及 `ready_count/blocked_count/zombie_count/
live_count`。

规则：

- `sched_core_pick()` 弹出 ready 队头并置 RUNNING；没有可运行线程返回 NULL。
- `sched_core_block()` 只接受 RUNNING → BLOCKED，挂 blocked 链，记 deadline。
- `sched_core_poll(now)` 扫描 blocked 链，`wake_tick != 0 && now >= wake_tick`
  的线程搬回 ready 队尾并记 `wakes_timer`。
- `sched_core_wake()` 只接受 BLOCKED，搬回 ready 队尾并记 `wakes_explicit`；
  状态不对返回 `THREAD_EBAD`。
- `sched_core_zombie()` 置 DEAD、live_count--；`sched_core_reap(skip)` 取一个
  zombie（跳过当前正在跑的线程），从 all 链摘除并置 UNUSED。

### 4.2 切换点（`sched.c`）

```text
sched_yield:   current RUNNING -> READY，放 ready 队尾 -> sched_switch
sched_switch:  reap_zombies(skip=current) -> poll(TSC) -> pick
               next == NULL 且 current 仍 RUNNING：原地返回（不空切）
               next == current：原地返回（队列里只有自己）
               否则 current = next; switches++; switch_context(prev, next)
thread_exit:   canary 校验 -> zombie(self) -> sched_switch ->（不返回）
thread_sleep:  ticks==0 等价 yield；否则 block(deadline=now+ticks) -> switch
               返回 THREAD_WAKE_TIMER / THREAD_WAKE_EXPLICIT
```

**为什么 exit 延迟回收**：`thread_exit()` 不能在自己的栈上 `kfree`。退出线程
只变 DEAD 挂 zombie 链；下一次任何线程调度时，`sched_switch()` 先回收
zombie（跳过当前线程），安全地释放栈并把 TCB 槽清回 UNUSED。

### 4.3 idle 线程

`sched_start()` 创建 idle 线程（普通 TCB、独立 16 KiB 栈）并第一次
`sched_yield()` 让出 CPU。idle body：

```c
for (;;) {
    if (ready_count == 0 && blocked_count == 0) { cpu_hlt(); }  /* 无 IRQ 即终态 */
    sched_yield();                                             /* 轮询定时器 */
}
```

有睡着的线程时 idle 用 yield 忙轮询 TSC（协作式内核没有 tick 中断），
既没有 runnable 也没有 blocked 时才 `hlt`。将来接 tick IRQ 后，这里可以换成
真正的 `sti; hlt` 等中断。

### 4.4 时间源

`sched_arch_time()` = `rdtsc`（`sched_arch.c`）。`thread_sleep(ticks)` 的
`ticks` 是 TSC 增量，不做频率换算；自检用 5,000,000 tick（QEMU 下几毫秒）。
`deadline` 回绕到 0 时改成 1，避免和“只等显式唤醒”的 0 冲突。

### 4.5 与将来 tick IRQ / SMP 的关系

- 已提供 `sched_tick()`：只 poll 到期线程，不切换。
- 抢占式调度只需要在 IRQ handler 调用 `sched_tick()`，并在返回用户态/安全点
  调用 `sched_yield()`；TCB/状态机/回收路径不用改。
- SMP 需要每 CPU runqueue 和锁；当前所有 `sched_*` 假定单核 + IF=0，
  `thread_pool`/`sched` 都是全局静态，不能直接在 AP 上跑。

## 5. 自检（`sched_selftest.c`）

顺序（run-ng 的 5 条出口行）：

1. `sched_init()` + 创建 A/B/C 三个线程；断言 created=3、live=4、
   栈 16 KiB、栈基/栈顶/ctx.rsp 16 对齐、tid 唯一、canary 有效：
   `ng: sched create ok (3)`。
2. `sched_start()` 建 idle 并开始 round-robin。A/B/C 各循环 3 次
   “打印 `ng: thread <A|B|C> i` + yield”，主线程计数等待三个都退出并被回收。
   断言 trace 严格等于 `ABCABCABC`、每线程 3 次、切换数 ≥6：
   `ng: sched order ok (ABCABCABC)`、`ng: sched switch ok (rounds>=6)`。
3. 三个线程都 DEAD 且回收，zombie=0，blocked=0，栈只剩 idle 的 1 个：
   `ng: sched exit reap ok`。
4. D 线程 `thread_sleep(5,000,000)`：主线程先观察到 D 进 BLOCKED，再等它
   被定时器唤醒并回收；断言唤醒原因=TIMER、`wake_tsc >= deadline`、
   `wake_tsc - start >= ticks`。
5. E 线程 `thread_sleep(2^60)`：主线程观察到 BLOCKED 后 `thread_wake(tid)`
   显式唤醒；断言原因=EXPLICIT、重复唤醒 `THREAD_EBAD`、唤醒不存在的 tid
   `THREAD_ENOENT`：
   `ng: sched sleep/wake ok`。
6. 收尾检查 `sched_stack_live()==1`、`sched_stack_corrupt_count()==0`、
   zombie=0。main.c 打印 `ng: WP1-5 selftest PASS`。

所有等待都有 1,000,000 次 yield 的自旋上限，失败不会拖到 run-ng 超时；
失败行统一 `ng: sched ... FAIL`，run-ng 的失败模式会提前结束。

## 6. host 单测（fake 环境）

`sched.c` 只依赖 `sched_arch.h` 的 6 个钩子：TSC、栈分配/释放、上下文切换、
新线程入口、idle。`tests/unit-ng/test_sched.c` 提供 fake 版本：

- fake 时钟（测试自己推进 `now`）；
- fake 栈（malloc/free，记录次数）；
- fake switch（空实现：纯状态机测试不切栈，被调用即 `CHECK(0)`）；
- fake idle（no-op）。

覆盖：FIFO/round-robin 顺序与公平性、BLOCKED 不被调度、定时 poll 边界
（99/100/199/200）、deadline=0 不超时、显式唤醒的队尾顺序、唤醒错误码、
zombie/reap/skip、live 计数、`thread_create`（tid、16 KiB、对齐、canary、
ctx 初值、FIFO 入队、槽位上限、NULL fn）、`thread_sleep(0)` 语义、
`thread_wake` 错误路径。当前 **390 checks, 0 failures**（WP1-4 为 166，
本 WP +224）。

## 7. 边界 / 遗留

- **协作式**：线程不让出就永远占 CPU；没有 tick IRQ、没有抢占。
- **单核**：全局 `sched`/`thread_pool`，无锁；SMP 每 CPU runqueue 留给后续。
- **无优先级/时间片/老化**：纯 FIFO round-robin。
- **无守护页**：栈溢出只靠 canary 发现（且只在退出时检查）；栈固定 16 KiB，
  TCB 上限 16（含 boot/idle）。
- **BLOCKED 只有一个原因（sleep）**：没有等待队列/信号量/IPC 阻塞；wake 按
  tid 查 all 链（O(n)，n≤16）。
- **退出线程的栈延迟回收**：依赖“下一次别人调度”触发；如果最后一个可运行
  线程退出且没有 idle，就 `hlt`（当前 `sched_start` 总会建 idle）。
- **TSC 不做频率校准**：`thread_sleep` 的 tick 是原始 TSC 增量，跨机器不可比。
- **用户线程/ring3 线程**：不做；`thread_t` 只表示内核线程，ELF64/用户态线程
  留给 WP1-7 之后。
