# 浸泡结果 — 0.72.0 发布前最终 8 小时（单一 HEAD）

## 结论（先看）

**PASS**：在单一 HEAD `646f81d`（tag `v0.72.0`，版本 0.72.0，`ci.ps1 -Full -StrictBom` 131/131）上，用文件串口跑满连续 **8 h 00 m 05 s**：

- **28,643** 条心跳（`0 → 28,642`，**无跳号、无缺失**），约 **59.7 次/分钟**；
- 串口 **1,116,133 B**，结束时仍在增长（无停滞）；
- **0 次 panic / 0 次 oops / 0 次 triple fault / 0 次 GP / 0 个 CRASH.txt / 0 次停滞**；
- `free_pages=26494`、`procs=18`、`faults=1` 在 **479 个快照（uptime 60 s → 28,740 s）**中完全不变；
- QEMU 按计划被脚本正常终止，脚本 `EXIT=0`。

这是**单一构建、单一 HEAD、连续不中断**的 8 小时，不是跨构建拼凑，可作为 v0.72.0 的 release 前浸泡证据。

## 构建与冒烟

| 步骤 | 命令 | 结果 |
|---|---|---|
| 快照 | `git status --porcelain -- src` | 空；无 QEMU 进程 |
| HEAD / tag | `git rev-parse --short HEAD` / `git describe --tags` | `646f81d` / `v0.72.0` |
| 构建 | `build.ps1`（普通，增量） | OK，**13.0 s**，kernel **260,308 B**，`dist/floppy.img` 1,474,560 B，03:07:39 生成 |
| 冒烟 | `soak-file.ps1 -Hours 0.05 -Smp 2 -SerialLog obj/soak-file/smoke-final.log` | 180 s，心跳 0→168，17,751 B，exit 0，无 CRASH |

## 8 小时浸泡记录

| 项 | 值 |
|---|---|
| 启动 | 2026-10-26 **03:11:03**（QEMU PID 14752） |
| 结束 | 2026-10-26 **11:11:08**（串口最后写入） |
| 墙钟时长 | **8 h 00 m 05 s** |
| 客机 uptime（最后 soak 行） | 28,740 s（7 h 59 m 00 s，含约 1 min 引导） |
| 命令 | `tools\soak-file.ps1 -Hours 8 -Smp 2 -SerialLog obj/soak-file/serial-8h-final.log` |
| 结束方式 | 到点由脚本 `Stop-Process` 终止 QEMU，`soak-file done: elapsed=8 h, last heartbeat=28641`，`EXIT=0` |

### 每小时采样（`obj/soak-file/hourly-final.log`）

| 小时 | 心跳最大值 | 串口字节 | 心跳行数 | QEMU | CRASH |
|---|---|---|---|---|---|
| 0 | 1,183 / 2,376 | 55,434 / 100,695 | 1,184 / 2,377 | alive | False |
| 1 | 3,570 | 145,993 | 3,571 | alive | False |
| 2 | 7,151 | 281,850 | 7,152 | alive | False |
| 3 | 10,733 | 418,491 | 10,734 | alive | False |
| 4 | 14,315 | 558,027 | 14,316 | alive | False |
| 5 | 17,896 | 697,525 | 17,897 | alive | False |
| 6 | 21,479 | 837,099 | 21,480 | alive | False |
| 7 | 25,061 | 976,635 | 25,062 | alive | False |
| 8 | 28,642 | 1,116,133 | 28,643 | 结束后终止 | False |

> 监控器每 20 min 采样一次，共 25 个样本（`obj/soak-file/monitor-final.csv`）；会话内另按约 20 min 人工巡检，逐次记录心跳/字节/进程/CRASH，结果与监控器一致。

## 健康指标

- **心跳连续性**：28,643 条，首 0、末 28,642，相邻编号差恒为 1（`HB_GAPS=0`），无挂起或丢心跳。
- **内存/进程**：`soak: uptime=… free_pages=… procs=… faults=…` 共 479 行，三值全程唯一组合 `26494 / 18 / 1`——无页泄漏、无进程泄漏。
- **启动自检**：`memtest` 8/8 + pmm/heap 额外项 PASS（`failures = 0`），`selftest: PASS`，`acct: selftest PASS`，`fs: backend selftest ok`，`abi: version ok … abi=1.3`、`native: ring3 ABI 0.1 ok`。
- **登录栈**：`getty: tty0 osteachos login required`，`osteachos login:` 提示正常（本次 cmdline 未启用 autologin）。
- **串口增长**：到最后一次采样仍持续增长，`StallSec=120` 判据从未触发。

## CRASH / panic / 停滞统计

| 项 | 值 |
|---|---|
| `obj/soak-file/CRASH.txt` | **不存在**（全程） |
| panic / oops / triple fault / general protection / BUG: / #GP | **0** |
| 停滞（大写 `STALL` 标记 / 串口 120 s 无增长） | **0** |
| QEMU 提前退出 | 否（按计划到点终止） |
| 心跳缺失 | 0 |

> 复核说明：`Select-String -Pattern 'panic|oops|...|STALL'` 曾报 2 条，人工核对为 `installed` 中的子串 `stall`（`system:/bin/shell installed`、`legacy gates installed`），**不是**停滞事件；按词边界/大写 `STALL` 复核为 0。

## 产物（`obj/soak-file/`）

| 文件 | 说明 |
|---|---|
| `serial-8h-final.log` | 8 h 串口日志，1,116,133 B，28,643 条心跳 |
| `serial-8h-final.out` | 浸泡脚本输出，`soak-file done … / EXIT=0` |
| `smoke-final.log` / `smoke-final.out` | 3 min 冒烟日志 |
| `monitor-final.csv` / `hourly-final.log` | 25 个 20 min 采样 / 每小时汇总 |
| `monitor-final.ps1` | 本次监控脚本（BOM） |
| `build-final.log` | 构建日志 |

## 约束遵守

- 未修改 `src/**`、`build.ps1`、`Makefile`、`tools/*`；本次未触发任何修复路径。
- 浸泡期间未跑 `-Test` / flake-hunt / 其它 QEMU；全程仅 `dist/floppy.img` 单个实例（PID 14752）。
- 未删除 `dist/`；未 `git add -A`；提交仅限本文件。
- 只写 `obj/soak-file/**` 与本文件。

## 结论与建议

v0.72.0（`646f81d`）在单一 HEAD 上连续 8 小时稳定：无 panic、无 CRASH、无停滞、无泄漏，心跳与内存/进程指标全程恒定。**建议放行 release。**

> 附注：本会话的 20 min 巡检脚本 `obj/soak-file/monitor-final.ps1` 属临时产物，不入库（`obj/` 已被忽略）。
