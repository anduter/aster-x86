# 浸泡结果 — 0.73.0 发布后 8 小时（单一 HEAD `f982d3b` / tag `v0.73.0`）

## 结论（先看）

**PASS**：在发布提交 `f982d3b`（tag `v0.73.0`，版本 0.73.0，`ci.ps1 -Full -StrictBom` 182/182 ×2）构建的镜像上，用文件串口连续跑满 **8 h 00 m 02 s**：

- **28,639** 条心跳（`0 → 28,638`，**无跳号、无重复**），约 **59.7 次/分钟**；
- 串口 **1,117,181 B**，结束时仍在增长（无停滞）；
- **0 次 panic / 0 次 oops / 0 次 triple fault / 0 次 GP / 0 个 CRASH.txt / 0 次停滞**；
- `free_pages=26492`、`procs=19`、`faults=1` 在 **478 个快照（uptime 60 s → 28,680 s）** 中完全不变；
- QEMU 按计划被脚本终止，`soak-file done: elapsed=8 h, last heartbeat=28638`，`EXIT=0`。

这是**单一构建、单一 HEAD、连续不中断**的 8 小时，不是跨构建拼凑，可作为 v0.73.0 的 release 浸泡证据。

> 说明：浸泡期间 main 被并行会话继续推进（最终到 v0.74.1 / 248 项）。本报告只覆盖 0.73.0 发布提交 `f982d3b`，不与后续 HEAD 的回归数字混用。

## 构建与冒烟

| 步骤 | 命令 / 值 | 结果 |
|---|---|---|
| 发布 HEAD | `f982d3b1edbd9548c84b09ff971b24d5d92ed683` | tag `v0.73.0`（annotated） |
| 版本 | `src/kernel/core/version.h` | `0.73.0` |
| 构建 | `build.ps1`（默认 `dist/`） | OK，kernel **309,492 B**，`dist/floppy.img` 1,474,560 B，**00:27:49** 生成 |
| 镜像 SHA256 | `dist/floppy.img` | `8900d515479076122376041b0d82dd8aac16f3e93d0d04972f7f9635acc0d33d` |
| 冒烟 | `soak-file.ps1 -Hours 0.05 -Smp 2 -SerialLog obj/soak-file/smoke-0.73.log` | 180 s，心跳 0→169（170 条），19,023 B，`EXIT=0`，无 CRASH |
| 冒烟版本行 | 串口 | `kernel: x86 OS v0.73.0 (build Oct 27 2026 00:27:35)` |

## 8 小时浸泡记录

| 项 | 值 |
|---|---|
| 启动 | 2026-10-27 **00:31:11**（QEMU PID 13944） |
| 结束 | 2026-10-27 **08:31:13**（脚本终止；串口最后写入 08:31:12） |
| 墙钟时长 | **8 h 00 m 02 s** |
| 客机 uptime（最后 soak 行） | 28,680 s（7 h 58 m 00 s，含约 1 min 引导） |
| 命令 | `tools\soak-file.ps1 -Hours 8 -Smp 2 -SerialLog obj/soak-file/serial-8h-0.73.log` |
| 结束方式 | 到点由脚本 `Stop-Process` 终止 QEMU，`EXIT=0` |
| 串口 SHA256 | `e7e80386b74f51661c62a849d8fe85f8e26e4730291b043ce901ae047d7ea20c` |

### 每小时采样（`obj/soak-file/hourly-0.73.log` / `monitor-0.73.csv`）

| 时间 | 心跳最大值 | 串口字节 | 快照 free/procs/faults | QEMU（本浸泡） | CRASH |
|---|---|---|---|---|---|
| 00:51 | 1,192 | 57,024 | 26492 / 19 / 1 | alive | False |
| 01:51 | 4,775 | 192,955 | 26492 / 19 / 1 | alive | False |
| 02:51 | 8,356 | 328,812 | 26492 / 19 / 1 | alive | False |
| 03:51 | 11,942 | 466,830 | 26492 / 19 / 1 | alive | False |
| 04:51 | 15,528 | 606,518 | 26492 / 19 / 1 | alive | False |
| 05:51 | 19,111 | 746,092 | 26492 / 19 / 1 | alive | False |
| 06:51 | 22,686 | 885,362 | 26492 / 19 / 1 | alive | False |
| 07:51 | 26,262 | 1,024,670 | 26492 / 19 / 1 | alive | False |
| 08:31（结束） | 28,638 | 1,117,181 | 26492 / 19 / 1 | 到点终止 | False |

> 监控器每 20 min 采样一次，共 **24 个样本**（00:51 → 08:31，`monitor-0.73.csv`）；会话内另按约 10–20 min 巡检并写入 `inspect-0.73.log`，两者一致。
>
> 小时行的心跳数按 `soak: uptime=3600·N` 前累计的 `thread: heartbeat` 行统计：第 1 小时 3,584 条、第 7 小时 25,112 条，速率稳定。

## 健康指标

- **心跳连续性**：28,639 条，首 0、末 28,638，相邻编号差恒为 1（`gaps=0`、`dups=0`）。
- **内存/进程**：`soak: uptime=… free_pages=… procs=… faults=…` 共 **478** 行，三值全程唯一组合 **26492 / 19 / 1**——无页泄漏、无进程泄漏。
  - 与 0.72.0 的 `26494 / 18 / 1` 相比，`free_pages` 少 2、`procs` 多 1，符合 0.73.0 新增 `pkgd`/`pkgtest` 等组件程序后的基线，不是泄漏。
- **启动自检**：`selftest: PASS`、`fs: backend selftest ok`、`pkg: selftest done`、`net: tcp selftest PASS`、`sip: selftest PASS`（串口可见）。
- **登录栈**：`getty: tty0 … login required`、`osteachos login:` 正常（未启用 autologin）。
- **串口增长**：到最后一次采样仍持续增长，`StallSec=120` 判据从未触发。

## CRASH / panic / 停滞统计

| 项 | 值 |
|---|---|
| `obj/soak-file/CRASH.txt` | **不存在**（全程） |
| `KERNEL PANIC` / `panic:` / `oops` / `triple fault` / `general protection` / `BUG:` | **0** |
| 停滞（大写 `STALL` / 串口 120 s 无增长） | **0** |
| QEMU 提前退出 | 否（按计划到点终止） |
| 心跳缺失 / 重复 | 0 / 0 |

> 复核方法：`-match` 使用词边界/大写标记；`installed` 中的子串 `stall`、“panic” 源码清单行不计入。

## 环境注记（并行会话）

浸泡期间工作区被多个并行开发会话共用，属预期：

1. **并行 QEMU**：期间观察到 1–3 个其它 `qemu-system-i386`，例如 `.dist-sec`（QMP 4511，01:30 起）和 04:59 起的实例；均未干预，也未运行 `-Test`。本浸泡始终保持唯一 `dist/floppy.img` 实例（PID 13944），心跳按 tick 计，不受 CPU 争用影响。
2. **镜像锁**：`dist/floppy.img` 被 QEMU 持有文件锁，期间其它会话的构建退到 `floppy-run.img`；本镜像 mtime 始终为 **00:27:49**，结束时 SHA256 与启动时一致（见上）。
3. **main 前进**：浸泡开始时 HEAD 为 `f982d3b`；结束时 main 已到 `0cdf61f`（v0.74.1，248 项）。0.73.0 的 tag/镜像/串口数据均为独立证据，不受影响。
4. **宿主端口**：约 00:25 期间 qBittorrent 临时绑定 `0.0.0.0:12345`（BitTorrent 出站连接），与 `-Test` 的 echo 端口冲突，详见下节。

## 发布与 CI（含失败与排除）

| 项 | 结果 |
|---|---|
| release | `tools/release.ps1 -Version 0.73.0`，commit `f982d3b`，tag `v0.73.0`（annotated → `f982d3b`） |
| 发布产物 | `floppy.img`、`kernel.bin` 309,492 B、`user_shell.elf`、`shell.sig`、`kernel.sig`、`SHA256SUMS`、`RELEASE.md` |
| 模数指纹 | `sha256:eaff7f6182fe1a11e413aa5a951f894d62b70daeba440ba3216b34af5eadb524`（与 `keys/shell_public.pem` 一致） |
| 发布 SHA256 | `floppy 9f122f8f…a9d8`、`kernel 41865074…7b32`、`user_shell 45be5e4a…cb3a`、`shell.sig cbd284cd…fcaf`、`kernel.sig c85c586f…4a62`（完整值见当时校验输出与 `obj/release-0.73.0.log`） |
| `ci.ps1 -Full -StrictBom` | **182/182 ×2 PASS**，worktree clean，84.1 s |

**两次前置失败（均非代码缺陷，未改检查项/未放宽断言）：**

1. **`restart 后服务可用` 偶发失败**（第 1 次 -Test）：`user_demo` 的 restart 轮询（最多 2 s）与 `pkgtest` 卸载 `echosvc` 并发，harness 在 36.0 s 采样时 `native: restart ok` 尚未出现；串口无 `FAILED`，仅此 1 项失败。日志保留：`obj/ci/flake-restart-2026-10-27.log`。第 3 次重跑双 PASS。
2. **echo 端口 12345 被占**（第 2 次 -Test）：宿主 qBittorrent 临时绑定 `0.0.0.0:12345`，`build.ps1` 按设计拒绝启动 `-Test`；preflight 与 build 之间发生，属环境竞态。日志保留：`obj/ci/full-fail2-2026-10-27.log`。

> 发布产物 `dist/release/` 在 04:55 被并行会话的 v0.74.1 发布覆盖（`dist/` 为 gitignored）。0.73.0 的 tag、`docs/changelog/v0.73.0.md`、`obj/release-0.73.0.log` 与本节 SHA256 记录仍可追溯；未重新生成 0.73.0 产物以免覆盖并行工作。

## 产物（`obj/soak-file/`，gitignored）

| 文件 | 说明 |
|---|---|
| `serial-8h-0.73.log` | 8 h 串口日志，1,117,181 B，28,639 条心跳 |
| `serial-8h-0.73.out` | 浸泡脚本输出，`soak-file done … / EXIT=0` |
| `smoke-0.73.log` / `smoke-0.73.out` | 3 min 冒烟 |
| `monitor-0.73.csv` / `hourly-0.73.log` | 24 个 20 min 采样 / 每小时汇总 |
| `inspect-0.73.log` | 会话内巡检记录（约 10–20 min 一次） |
| `monitor-0.73.ps1` / `check.ps1` | 本次监控/巡检临时脚本（不入库） |

## 约束遵守

- 未修改 `src/**`、`build.ps1` 检查项、`tools/soak*.ps1`、`flake-hunt`；本次未触发任何修复路径。
- 浸泡期间未跑 `-Test` / flake-hunt / 其它 QEMU（并行会话的 QEMU 未干预）。
- 未删除 `dist/`；未 `git add -A`；提交仅限本文件与 status/roadmap 路径。
- 未 push（无远端）；未声称 GitHub Actions 实跑。

## 结论与建议

v0.73.0（`f982d3b`）在单一 HEAD 上连续 8 小时稳定：无 panic、无 CRASH、无停滞、无泄漏，心跳与内存/进程指标全程恒定。**v0.73.0 的 release 浸泡证据成立。**

> 后续 main 已到 v0.74.1（248 项），其 8h 浸泡需另行安排；本报告不覆盖。
