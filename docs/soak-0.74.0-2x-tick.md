# v0.74.0 浸泡作废记录 — 2x tick（IRQ0 迁 IOAPIC 后重复投递）

## 结论（先看）

**本轮 0.74.0 8 小时浸泡作废，不计入 release 证据。** 原因不是崩溃/停滞，而是**计时口径错误**：

- v0.74.0 镜像的 guest tick 约为**墙钟的 2 倍**（实测 1.90–2.00x）；
- 心跳从 1 次/s 变为 **≈2 次/s**，`soak: uptime` 每 **30 s 墙钟**推进 60 guest-s；
- 0.73.0 镜像同机同参数为 **1.00x**，是 0.74 新增的 `irq_migrate_all_to_ioapic()` 把 **IRQ0 迁到 IOAPIC GSI2 后出现的重复投递**；
- 已转 **audit-smp** 修复；修复 + `ci -Full` 247/247 后，用新镜像**重起完整 8 h**（本轮不作为 PASS/FAIL 依据）。

本轮唯一可用的信息：**2x tick 本身没有造成内存/进程/崩溃问题**——`free_pages=26487`、`procs=23`、`faults=1` 全程恒定，`CRASH.txt` 不存在，panic/oops/triple fault/STALL 均为 0。

## 构建与冒烟

| 项 | 值 |
|---|---|
| HEAD / tag / 版本 | `ac4f72f` / `v0.74.0` / 0.74.0 |
| `git status --porcelain` | 空（构建前） |
| 构建 | `build.ps1` 普通构建，**20.2 s**，kernel **383,232 B** |
| 镜像 | `dist/floppy-run.img`，1,474,560 B，2026-10-27 **04:03:46** |
| 镜像版本串 | `kernel: x86 OS v0.74.0 (build Oct 27 2026 04:03:46)` |
| 冒烟 | `soak-file.ps1 -Hours 0.05 -Smp 2 -SerialLog obj/soak-file/smoke-0.74.log`，180.4 s，**323 心跳**（0→322，无跳号/重复），32,249 B，无 CRASH |

> 环境注记：`dist/floppy.img` 被**并行运行的 0.73.0 8h 浸泡**（QEMU PID 13944）占用，`build.ps1` 自动回退写出 `dist/floppy-run.img`；本轮浸泡用后者。0.73 浸泡全程未动，仍在运行。

## 现象

1. **冒烟就异常**：179 s 墙钟内串口出现 `uptime=60/120/180/240/300` 五行、323 条心跳——按 1:1 应只有 `uptime=60/120` 和 ~170 条心跳。
2. **专项探测确认 2.00x**（`obj/soak-file/probe-clock-0.74.out`）：

| 墙钟 (s) | 心跳计数 | 已出现的 uptime 行 | 串口字节 |
|---|---|---|---|
| 2.0 | 0 | （启动中） | 92 |
| 42.1 | 48 | 60 | 22,446 |
| 72.1 | 107 | 60/120 | 24,573 |
| 102.2 | 167 | 60/120/180 | 26,788 |
| 132.2 | 226 | 60/120/180/240 | 28,967 |

   稳态**每 30.0 s 墙钟 +60 guest-s**，心跳稳态 ≈1.98/s。心跳周期 `HEARTBEAT_PERIOD=100 tick`，1:1 时应为 1/s → 全局 `ticks` 实为 **200/s**。

3. **8 h 浸泡（已终止）同样 2x**：

| 项 | 值 |
|---|---|
| 启动 | 2026-10-27 **04:19:37**（QEMU PID 28404） |
| 人工终止 | 2026-10-27 **04:39:52**（墙钟 **1207 s**） |
| 串口 | `obj/soak-file/serial-8h-0.74.log`，**109,795 B** |
| 心跳 | **2,380** 条（0→2379） |
| soak 行 | **40** 条，`uptime=60,120,…,2400` |
| 比例 | 04:30 guest=1200s/wall=630s=**1.90x**；04:39 guest=2340s/wall=1207s=**1.94x**；末行 2400/1207=**1.99x** |
| 健康 | `free_pages=26487 procs=23 faults=1` 恒定；`CRASH.txt` 无；panic/oops/triple fault/STALL=0 |

## QEMU 现场（同一镜像）

`info irq`（两次采样相隔 60.76 s，IRQ0 计数 493→6570）：

```
delta = 6077 / 60.76 s = 100.0 次/s      ← PIT 线本身仍是 100 Hz，不是 200 Hz
IRQ statistics for ioapic / isa-i8259（两组计数相同）:
  0: 2498 (30s) → 3601 (40s)
  1/4/6/12/14: 少量（键盘/串口/floppy/鼠标/ATA 探测）
```

`info pic`：

```
ioapic0: ver=0x20 id=0x00 sel=0x12 (redir[1])
  pin 0   0x0000000000010000 dest=0 vec=0   active-hi edge  masked
  pin 1   0x0100000000000021 dest=1 vec=33  active-hi edge
  pin 2   0x0000000000000020 dest=0 vec=32  active-hi edge        ← PIT (IRQ0->GSI2)
  pin 11  0x000000000000802b dest=0 vec=43  active-hi level       ← e1000
  pin 12  0x000000000000002c dest=0 vec=44  active-hi edge        ← 鼠标
pic0: irr=03 imr=ff isr=00 hprio=0 irq_base=20 rr_sel=0 elcr=00 fnm=0
pic1: irr=10 imr=ff isr=00 hprio=0 irq_base=28 rr_sel=0 elcr=0c fnm=0
```

`info lapic`（CPU0）：

```
LVT0   0x00008700 active-hi level  ExtINT (vec 0)     ← LINT0 仍为 ExtINT 模式
LVTT   0x00020400 active-hi edge   periodic NMI       ← BSP 定时器只送 NMI 看门狗
SPIV   0x000001ff APIC enabled, spurious vec 255
```

启动日志（0.74 独有）：

```
madt: irq0 -> gsi2 flags=0x0
ioapic: base=0xfec00000 id=0 version=0x20 entries=24 (PIC retained)
ioapic: irq0 -> gsi2 apic 0 vector=0x20 edge (PIC masked, LAPIC EOI)
ioapic: legacy IRQ routing done (5 irq(s) via IOAPIC, PIC fallback armed)
```

**判读**：8259 已全屏蔽（`imr=ff`），PIT 线仍是 **100 次/s**，但 guest `ticks` 为 **200/s**——即每次 IRQ0 被**处理了两次**。0.73.0 的启动日志没有 `ioapic: irq0` 行（IRQ0 留在 8259），同参数下 1.00x，因此问题由 0.74 新增的 IRQ0→GSI2 迁移引入。具体重复路径（IOAPIC GSI2 投递 + ExtINT/8259 另一路，或其它）由 audit-smp 定位。

## 影响面（为什么必须作废）

- `uptime`、`sleep`、`TCP RTO`、心跳、`soak: uptime=…` 等所有**基于 tick 的计时**实际墙钟减半；
- 8 h 墙钟会被记成 ~16 h guest 时间，心跳/采样口径全部失真，不能作为“8 h 稳定”证据；
- 反过来，本轮未发现内存/进程泄漏或崩溃：`free_pages/procs/faults` 恒定、无 CRASH/panic/STALL。

## 现场保留与后续

| 文件（`obj/soak-file/`） | 说明 |
|---|---|
| `serial-8h-0.74.log` | 2x 现场串口，109,795 B，2,380 心跳，40 条 soak 行（已停止增长） |
| `serial-8h-0.74.out` | 本轮浸泡脚本输出（人工终止，exit 1） |
| `smoke-0.74.log` / `smoke-0.74.out` | 3 分钟冒烟 |
| `probe-clock-0.74.ps1` / `.out` / `.log` | guest vs 墙钟速率探测 |
| `probe-irq-count.ps1` | `info irq` 采样脚本 |
| `inspect-0.74.ps1` / `inspect-0.74.log` | 巡检 |
| `monitor-0.74.ps1` / `monitor-0.74.csv` | 20 min 采样器（本轮 1 个点后终止） |
| `build-0.74.log` | 普通构建日志 |
| `evidence-2x-tick.txt` | 本文全部原始证据汇总 |

**下一步（等指令）**：audit-smp 修复 + `ci -Full` **247/247** 后，重新 `build.ps1`（仍用 `dist/floppy-run.img`，避开被 0.73 占用的 `floppy.img`），确认镜像为修复后版本，再起新的 8 h 浸泡并重新写 `docs/soak-0.74.0.md`。本轮 `docs/soak-0.74.0.md` **不生成**。

## 约束遵守

- 未修改 `src/**`、`build.ps1`、`Makefile`、`tools/*`；
- 仅写 `obj/soak-file/**` 与本文件；提交仅限本文件；
- 未删除 `dist/`；未 `git add -A`；
- 浸泡期间未跑 `-Test`/flake-hunt/其它 QEMU（除速率探测的短时 QEMU，均使用独立 monitor 端口 4497–4502 并已全部结束）；
- 0.73.0 8h 浸泡（PID 13944）未动、仍在运行。
