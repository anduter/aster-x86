# 浸泡结果 — 0.74.1 发布前最终 8 小时（单一 HEAD，含 PIT 2x 修复验证）

## 结论（先看）

**PASS**：在 `39b3512`（tag `v0.74.1`，版本 0.74.1，`ci -Full` **248/248 ×2**）上，普通构建出的 `x86 OS v0.74.1 (build Oct 27 2026 04:56:07)` 用文件串口连续跑满 **8 h 00 m 05 s**（04:59:41 → 12:59:46）：

- **28,542** 条心跳（`0 → 28,541`，**无跳号、无缺失、无重复**），约 **59.7 次/分钟**——回到 1 秒 1 跳；
- 串口 **1,120,959 B**，到最后一次采样仍在增长（无停滞）；
- **0 次 panic / 0 次 oops / 0 次 triple fault / 0 次 GP / 0 个 CRASH.txt / 0 次停滞**；
- `free_pages=26487`、`procs=23`、`faults=1` 在 **478 个快照（uptime 60 s → 28,680 s）**中完全不变；
- **PIT 1:1 自检通过**：`irq: pit rate ok (1200 ticks / 13 s)`；guest uptime 28,680 s / 墙钟 28,804 s = **0.996**；
- QEMU 到点由脚本正常终止：`soak-file done: elapsed=8 h, last heartbeat=28541`，`EXIT=0`。

这是**单一构建、单一 HEAD、连续不中断**的 8 小时，且专门覆盖了 v0.74.0 的 2x tick 回归修复，可作为 v0.74.1 的 release 前浸泡证据。

## 2x tick 回归与修复（0.74.0 → 0.74.1）

v0.74.0 的 8 h 浸泡因 guest tick ≈2x 墙钟而作废（见 [soak-0.74.0-2x-tick.md](soak-0.74.0-2x-tick.md)）。根因与修复：

| 项 | v0.74.0（作废，`ac4f72f`） | v0.74.1（本报告，`39b3512`） |
|---|---|---|
| PIT 通道 0 模式 | mode 3 方波：OUT 每周期翻转两次 | **mode 2 rate generator**：每周期一个脉冲（`ea5da7a`） |
| IRQ0 投递 | 迁到 IOAPIC GSI2（edge） | 迁到 IOAPIC GSI2（edge，不变） |
| 结果 | 8259 只认一个上升沿，IOAPIC edge 把两次都当触发 → 100 Hz PIT ≈ **200 次 IRQ0/s** | 两种投递路径都只有 **1 次/周期** |
| 频率自检 | 无 | `irq: pit rate ok (1200 ticks / 13 s)`（1200 tick 应 ≈12 RTC 秒，容差 8–18 s；2x 回归时约 6 s） |
| 3 min 冒烟 | 179 s 内 `uptime=60…300`、323 心跳（≈2/s） | 180.4 s 内 `uptime=60/120`、163 心跳（1/s） |
| 8 h 试点 | 墙钟 1,207 s 已到 guest 2,400 s = **1.99x** | 墙钟 28,804 s 对应 guest 28,680 s = **0.996** |

> 判定：2x 回归已修复，计时口径恢复 1:1；本轮 `uptime`/`sleep`/TCP RTO/心跳/`soak` 行全部按墙钟走。

## 构建与冒烟

| 步骤 | 命令 | 结果 |
|---|---|---|
| 快照 | `git status --porcelain` | 空；仅 0.73.0 旧浸泡的 QEMU 在跑 |
| HEAD / tag | `git rev-parse --short HEAD` / `git describe --tags` | `39b3512` / `v0.74.1` |
| 构建 | `build.ps1`（普通，增量） | OK，**19.0 s**，kernel **383,232 B**，`dist/floppy-run.img` 1,474,560 B，04:56:22 生成 |
| 镜像版本串 | — | `kernel: x86 OS v0.74.1 (build Oct 27 2026 04:56:07)` |
| 冒烟 | `soak-file.ps1 -Hours 0.05 -Smp 2 -SerialLog obj/soak-file/smoke-0.74.1.log` | 180.4 s，心跳 0→162，**26,650 B**，`irq: pit rate ok (1200 ticks / 13 s)`，`uptime=60/120`，exit 0，无 CRASH |

> 环境注记：构建时 `dist/floppy.img` 仍被并行运行的 **0.73.0 8h 浸泡**（QEMU PID 13944）占用，`build.ps1` 自动回退写出 `dist/floppy-run.img`，本轮浸泡用后者。该 0.73 浸泡于 **08:31** 按计划自行结束（`soak-file done: elapsed=8 h, last heartbeat=28638`），**未被本会话触碰**。

## 8 小时浸泡记录

| 项 | 值 |
|---|---|
| 启动 | 2026-10-27 **04:59:41**（QEMU PID 24508） |
| 结束 | 2026-10-27 **12:59:46**（串口最后写入 12:59:45） |
| 墙钟时长 | **8 h 00 m 05 s**（28,805 s） |
| 客机 uptime（最后 soak 行） | 28,680 s（7 h 58 m，含约 2 min 引导/停机开销） |
| 命令 | `tools\soak-file.ps1 -Hours 8 -Smp 2 -Image dist/floppy-run.img -MonitorPort 4496 -SerialLog obj/soak-file/serial-8h-0.74.1.log` |
| 结束方式 | 到点由脚本 `Stop-Process` 终止 QEMU，`EXIT=0` |

### 每小时采样（`obj/soak-file/hourly-0.74.1.log`）

| 时刻 | 墙钟 s | guest uptime | 心跳 | 串口字节 | QEMU | CRASH | free/procs/faults |
|---|---|---|---|---|---|---|---|
| 05:20 | 1,200 | 1,200 | 1,194 | 64,618 | alive | False | 26487/23/1 |
| 06:00 | 3,600 | 3,600 | 3,574 | 154,918 | alive | False | 26487/23/1 |
| 07:00 | 7,200 | 7,140 | 7,141 | 290,201 | alive | False | 26487/23/1 |
| 08:00 | 10,800 | 10,740 | 10,707 | 426,224 | alive | False | 26487/23/1 |
| 09:00 | 14,400 | 14,340 | 14,276 | 565,266 | alive | False | 26487/23/1 |
| 10:00 | 18,000 | 17,940 | 17,845 | 704,308 | alive | False | 26487/23/1 |
| 11:00 | 21,600 | 21,540 | 21,415 | 843,388 | alive | False | 26487/23/1 |
| 12:00 | 25,200 | 25,080 | 24,986 | 982,449 | alive | False | 26487/23/1 |
| 13:00 | 28,801 | 28,680 | 28,541 | 1,120,959 | 已按计划终止 | False | 26487/23/1 |

> 监控器每 20 min 采样一次，共 **24 个样本**（`obj/soak-file/monitor-0.74.1.csv`，25 行含表头）；会话内另按约 10–30 min 人工巡检，逐次记录心跳/字节/guest/CRASH/QEMU，结果与监控器一致（`inspect-0.74.1.log`，22 条）。
> 表中 guest 值取"最近一条 60 s soak 行"，最多滞后 60 s；1:1 的判据用**心跳增量/墙钟增量**（各区间 0.993–1.000）与最终 uptime/墙钟 0.996。

## 健康指标

- **心跳连续性**：28,542 条，首 0、末 28,541，相邻编号差恒为 1（`gaps=0`，`unique=28542`），无挂起或丢心跳。
- **1:1 计时**：`irq: pit rate ok (1200 ticks / 13 s)`；全程心跳 ≈59.7 次/分钟；最终 guest 28,680 s vs 墙钟 28,804 s = **0.996**（差值即引导/终止开销）。
- **内存/进程**：`soak: uptime=… free_pages=… procs=… faults=…` 共 478 行，三值全程唯一组合 `26487 / 23 / 1`——无页泄漏、无进程泄漏、无异常缺页。
- **启动自检**：`memtest` 8/8 + pmm/heap/rss/file-mmap 额外项 PASS（`failures = 0`），`selftest: PASS`；`native: ABI 0.1 gate ok`；`smp: tick check` 正常；`irq: pit rate ok`。
- **串口增长**：到最后一次采样仍持续增长，`StallSec=120` 判据从未触发（`STALL` 行 = 0）。

## CRASH / panic / 停滞统计

| 项 | 值 |
|---|---|
| `obj/soak-file/CRASH.txt` | **不存在**（全程） |
| panic / oops / triple fault / general protection / #GP | **0**（大小写敏感匹配） |
| 停滞（`STALL` 行 / 串口 120 s 无增长） | **0** |
| QEMU 提前退出 | 否（按计划到点终止，exit 0） |
| 心跳缺失 / 重复 | 0 / 0 |

> 复核说明：不区分大小写地搜索 `stall` 会命中 `installed` 子串（`system:/bin/shell installed`、`legacy gates installed`），已按大小写敏感 + 词边界复核为 0 个真实停滞。

## 产物（`obj/soak-file/`）

| 文件 | 说明 |
|---|---|
| `serial-8h-0.74.1.log` | 8 h 串口日志，1,120,959 B，28,542 条心跳 |
| `serial-8h-0.74.1.out` | 浸泡脚本输出，`soak-file done … EXIT=0` |
| `smoke-0.74.1.log` / `smoke-0.74.1.out` | 3 min 冒烟（含 PIT 自检） |
| `monitor-0.74.1.csv` / `hourly-0.74.1.log` | 24 个 20 min 采样 / 9 个小时级汇总 |
| `inspect-0.74.1.log` | 会话内 22 次人工巡检 |
| `build-0.74.1.log` | 构建日志 |
| `probe-*` / `evidence-2x-tick.txt` | 0.74.0 2x 回归的探测脚本与现场（作废轮次，保留） |

## 约束遵守

- 未修改 `src/**`、`build.ps1`、`Makefile`、`tools/*`；本轮未触发任何修复路径。
- 浸泡期间未跑 `-Test` / flake-hunt / 其它 QEMU；全程仅本浸泡一个 QEMU 实例（PID 24508）。
- 0.73.0 旧浸泡（PID 13944）并行运行至 08:31 自行结束，本会话未动。
- 未删除 `dist/`；未 `git add -A`；提交仅限本文件。
- 只写 `obj/soak-file/**` 与本文件。

## 结论与建议

v0.74.1（`39b3512`）在单一 HEAD 上连续 8 小时稳定：无 panic、无 CRASH、无停滞、无泄漏，心跳与内存/进程指标全程恒定，且 PIT 1:1 自检与 guest/墙钟比例确认 **2x tick 回归已修复**。**建议放行 release。**

> 注：本报告写作时仓库 HEAD 已推进到 `2c31d5b`（`v0.74.1` 之后 3 个提交，均为 docs/status 类，`src/`、`build.ps1`、`Makefile`、`tools/` 无改动）。浸泡镜像构建于 **`39b3512`（v0.74.1）**，与上述提交的内核内容一致。
