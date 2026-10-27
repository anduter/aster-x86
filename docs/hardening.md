# 加固批次（v0.74.2）

基线：0.74.1 / HEAD `3f91e19` / 248 项检查。本批次每项独立提交、独立回归；
新增自检行都进 `build.ps1` 的 checks，最终 `ci.ps1 -Full -StrictBom` 连跑两遍。

约定：

- 现象、根因、修复、回归、遗留五段，一项一节。
- 回归行是**内核/用户态打印的稳定字符串**，由 `build.ps1` 断言；手工 `-smp 4`
  验证另记串口证据。
- 设计级问题不硬改，记在每节“遗留”和文末“未修项”。

---

## 1. 4 核 AP LAPIC tick 偏斜（P1）

**现象**：`-smp 4` 启动 2s 时 `smp: tick check after 2s bsp=200`，三个 AP 分别是
368/286/203，核间 tick 最多差 84%。

**根因**（三层，叠在一起）：

1. **校准分频没有固定**。`apic_watchdog_start()` 用 LAPIC 当前计数做 100ms
   校准，但没写 DCR，依赖复位值；真实硬件/QEMU 复位分频是 /2，而
   `apic_tick()` 的 `period = elapsed * 10 / 4` 与 AP 的
   `count = period / 400` 都假设校准在 divide-by-1。分频差一倍，AP 定时器
   频率就错一倍。
2. **校准窗口只取 10 个 PIT tick**。QEMU/TCG 下 PIT 边沿在 IF=0 或投递延迟时
   会被 IOAPIC 合并，10 个 tick 不一定是 100ms；启动早期尤其严重，校准出的
   period 偏大，AP 的 10ms 被装成 12–15ms。
3. **AP tick 基准没有对齐**。AP 在 BSP 校准完成前用启动初始计数
   （`1,000,000` @ /16）跑过一段，且各 AP 启动时刻相差 ~0.5s；即使频率正确，
   2s 时刻的累计 tick 也天然不齐（368/286/203 的阶梯就是启动顺序）。

**修复**：

- `dev/intc/apic.c`：校准前显式 `DCR = 0x0B`（divide-by-1）；NMI 周期也用
  /1 计数，公式才成立。
- 校准窗口从 10 个 tick 加长到 100 个 tick（`APIC_CAL_TICKS`）。per-CPU tick
  要和 BSP 的 PIT tick 直接比较，因此 AP 的“10ms 计数”按 BSP 实际 tick
  间隔（含被合并的边沿）来定；100 tick 把启动早期的高延迟摊平。
- `proc/smp.c`：AP 在第一次拿到校准计数、重装自己的 LAPIC 时，把
  `per_cpu[c].ticks` 置为 `timer_ticks()`（BSP 墙钟），之后按校准后的周期推进。
  这消除了 AP 启动先后和校准前未校准 tick 的影响。
- `proc/trap/irq.c`：tick 200 时新增自检行，比较所有在线 CPU 的绝对 tick
  与 BSP 的偏差，`max_delta <= 10%` 打印 ok。

**回归**：

```text
smp: tick check after 2s bsp=200 ap=189 ap2=0 ap3=0 (online=2)
smp: ap tick skew ok (max_delta=5%, limit=10%, window=2s)

# -smp 4 手工（serial-smp4.log）
smp: tick check after 2s bsp=200 ap=191 ap2=192 ap3=191 (online=4)
smp: ap tick skew ok (max_delta=4%, limit=10%, window=2s)
```

`build.ps1` 新增检查 `AP tick 校准无偏斜`；`-Test` 249/249 PASS。

**遗留**：

- 自检是 2s 绝对 tick 比较，AP 若在 tick 200 之后才上线会天然落后；当前
  `smp_start_aps()` 在 `sti` 前完成，实际不会发生。若将来支持热插拔 CPU，
  应改成“上线后 2s 窗口”的增量比较。
- 校准仍是启发式（100 tick 平均），TCG 负载极端时 190±10% 是预期下限；
  `max_delta` 打印出来便于观察长期漂移。
---

## 2. 信号打断阻塞 syscall → EINTR（P1）

**现象**：阻塞在 `accept`/`read`/pipe/wait 上的进程收不到 `-EINTR`；发了
SIGKILL 也不一定退出，只能等超时（TCP accept 2s、管道永久）。

**根因**：两层。

1. `proc_send_signal()` 只在目标 `state == PROC_BLOCKED` 时调用
   `sched_wake()`。目标如果处在“检查完条件、还没真正置 BLOCKED”的 RUNNING
   窗口，信号只写入 `pending_sig`，没有 wake latch；随后 `proc_block()` 正常
   睡下，再也没有人叫醒它 —— 信号（含 SIGKILL）丢唤醒。这正是回归里偶发
   卡死的原因：同样的测试有时过、有时 100s 超时。
2. 阻塞循环本身不检查 `pending_sig`。即使被唤醒，`accept` 的超时计数、
   `pipe_read` 的条件循环都会重新睡回去，把这次唤醒“吃掉”。

**修复**：

- `proc/process.c`：`proc_send_signal()` 改为**无条件** `sched_wake()`。
  `sched_wake()` 对 RUNNING/READY 目标只置 `wake_pending`（由
  `proc_block()` 的 `sched_lock` 临界区消费），不会迁移正在运行的进程；
  对 BLOCKED 目标才真正入队。与 `proc_wakeup()` 的既有语义一致。
- 新增 `proc_signal_pending()`（当前进程有未屏蔽信号）。
- `net/tcp.c`：`net_tcp_accept()`/`net_tcp_conn_read()` 睡前检查，返回
  `-EINTR`；`net/net_syscall.c` 原样透传（SYS_ACCEPT、SYS_RECVFROM 不再
  统一改写成 -1）。
- `fs/pipe.c`：`pipe_read`/`pipe_write` 睡前检查；已传输的部分字节照常返回，
  一个字节都没有时返回 `-EINTR`。
- `proc/process.c`：`proc_wait_options()` 在真正 block 前检查；僵尸仍优先
  回收，SIGCHLD 不会抢走可回收的子进程。
- `proc/trap/syscall_util.c`：`sys_getchar_block()` 检查；`fs/fs_syscall.c`
  把 stdin 的 `-EINTR` 和管道写的 `-EINTR` 原样透传，不折叠成 -1。
- `core/error.h`：`#define EINTR 4`（Linux errno 4，syscall 返回 -4）。
- 返回用户态前 `proc_check_signals()` 本来就会跑：SIGKILL 走默认终止
  （退出码 128+9），装了 handler 的信号跳转到 handler，不会吞掉。

**回归**（`-Test` 注入 `sectest` 时跑，避免启动期并发影响信号到达时机）：

```text
ushell: einr read ok
ushell: einr accept ok
ushell: einr kill ok
```

`build.ps1` 新增 3 项检查；`-Test` **252/252 PASS**，连续两次。

**遗留**：

- `proc_t.pending_sig` 仍是单槽位（同信号覆盖、不同信号后者覆盖前者）。
  多信号排队需要 `sigset_t`/位图 + 屏蔽字，属于设计级改动，本批不做。
- SIGKILL 在 `proc_check_signals()` 里走通用“默认终止”，没有单独的
  “不可捕获/不可忽略”快速路径；功能正确，但若 handler 表被写坏，
  SIGKILL 仍可能被错误投递。后续把 SIGKILL/SIGSTOP 语义单列。
- 管道写返回部分字节 + 后续 `-EINTR` 的组合已按 POSIX 语义处理，但用户态
  libc 没有 errno 全局变量，调用方只能看返回值 -4。
---

## 3. chroot 绝对软链接绕过（P1 安全）

**现象**：chroot 到 `system:/jail` 后，进程内 `/link -> /etc/passwd` 仍能读到
卷根的 `/etc/passwd`（真实口令文件）。

**根因**：chroot 是在 syscall 层用 `make_abs_path()` 给**用户传入的路径**
加根前缀实现的；`ramfs.c` 的 `resolve_path()` 在跟随软链接时只拿到卷内相对
路径，不知道进程根。绝对目标 `"/x"` 被当成“卷根”而不是“chroot 根”解析。

**修复**（`fs/ramfs.c`）：

- 解析软链接时取当前进程的 chroot 根（`proc_root_of()`，新增到模块接口
  `include/proc.h`），换算成本卷内前缀（如 `jail`）。
- 绝对目标 `"/x"` 重定位为 `<chroot>/x`；相对链接语义不变。
- chroot 生效时，目标路径含 `".."` 分量一律拒绝（跳数上限只防环，不防
  向上爬出 chroot）；嵌套链接递归解析时同样应用。
- 未 chroot / 内核线程（`proc_current()==0`）路径完全保持原行为。

**回归**（`sectest`，-Test 构建）：子进程在 `/tmp` 建
`/tmp/ct-link -> /etc/passwd`，chroot 到 `system:/tmp` 后打开 `/ct-link`，
必须失败：

```text
fs: audit chroot symlink containment ok
```

`build.ps1` 新增 1 项检查；`-Test` **253/253 PASS**。

**遗留**：

- chroot 只做路径前缀重定位，没有独立的 root 目录对象/引用计数；chroot
  根被删除后行为未定义（与 Linux 的“已 chroot 进程仍持有根”不同）。
- 只堵了符号链接这一条；`/proc/self/root` 之类接口不存在，暂无其它绕过面。
- 绝对硬链接不可能（link 只接受同卷路径），不在本项范围。