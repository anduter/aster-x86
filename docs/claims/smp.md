# smp 会话声明（P1 多核扩展）

日期：2026-10-27　分支：`main`（基线 `79f5c66`）　会话：`smp`

## 1. 改动范围

只改归属路径：

| 文件 | 内容 |
|---|---|
| `src/kernel/proc/sched.c` | 全局 `sched_lock` 拆成 per-CPU `rq_lock[4]` + 全局状态锁；`sched_wake/sched_balance/sched_tick/sched_deliver_pending` 适配；AP 看门狗覆盖全部 AP；新增锁/runqueue 自检 |
| `src/kernel/proc/process.c` | `proc_block()` 阻塞时把进程从 runqueue 摘掉（配合 wake_pending 单点补投） |
| `src/kernel/proc/smp.c` | AP bring-up 等待全部 AP（不只 cpu1）；每颗 AP 独立重装 LAPIC timer；TLB shootdown ack 校验；`smp: N cpus online` 自检行 |
| `src/kernel/proc/trap/irq.c` | 传统 IRQ 全量迁 IOAPIC（PIT/键盘/串口/鼠标/网卡），失败逐条回退 PIC；电平触发补 IOAPIC EOI；IRQ 掩码同步 IOAPIC |
| `src/kernel/proc/trap/interrupt.h` | 声明 `irq_migrate_all_to_ioapic()` |
| `src/kernel/dev/intc/madt.c/.h` | 解析 MADT Interrupt Source Override（IRQ0→GSI2 等），打印映射 |
| `src/kernel/dev/intc/ioapic.c/.h` | 重定向表支持电平/低有效；新增 IOAPIC EOI |

未改 `build.ps1`、`include/abi.h`、`mm/**`；未新增 syscall。

## 2. 锁拆分与锁顺序

旧实现：一把全局 `sched_lock` 同时保护所有 CPU 的 runqueue 和进程状态。

新实现：

- `rq_lock[c]`（每 CPU 一把）：保护 `per_cpu[c].runq_mask` 与
  `sched_bucket[c][prio]` 的归属；本 CPU 的 tick/选进程只拿自己的锁。
- `sched_lock`（全局，保留）：保护 `proc_t` 的
  `state / eff_prio / base_prio / wait_ticks / wake_pending / runs / last_cpu`
  以及 `proc_switch_from[]`、`sched_aging_ticks`。

**严格锁顺序**：

```
rq_lock[*]（需要多把时按 CPU 号升序）  ->  sched_lock
```

- 单 CPU 路径：`rq_lock[c] -> sched_lock`（`sched_next` / aging / pending 补投）。
- 跨 CPU 路径（wake / enqueue / balance / clear_slot / set_priority）：
  `rq_lock_all()`（c=0..3 升序）`-> sched_lock`。
- **没有**任何路径在持有 `sched_lock` 时回头拿 `rq_lock`，因此不存在
  global→rq 的反向边；多把 rq 锁固定升序，也不会出现 rq 之间的 ABBA。
- 负载采样（`sched_place`）只逐把拿目标 CPU 的 `rq_lock` 读位图，不阻塞其它核。

`proc_block()` 现在在置 `BLOCKED` 后调用 `sched_dequeue()`：阻塞进程不再留在
runqueue 位图里，`sched_deliver_pending()` 才能安全地“只往当前 CPU 入队”，
避免同一进程同时出现在两颗 CPU 的队列里（共享内核栈会并发执行）。

## 3. 新增 selftest 行（期望断言）

### 2 核默认（`-Test`，build.ps1 仍用 `-smp 2`）

```text
smp: 2 cpus online (LAPIC ready, per-cpu runqueues)
smp: cpu0 apic=\d+ ticks=\d+ ipi=\d+ tlb=\d+ runq=0x[0-9a-f]+ \(bsp\)
smp: cpu1 apic=\d+ ticks=\d+ ipi=\d+ tlb=\d+ runq=0x[0-9a-f]+
smp: tlb shootdown ack=1/1 ok
smp: 2/2 cpus online, 1 AP GDT/TSS/IDT ready, AP ticks=[1-9]\d* ipi=[1-9]\d* tlb=[1-9]\d*
smp: tick check after 2s bsp=\d+ ap=\d+ ap2=0 ap3=0 \(online=2\)
sched: per-cpu locks ok \(nr=2, order=rq->global\)
sched: runqueue cpu0 mask=0x[0-9a-f]+
sched: runqueue cpu1 mask=0x[0-9a-f]+
madt: irq0 -> gsi2 flags=0x0
ioapic: irq0 -> gsi2 apic 0 vector=0x20 edge \(PIC masked, LAPIC EOI\)
ioapic: legacy IRQ routing done \([1-9]\d* irq\(s\) via IOAPIC, PIC fallback armed\)
```

> 保留的旧断言 `smp: 2/2 cpus online, 1 AP GDT/TSS/IDT ready, ...` 未改格式。
> 新行可直接由父会话加入 `build.ps1` 的 checks（建议取前 8 行里有区分度的 4~6 条）。

### 4 核手工（`qemu -smp 4`）

```text
smp: 4 cpus online (LAPIC ready, per-cpu runqueues)
smp: cpu2 apic=\d+ ticks=[1-9]\d* ipi=[1-9]\d* tlb=[1-9]\d* runq=0x[0-9a-f]+
smp: cpu3 apic=\d+ ticks=[1-9]\d* ipi=[1-9]\d* tlb=[1-9]\d* runq=0x[0-9a-f]+
smp: tlb shootdown ack=3/3 ok
smp: 4/4 cpus online, 3 AP GDT/TSS/IDT ready, AP ticks=[1-9]\d* ipi=[1-9]\d* tlb=[1-9]\d*
smp: tick check after 2s bsp=\d+ ap=\d+ ap2=[1-9]\d* ap3=[1-9]\d* \(online=4\)
sched: per-cpu locks ok \(nr=4, order=rq->global\)
sched: runqueue cpu0 mask=0x[0-9a-f]+
sched: runqueue cpu1 mask=0x[0-9a-f]+
sched: runqueue cpu2 mask=0x[0-9a-f]+
sched: runqueue cpu3 mask=0x[0-9a-f]+
sched: pid \d+ \(spin\) running on cpu 2
sched: pid \d+ \(spin\) running on cpu 3
```

## 4. `-smp 4` 手工验证

构建测试镜像（测试模式带 `autologin=root`；`build.ps1` 不提供 `-smp` 参数，
所以先用 `-Test` 构建出镜像，再手工起 QEMU）：

```powershell
# 1) 用现有 -Test 产物（floppy/disk/fat）复制一份，避免影响默认 2 核回归
Copy-Item .dist-smp\floppy.img .dist-smp4t\
Copy-Item .dist-smp\disk.img   .dist-smp4t\
Copy-Item .dist-smp\fat.img    .dist-smp4t\

# 2) 手工 4 核启动（QMP 4513，串口写 serial-smp4.log）
qemu-system-i386 -fda .dist-smp4t/floppy.img `
  -drive file=.dist-smp4t/disk.img,format=raw,if=ide,index=0 `
  -drive file=.dist-smp4t/fat.img,format=raw,if=ide,index=1 `
  -boot a -m 256 -vga std -cpu qemu32,+nx,+smep,+smap -smp 4 `
  -nic user,model=e1000 -display none -no-reboot `
  -serial file:serial-smp4.log -qmp tcp:127.0.0.1:4513,server,nowait
```

实测（`serial-smp4.log`，2026-10-27）：

```text
smp: BSP online (apic id=0), 4 cpu(s) discovered, AP startup pending
smp: 4 cpus online (LAPIC ready, per-cpu runqueues)
smp: cpu0 apic=0 ticks=0 ipi=0 tlb=0 runq=0x0 (bsp)
smp: cpu1 apic=1 ticks=227 ipi=1 tlb=1 runq=0x0
smp: cpu2 apic=2 ticks=133 ipi=1 tlb=1 runq=0x0
smp: cpu3 apic=3 ticks=40 ipi=1 tlb=1 runq=0x0
smp: tlb shootdown ack=3/3 ok
smp: 4/4 cpus online, 3 AP GDT/TSS/IDT ready, AP ticks=231 ipi=1 tlb=1
smp: tick check after 2s bsp=200 ap=348 ap2=253 ap3=160 (online=4)
sched: per-cpu locks ok (nr=4, order=rq->global)
sched: runqueue cpu0 mask=0x12211
sched: runqueue cpu1 mask=0x4a2
sched: runqueue cpu2 mask=0x4844
sched: runqueue cpu3 mask=0x9108
sched: pid 3 (svcwatch) running on cpu 2
sched: pid 4 (fsd) running on cpu 3
sched: pid 15 (spin) running on cpu 2
sched: pid 16 (spin) running on cpu 3
```

结论：

- 4/4 CPU online，3 个 AP 都装好 GDT/TSS/IDT，LAPIC timer 全部在走
  （2s 时 `bsp=200 ap=348 ap2=253 ap3=160`）；
- 四颗 CPU 的 runqueue 位图都非零（进程被负载放置到 cpu0..3）；
- TLB shootdown 3/3 ack；调度器把 spin 线程实际跑在 cpu2/cpu3；
- 心跳线程持续打印 `thread: heartbeat 0..19 pid=8 cpu=1`（每秒递增，
  四核运行期间未停），说明时钟/调度/用户态入口都活着；
- 进入用户态 shell 后 `ushell: ready (ring3, legacy int 0x81)` 正常出现，
  键盘注入 `help` 有响应（`ushell: commands`）。

## 5. IOAPIC 全量路由

- `madt.c` 新增 Interrupt Source Override 解析：QEMU 下
  `irq0 -> gsi2`（PIT）、`irq5/9/10/11 -> gsi* flags=0xd`（PCI 电平低有效）。
- `smp_start_aps()` 在 AP 全部就绪后调用 `irq_migrate_all_to_ioapic()`：
  对每个已注册处理函数的传统 IRQ 执行迁移，目标默认 BSP；
  键盘 IRQ1 随后仍由既有 bootseq 逻辑改投 AP。
- 迁移顺序：先屏蔽 PIC 该线 → 编程 IOAPIC 重定向表 → 成功才保持屏蔽；
  任一条失败只回滚该条并打印 `stays on PIC (fallback)`。
- 电平触发中断在 `irq_dispatch()` 里先写 IOAPIC EOI（寄存器 0x40）再发 LAPIC EOI；
  `irq_mask/unmask()` 对已迁移的线同时操作重定向表。
- 跳过 IRQ2（8259 级联线在 IOAPIC 模式下就是 PIT 的 GSI2）；全部设备迁移成功时
  才掩掉 PIC 级联线，否则保留从片回退路径。

实测 2 核日志：

```text
madt: irq0 -> gsi2 flags=0x0
ioapic: irq0 -> gsi2 apic 0 vector=0x20 edge (PIC masked, LAPIC EOI)
ioapic: irq1 -> gsi1 apic 0 vector=0x21 edge (PIC masked, LAPIC EOI)
ioapic: irq4 -> gsi4 apic 0 vector=0x24 edge (PIC masked, LAPIC EOI)
ioapic: irq11 -> gsi11 apic 0 vector=0x2b level (PIC masked, LAPIC EOI)
ioapic: irq12 -> gsi12 apic 0 vector=0x2c edge (PIC masked, LAPIC EOI)
ioapic: legacy IRQ routing done (5 irq(s) via IOAPIC, PIC fallback armed)
```

## 6. 回归结果

- `powershell -NoProfile -File tools\session-run.ps1 -Name smp -Test`
  （默认 `-smp 2`，提交 `647f8a6` 之后复跑，2026-10-27 02:2x）：
  **TEST PASS (182/182)**，`exit=0`，`elapsed=55.0s`。
- 提交前的共享树中间态曾为 181/182，唯一失败是 pkg 会话未提交的组件
  ELF-entry 改动（`pkg: reinstall service online ok` 行尚未恢复）；pkg 提交
  `15d15b0` 后本会话复跑即 182/182，说明该失败与调度/中断改动无关。
- 本会话改动直接相关的断言全部 PASS：SMP 2/2 online、TLB ack、per-cpu
  locks、runqueue 位图、sched priority/aging、heartbeat、fork/COW、shell 与
  文件系统回归。
- 隔离验证：另在 `git worktree`（HEAD `79f5c66` + 本会话 9 个文件）上跑过
  同一套 `-Test`，调度/SMP/内核自检同样 PASS；该隔离树缺少其它会话未提交的
  fs 修复，`/bin/shell` 验签走内核 shell 回退，故不作为 182 计数依据。

## 7. 遗留 / 未做

- 未启用 x2APIC（仍用 xAPIC MMIO，32 位 APIC ID）；
- 不支持 CPU 热插拔 / offline，AP 启动固定 INIT-SIPI-SIPI + 延时；
- `SMP_MAX_CPUS = 4`（`per_cpu[]`、`rq_lock[]` 静态数组上限），>4 核需扩数组；
- 只解析/使用 MADT 里第一个 IOAPIC；未做 MSI/MSI-X 路由；
- 传统 IRQ 的 PCI 电平触发已支持 IOAPIC EOI，但 PCI 设备仍走 IRQ 线，未改 MSI；
- `build.ps1` 默认仍是 `-smp 2`；4 核为手工验证，未改测试默认真实硬件/GUI 未涉及。
