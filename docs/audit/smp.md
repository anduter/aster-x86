# audit-smp：SMP 并发/竞态崩溃扫描（2026-10-27）

会话：`audit-smp`　基线：`main`（集成提交 `38161af`，225 项检查）　预算：8 h

范围：`proc/**`、`mm/**`、`dev/intc/**`（只修这三处）；测试与文档落在
`tests/audit/smp/**`、`tools/audit-smp*.ps1`、`docs/audit/smp.md`。
独立构建：`.obj-audit-smp` / `.dist-audit-smp`；`-smp 4` 使用单独的镜像副本
`.dist-audit-smp4`；QEMU QMP **4544**，串口 `obj/audit/smp.log`（4 核为
`obj/audit/smp4.log`）。未使用 `session-run`、未占用 12345。

## 1. 方法

1. 静态读进程/内存状态机与锁：`proc/sched.c`、`proc/process.c`、
   `proc/trap/*`、`mm/vmm.c`、`mm/pmm.c`、`mm/heap.c`、`dev/intc/*`；
   画锁顺序（`rq_lock[*] → sched_lock`、`vm_lock → pmm_lock`）与
   `proc_t` 状态迁移（FREE/READY/RUNNING/BLOCKED/ZOMBIE）。
2. 写内核级并发自检 `proc/audit_smp_selftest.c`（调度器启动前注册/放置，
   避免“刚 spawn 就被调度”的第二竞态），三个测试：
   - 两 CPU 屏障同步进入 `proc_spawn()`，比对槽位/pid；
   - 目标线程 RUNNING 时从另一 CPU `proc_wakeup()`（check→block 丢失窗口）；
   - 两 CPU 并发 `vmalloc()/vfree()`。
   另有 `proc/audit_priv_selftest.c`（权限边界，见 §3.5–3.7）。
3. 2 核/4 核各跑；失败现场用 `sched: dbg`（临时插桩）定位、随后清理。
   复现脚本：`tools/audit-smp-run.ps1`；断言脚本 `tests/audit/smp/check.ps1`。

## 2. 修复清单

| # | 类别 | 文件 | 现象 | 修复 |
|---|---|---|---|---|
| 1 | 槽位/pid 竞态 | `proc/process.c` | 并发 fork/spawn 选中同一 `proc_t`；pid 重复 | `proc_slot_lock` 串行化扫描+占位+`next_pid` |
| 2 | lost wakeup | `proc/sched.c`、`proc/process.c` | check→block 窗口内的唤醒被丢，waitpid/pipe/getchar 永久睡 | `wake_pending` latch + `sched_block_begin()` 锁内消费 |
| 3 | runqueue 损坏 | `proc/process.c`、`proc/sched.c` | hlt 阻塞分支返回后未重新入队 → RUNNING 不在任何 runq → 永久饿死 | `proc_block()` 返回前 `sched_enqueue()`；新增 RUNNING⇒在队断言 |
| 4 | 全局分配器竞态 | `mm/vmm.c` | 并发 vmalloc 拿到同一虚拟区间/重复释放物理页 | `vm_lock` 保护 bitmap+映射+释放 |
| 5 | 看门狗盲区（加固） | `dev/intc/apic.c` | BSP PIT 停摆被 AP syscall 掩盖，静默 hang | NMI 里检查 BSP tick 停摆（~2s）并 panic |

汇总：**4 个并发 bug**（其中 #3 是静默饿死、#1 可导致双核同栈执行/pid 重复），
外加 1 处看门狗加固；`tcptest` 偶发卡死（A/B 10% → 0%）是 #3 的下游表现，
不是独立 bug。另接收 `audit-syscall` 的 3 个 P0（§3.5–§3.7），共 7 项修复
+ 1 项加固。

### 3.1 并发 fork/spawn 的槽位与 pid 分配竞态（bug 1）

**现象/复现**：两颗 CPU 同时 `proc_spawn()`/`proc_fork()`。旧实现：

```c
for (i..PROC_MAX) if (proc_table[i].state == PROC_FREE) { p = &proc_table[i]; break; }
...
void *stack = malloc(PROC_STACK_SIZE);      /* 窗口很大 */
p->pid = next_pid++;  p->state = PROC_READY;
```

`FREE` 槽位在 `state=READY` 之前一直对另一颗 CPU 可见，且 `next_pid++`
不是原子读改写。两个新进程会落到同一个 `proc_t`：后写者的
`stack_base/page_dir/esp` 覆盖前者，两者共用同一内核栈；`next_pid` 丢更新
还会让两个进程拿到同一个 pid（waitpid 认错子进程）。
自检 `audit: smp spawn race`：两 worker 用两方屏障同时 spawn，比较
“仍存活的两个子进程”的槽位与 pid。

**修复**：`proc_slot_alloc()` 在 `proc_slot_lock` 内完成“扫描 FREE +
`state=READY` 占位 + `next_pid++`”；`proc_slot_discard()/proc_slot_clear()`
用同一把锁 memset，避免 allocator 看到 `state` 已清零、其余字段半清零的槽位。
`proc_spawn()`/`proc_fork()` 的失败回滚改用 `proc_slot_discard()`（尚未入队，
不需要 rq/sched 锁）。锁序：`rq_lock[*] → sched_lock → proc_slot_lock`。

**回归**：`audit: smp spawn race PASS (rounds=24)`（修复前稳定
`FAIL (slot0==slot1/pid0==pid1)` 或直接 panic，见 §4）。

### 3.2 check→block 窗口的 lost wakeup（bug 2）

**现象/复现**：`waitpid`、`pipe_read`、`sys_getchar_block`、
`port_accept_block` 都是“先检查条件，再 `proc_block()`”。旧
`sched_wake()` 只对 `state==BLOCKED` 入队；若唤醒在检查之后、`state=BLOCKED`
之前到达，它看到的是 `RUNNING`，唤醒被直接丢弃，随后线程永久睡眠。
自检 `audit: smp wake-vs-block` 把目标精确钉在窗口里：目标在 CPU0 自旋
（RUNNING）并置 `lw_ready`，CPU1 的 orchestrator 调 `proc_wakeup(chan)`
后才放行目标进入 `proc_block()`。旧实现必丢（超时 FAIL），修复后
`latency=0 ticks`。

**修复**：
- `sched_wake()`：`FREE/ZOMBIE` 忽略；`BLOCKED` 走原来的入队/延迟补投；
  `READY/RUNNING` 只置 `wake_pending`（不再把正在别的 CPU 上跑的进程
  “摘掉再入队”）。
- `proc_wakeup()` 对 RUNNING 进程也调用 `sched_wake()`（latch 该次唤醒）。
- `proc_block()` 通过 `sched_block_begin()`（持 `sched_lock`）先消费
  latch，有就直接返回让调用方重新检查条件；否则置 `BLOCKED`。
  这样“waker 先拿锁 → blocker 看到 latch”或“blocker 先拿锁 → waker 看到
  BLOCKED”两者必居其一，唤醒不会丢。

**回归**：`audit: smp wake-vs-block PASS (latency=0 ticks)`。

### 3.3 proc_block 的 hlt 分支不重新入队 → 永久饿死（bug 3）

**现场**：`audit: mm vmalloc concurrent`（其实是任意的 2 线程 + 睡眠负载）
间歇性 `worker timeout`；超时转储：

```text
audit: dbg vm-timeout pid=17 state=1 eff=3 base=3 pin=0 wp=0 chan=0x0 bit=0x10000 mask=0/28080
sched: dbg slot=16 unlink_ready=44 last_unlink_state=3 ... (link_seq=1088 unlink_seq=1071)
audit: mm vmalloc concurrent FAIL (worker timeout ack0=0 ack1=3 iters=1200 ... w0state=1)
```

worker 是 `PROC_READY`，但 bit 16 不在 cpu0/cpu1 任何 `runq_mask` 里，CPU0
仍有 tick（不是关中断死锁）——即“READY 但永远选不到”。

**根因**：`proc_block()` 在“没有别的 READY”时走 `sti; hlt` 分支。唤醒方
`sched_wake()` 看到目标仍是 current，只置 `wake_pending`（正确，不能入队），
hlt 循环消费 latch 后**直接返回**，进程以 RUNNING 身份继续跑却仍在
`proc_block()` 开头被 `sched_dequeue()` 摘掉的位置。下一次时间片到期，
`sched_tick()` 把 cur 置 `READY` 后 `sched_next()` 在本 CPU 位图里找不到它；
若此时有别的 READY，就切走，该进程永远停留在 READY+未入队。旧代码在
`-smp 2` 上被本自检稳定命中（连续 10 次启动全部超时）。

**修复**：`proc_block()` 在恢复 `state=RUNNING` 后调用
`sched_enqueue(p, smp_cpu_id())`（unlink+link 幂等），保证
“RUNNING ⇒ 在队列里”。另在 `sched_tick()` 增加永久断言：RUNNING 的
current 若不在本 CPU 位图里，打印现场并
`panic("sched: RUNNING pid %u not linked on cpu%u")`，把饥饿变成可诊断
panic（BLOCKED 的 hlt 窗口不算违规）。

**回归**：`audit: mm vmalloc concurrent PASS (iters=2400)` 与
`audit: smp selftest PASS`；修复后连续 5 次启动 5/5 PASS。

### 3.4 vmalloc/vfree 无锁（bug 4）

**现象**：`vm_bitmap`、映射、`vfree` 的位图清理都是全局共享状态，旧实现
完全无锁。两颗 CPU 同时 `vmalloc()` 时 `vm_find_free()` 会返回同一
`start_page`：两个调用者把不同物理页映射到同一个虚拟地址（共享的内核
页表，后者覆盖前者），随后两次 `vfree()` 还会重复清位图/重复释放物理页，
表现为块头校验失败（`vmm: WARN`）、`pmm: WARN double free`、两个调用者
互相踩数据。内核 `mmap` 文件路径（`mm/filemap.c`）与 `memtest` 都会走
`vmalloc`。

**修复**：`mm/vmm.c` 增加 `vm_lock`，`vmalloc()` 的“查位图→分配/映射→
写块头→标位”与 `vfree()` 的“校验→清位图→释放/解映射”整体串行化；
错误/重复释放的 `vmm_warn` 放到解锁之后。锁序 `vm_lock → pmm_lock`，
无反向路径（pmm 不调用 vmalloc）。

**回归**：`audit: mm vmalloc concurrent PASS (iters=2400)`；自检同时断言
`vmm_faults`/`pmm_double_free` 无增量、两线程写入的 pattern 不被对方覆盖。

### 3.5 P0（转自 audit-syscall）：SIGRETURN 用户可伪造 iret 现场

`proc_sigreturn()` 旧实现把用户栈信号帧直接 `memcpy` 回 `interrupt_frame`
再 iret；用户可改 `cs=0x08`、`eip=内核地址`、`EFLAGS.IOPL=3`（或
NT/VM86），从 ring3 提权到 ring0/获得 I/O 权限。

修复（`proc/process.c`）：
- 新增 `proc_sigframe_valid()`：`cs==0x1B`、`ss==0x23`、`eip<0xC0000000`、
  EFLAGS.bit1=1，且只允许
  `CF|PF|AF|ZF|SF|TF|IF|DF|OF|AC|ID`（`0x00240FD7`），显式拒绝
  IOPL/NT/RF/VM/VIF/VIP；
- 用 `copy_from_user()` 读帧（#PF 修复表），先拷到内核临时帧校验通过后
  才写回现场；非法帧返回 -1 并打印，不污染现场、不 oops。

回归：`audit: sigreturn frame validation ok (user cs/ss, user eip, no iopl/nt/vm)`
（自检逐项构造 cs=0x08、ss=0x10、cs=0x3B、eip=0xC0001000、IOPL=3、NT、
VM86、缺 bit1，全部必须拒绝）。

### 3.6 P0（转自 audit-syscall）：copy_*_user 的用户地址窗口

原先只有 misc 组自己加了“下界”检查；其它 syscall 组仍可把内核低地址
（如 0x00100000 恒等映射）当用户缓冲传给 `copy_*_user`，读/写内核内存。

修复（`proc/trap/uaccess.c`）：`uaccess_addr_ok(addr,len,user_ctx)` 在
uaccess 层统一：
- 任意上下文：`addr < 0xC0000000` 且 `addr+len` 不回绕；
- 用户上下文（`p->is_user`）：还要求 `addr >= 0x40000000`；
- 内核线程/启动自检（`p==0 || !p->is_user`）保留低地址例外
  （`bootseq.c` 的 `syscall_selftest` 用内核字符串调 `SYS_WRITE`）。

回归：`audit: uaccess user window ok (kernel-low/3gb/wrap rejected)`。

### 3.7 P0（转自 audit-syscall）：SYS_KILL 无 uid 校验

旧实现按 pid 查表即发信号，普通用户可 kill root/其它用户进程。
修复（`proc/proc_syscall.c`）：新增 `proc_can_signal(from,to)`——发送者
`euid==0`、目标是自己、或目标 uid/suid 与发送者 uid/euid 匹配才允许；
否则 SYS_KILL 返回 -1。
回归：`audit: kill uid policy ok (root/self/same-uid/other)`。

### 3.8 追加：tcptest 偶发卡死 = 同一个“RUNNING 未入队”饥饿（bug 3）

audit-net 报：启动负载下 `tcptest` 偶发卡住，60s 内既没有
`net: tcp selftest PASS` 也没有 `proc: pid .. (tcptest) exited`；QMP 现场
曾见 CPU0 在 `memset` 且 `EFL.IF=0`。本会话用 2 核、带宿主 TCP/UDP echo
（45445）的启动循环复现/验证：

- **修复前（基线 `38161af`）**：tcptest 会卡在 `timer_ticks()` 驱动的 RTO/accept
  等待里；缺少 PASS/exit 行（见 `obj/audit/base-repro.out` 的 A/B 记录）。
- **根因不是长 memset/IF 丢失**：`memset` 的关中断调用点只有 pmm 毒化
  （单页 4KB，AP 栈启动期 16 页）和 vmalloc 块，量级是微秒；`proc_block` /
  `proc_switch_to` 的 IF 保存恢复成对（入口 pushfl → 出口 popfl；新线程由
  `thread_trampoline` `sti`；中断里切换由 iret 恢复被打断的 EFLAGS），没有
  找到 IF=0 泄漏。
- **真正原因**：tcptest 在 `proc_sleep_ticks(1)`/accept 等待中 `proc_block`
  时，如果 CPU 上没有别的 READY 会进 `sti;hlt` 分支；被 tick 唤醒后（唤醒方
  只置 `wake_pending`）旧代码直接返回，进程以 RUNNING 身份继续跑却**不在
  任何 runq_mask 里**（§3.3）。随后时间片到期被置 READY，`sched_next()`
  永远选不到它；tcptest 的所有超时都依赖 `timer_ticks()`，而它自己再也跑
  不到检查超时的那一行 → 表现为“PASS/exit 行消失”，且 BSP 仍在正常跑
  （所以看门狗不炸）。QMP 采样到 CPU0 在 memset/IF=0 只是那一瞬间 BSP 正在
  做 PMM 毒化，不是根因。
- **验证（A/B）**：同一脚本（2 核、宿主 TCP/UDP echo、每次启动等 60s 看
  `tcptest exited`）：
  - 基线 `38161af`（隔离 worktree，构建 38161af 自己的 -Test 镜像）：
    **10 次命中 1 次（10%）**。失败现场（`obj/audit/base-repro.out` run 3）：
    `tcptest_exit=False tcp_pass=False heartbeats=87`，QMP `info registers`：
    `CPU#0 ... EFL=00000246 [IF=1] HLT=1 CR3=019f7000`——CPU0 停在
    `proc_block()` 的 `sti;hlt` 分支里（current=某进程、页目录还是进程的），
    不是 memset/IF=0，也不是 PIT 停摆（BSP ticks 在走、AP 心跳到 87）。
  - 修复后同一脚本：**20/20 次 60s 内看到 PASS + exit（0% 命中）**
    （`obj/audit/tcp-repro2.out` 10/10、`obj/audit/tcp-repro3.out` 10/10）。
- **看门狗盲区**：`progress_now() = timer_ticks() + syscall_count()`，SMP 下
  AP 的 syscall 会掩盖 BSP PIT 停摆。`dev/intc/apic.c` 新增 BSP tick 停摆
  检查：NMI（由 BSP 自己的 LAPIC 定时器投递，不受 IF 影响）连续 8 次
  （~2s）看到 `per_cpu[0].ticks` 不前进就 panic，打印 eip/global ticks/
  syscall 数，把“BSP 关中断锁死但 AP 还在跑”从静默 hang 变成可诊断 panic。

### 3.9 P0（0.74 浸泡发现）：guest tick = 墙钟 2 倍（PIT mode 3 + IOAPIC 边沿双计数）

**现象**：v0.74.0 镜像启动后 guest tick 约 2 倍：179s 墙钟 uptime 到 300s，
`thread: heartbeat` ~1.9–2.0/s（心跳周期 100 tick）。0.73 为 1:1 且没有
`ioapic: irq0` 行。

**现场（QMP + 内核探针）**：

```text
ioapic0 ... redir[24]
  pin 2  0x0000000000000020 dest=0 vec=32 active-hi edge unmasked fixed physical
  （其余 pin 全部 masked 或 vec=0x21/0x24/0x2b/0x2c，没有第二个 vec=0x20）
pic0: irr=03 imr=ff isr=00      ← PIC 全屏蔽，PIT 在 IRR 里挂着但不投递
LAPIC LVT0 = 0x00008700 ExtINT active-hi level（未屏蔽）
LAPIC LVT1 = 0x00008400 NMI；LVTT = 0x00020400 periodic NMI
```

RTC 墙钟探针（每次 4 个 CMOS 秒）：

| 实验 | ticks/4s | 结论 |
|---|---|---|
| 原样（IOAPIC + PIC） | 783–797 (~199/s) | 2x |
| 屏蔽 LAPIC LINT0 | 797 | PIC/ExtINT 不是来源 |
| `irq_mask(0)`（PIC+IOAPIC 都关） | 0 | 没有其它 tick 源 |
| 只关 IOAPIC（PIC 保持开） | 0 | QEMU 里 PIC 路径本身不投递 |
| PIT 改 mode 2 后原样 | 394–400 (~99/s) | 1:1 |

**根因**：`pit_init()` 用 **mode 3（方波）**。方波每周期 OUT 翻转两次；
8259 的边沿检测只认上升沿（0.73 PIC 路径 1:1），而 IOAPIC 的 edge 重定向
把两次电平变化都当成一次触发 → 100Hz PIT 产生 ~200 次向量 0x20。
`irq_count(0)` 与 `timer_ticks()` 增量完全相等，说明就是同一个向量被投递
两次，而不是 ticks 被多写。

**修复**（`proc/trap/irq.c`）：PIT 通道 0 改用 **mode 2（rate generator，
每周期一个脉冲）**：`outb(PIT_CMD, 0x34)`。两种投递路径下都恢复 1 次/周期。
**回归自检行**（`timer_handler`，tick 200 与 1400 各读一次 RTC）：

```text
irq: pit rate ok (1200 ticks / 14 s)      ← 2x 回归时是 ~6s，打印 FAIL
```

（容差 8..18s：TCG/CI 负载高时定时器投递会滞后，实测 13.6s；2x 的 ~6s 远离下界。）

**验证**：
- 修复后 `soak: uptime=60 s` 在 QEMU 启动后 69.3s 出现（boot ~9s）→ 1:1；
  `smp: tick check after 2s` 的 AP tick=219（0.74 坏版本是 348）。
- `-smp 2` 全量 `build.ps1 -Test`：**247/247 PASS**（含 smp 审计 8 项）。
- `-smp 4` 同一套检查：241/247，唯一 6 项失败仍是 §5 列出的“2 核写死字符串”
  （`madt: 2 cpu`、`2 cpu(s) discovered`、`2/2 cpus online, 1 AP`、
  `smp: 2 cpus online`、`ack=1/1`、`spin running on cpu 1`），无新增失败；
  4 核自检 PASS、`audit: smp selftest start (cpus=4)`。

## 4. 复现（负向）与证据

负向构建 = 临时还原某一条旧逻辑，其余修复保留，用同一个自检跑 2 核启动；
证明每条 FAIL 只由对应 bug 触发（而不是测试本身不稳定）。

- **bug 3（静默饿死）**：修复前 10/10 次启动在 `audit: mm vmalloc concurrent`
  超时；保留的现场日志 `obj/audit/smp-run-1..9.log` 有：

  ```text
  audit: dbg vm-timeout pid=17 state=1 ... bit=0x10000 mask=0/28080
  audit: mm vmalloc concurrent FAIL (worker timeout ack0=0 ack1=3 iters=1200 ... w0state=1)
  ```

  `state=1`（READY）+ bit 不在任何 `runq_mask`，CPU0 仍有 tick → 不是关中断
  死锁，而是“READY 但永远选不到”。修复后同脚本 5/5 PASS。
- **bug 2（lost wakeup）**：临时去掉 `proc_wakeup()` 对 RUNNING 的 latch 后：
  `audit: smp wake-vs-block FAIL (lost wakeup, waited=600 ticks)`，同一次启动
  里 spawn race / vmalloc 仍 PASS；恢复修复后 PASS（`latency=0 ticks`）。
  现场：`obj/audit/neg-wake.log`。
- **bug 1（槽位/pid 竞态）**：临时把“占位+pid 分配”挪回 `malloc()` 之后
  （即旧窗口）：

  ```text
  audit: smp spawn race FAIL (reason=1 rounds=2 timeouts=0 slot0=22 slot1=22 pid0=29 pid1=29)
  ```

  `reason=1` = 两边拿到同一个 `proc_t` 槽位；同一行也显示同一个 pid
  （`next_pid++` 丢更新）。同次启动其它两项 PASS；恢复修复后
  `PASS (rounds=24)`。现场：`obj/audit/neg-spawn.log`。

## 5. 回归与提交

- 2 核：`build.ps1 -Test`（独立 `-EchoPort 45445`，不占 12345）
  **225/225 PASS**；自检行：
  ```text
  audit: sigreturn frame validation ok (user cs/ss, user eip, no iopl/nt/vm)
  audit: kill uid policy ok (root/self/same-uid/other)
  audit: uaccess user window ok (kernel-low/3gb/wrap rejected)
  audit: smp selftest start (cpus=2)
  audit: smp spawn race PASS (rounds=24)
  audit: smp wake-vs-block PASS (latency=0 ticks)
  audit: mm vmalloc concurrent PASS (iters=2400)
  audit: smp selftest PASS
  audit: smp selftest done
  ```
- 4 核：`tools/audit-smp-run.ps1 -Smp 4`（独立镜像 `.dist-audit-smp4`），
  自检同样 PASS，`audit: smp selftest start (cpus=4)`。
- 4 核 12 分钟浸泡：`tools/audit-smp-soak.ps1 -Minutes 12`（QMP 注入
  `sectest/fork/linktest` 循环，4 核 fork/exit/mmap/signal 压测）：
  `heartbeat_lines=503`、`sectest: ` 行 370、13 个 sectest 块、**0 panic /
  0 oops**、`audit: smp selftest PASS`；`obj/audit/smp4-soak.log`。
  说明：没有宿主回显服务器时 `sectest: tcp` 必失败；同一 shell 反复 `sectest`
  到第 4 次后 `SYS_MMAP` 会因该进程 4 个 mmap 槽位耗尽而失败（sectest 自己不
  munmap），随后 fs/blk 服务连接也因同一进程的 fd/连接状态退化——这是测试
  程序资源不释放，不是内核 panic/hang，与本次并发修复无关，记录为已知现象。
- `build.ps1` 的 225 项断言里有 6 条写死 2 核字符串（`madt: 2 cpu`、
  `2 cpu(s) discovered`、`2/2 cpus online, 1 AP`、`smp: 2 cpus online`、
  `ack=1/1`、`sched: pid N (spin) running on cpu 1`），所以 4 核跑同一套
  检查**按设计**是 **219/225**（其余 219 项全 PASS，含 audit 自检/TCP/pkg/
  fs/mm）。4 核用自检 + 12 分钟浸泡 + `tests/audit/smp/check.ps1` 验证
  （`audit: smp selftest start (cpus=4)` / `smp: 4/4 cpus online` /
  `tlb shootdown ack=3/3 ok`），现场 `obj/audit/test-smp4full.log`。
- 提交：
  - `f1e05d9` `[audit-smp] SMP concurrency fixes: slot/pid race, lost wakeup,
    unlinked RUNNING starvation, vmalloc lock; sigreturn/kill/uaccess P0s +
    selftests`（16 files，含本文件、内核修复、`tests/audit/smp/`、两个
    `tools/audit-smp*.ps1`）
  - `43b0720` `[audit-smp] NMI watchdog BSP tick-stall check; docs: tcptest
    A/B 10%->0% root cause, build.ps1 check lines, tcp repro script`
  - `68183ba` `[audit-smp] docs: final regression numbers ...`
  - `ea5da7a` `[fix] intc: PIT mode 2 (one pulse/period) - v0.74 tick was 2x
    after IRQ0 moved to IOAPIC; add 'irq: pit rate ok' selftest`（§3.9）

## 5.1 可直接并入 `build.ps1` 的检查项（给父会话）

正例（全部在 `-Test` 的 2 核/4 核启动里出现；建议加 8 条）：

```powershell
@{ Name='sigreturn 帧校验';     Ok = ($out -match 'audit: sigreturn frame validation ok \(user cs/ss, user eip, no iopl/nt/vm\)') },
@{ Name='kill uid 策略';        Ok = ($out -match 'audit: kill uid policy ok \(root/self/same-uid/other\)') },
@{ Name='uaccess 用户窗口';     Ok = ($out -match 'audit: uaccess user window ok \(kernel-low/3gb/wrap rejected\)') },
@{ Name='SMP spawn 竞态';       Ok = ($out -match 'audit: smp spawn race PASS \(rounds=\d+\)') },
@{ Name='SMP wake-vs-block';    Ok = ($out -match 'audit: smp wake-vs-block PASS \(latency=\d+ ticks\)') },
@{ Name='vmalloc 并发';         Ok = ($out -match 'audit: mm vmalloc concurrent PASS \(iters=\d+\)') },
@{ Name='SMP 自检总判定';       Ok = ($out -match 'audit: smp selftest PASS') },
@{ Name='SMP 自检收尾';         Ok = ($out -match 'audit: smp selftest done') },
```

负向（只在临时还原旧代码时出现，**不要**作为 build.ps1 的期望行；用于确认
自检的判别力，现场已存 `obj/audit/neg-*.log`、`obj/audit/base-repro.out`）：

```text
audit: smp spawn race FAIL (reason=1 ... slot0=22 slot1=22 pid0=29 pid1=29)
audit: smp wake-vs-block FAIL (lost wakeup, waited=600 ticks)
audit: mm vmalloc concurrent FAIL (worker timeout ack0=0 ack1=3 iters=1200 ... w0state=1)
（基线 38161af，tcptest 60s 无 PASS/exit）run 3: tcptest_exit=False tcp_pass=False heartbeats=87
```

注意：`audit:` 前缀与 `acct:`/`fs:` 审计行不冲突；若父会话只想在 `-smp 2`
检查，以上 8 条在 2 核上全部出现（4 核同样出现，`cpus=4`）。

## 6. 未修项 / 已知边界

1. **信号打断阻塞 syscall**：`proc_send_signal()` 只置 `pending_sig` 并
   `sched_wake`；阻塞在 accept/read 等内核循环里的用户进程不会返回用户态，
   `proc_check_signals()` 没有机会执行（SIGKILL 也不能立刻杀死它）。
   正确做法是让这些循环返回 `EINTR`，但涉及 `fs/**`、`net/**`（本会话
   不可改）；目前只在 `waitpid` 等 `proc/**` 路径可以后续补。
2. **`sched_tick` 时间片窗口**：`cur->state=READY` 与 `sched_next()` 之间
   不持 `sched_lock`。本次已让 `sched_wake()` 对 READY/RUNNING 只记 latch
   （不再迁移/入队），消除了“正在跑的进程被另一颗 CPU 入队”的主要放大面；
   但 `sched_balance` 仍可能在窗口里把 cur 当 READY 迁移（仅 BSP、需要
   恰好 20 tick 对齐）。彻底修法是把“置 READY + 选下一个”放进同一
   `rq_lock+sched_lock` 临界区。
3. **`sched_deliver_pending` 忽略 `cpu_pin`**：补投总是入队到执行补投的
   CPU，可能让亲和进程短暂跑在别的核上（语义问题，不会双跑）。
4. **AP bring-up 的暂存页**：BSP 以 `smp_cpu_count` 增量判断“AP 已登记”，
   但 AP 登记后还要读 `AP_DATA_PA`（栈/CR3/入口）。BSP 若在 AP 读完前
   开始下一个 AP 会覆盖暂存页（当前靠 INIT-SIPI 的固定延时规避，非保证）。
5. **TLB shootdown 只在启动自检使用**：当前“一个进程同时只跑在一颗 CPU
   上”，用户页表修改后靠 CR3 切换/invlpg 即可；若将来支持同地址空间多
   CPU，需要在 `mprotect/munmap` 后广播（`smp_tlb_shootdown()` 已有，
   但 ack 语义只是 IPI 往返）。
6. **vmalloc 虚拟区间与进程页目录**：`vmalloc` 的映射落在“当前进程”的
   内核 PDPT 项；所有进程共享同一份内核 PD/PT，因此映射是全局的。若将来
   改成每进程独立的 kernel PD，需要同步所有地址空间。
7. 未做 x2APIC/CPU 热插拔/MSI；`SMP_MAX_CPUS=4`。
