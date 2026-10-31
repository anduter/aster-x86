# ng-WP4-4：1 小时 smoke（PASS）

## 结论

**PASS** —— ng 镜像在 QEMU 11.0.0 上连续运行 **60.17 分钟**（18:06:11 →
19:06:21），12 次采样全部存活，15/30/45 分钟各注入两条 shell 命令，
**6/6 响应**（证据行 + 新提示符都出现），串口 **20,557 → 21,229 字节**
（+672 B 全部来自 6 条命令，空闲窗口零增长），**0 PANIC / 0 冻结 / 0 重启**，
结束时空闲在 `ng>` 提示符，经 QMP `quit` 正常退出。

- 镜像：`obj-ng/smoke/dist/aster-ng.elf`，2,082,328 字节，
  SHA-256 `027C88A53EA3DE5DDD9D8B5C0ED121298F96F057FF2E7D87FE4FE61B38AA294C`；
  构建源自 **9a6ab48**（`ng`，tag `ng-e3`）干净工作区（17:58:34 构建）。
- 宿主：QEMU 工作集 **73.1 → 73.5 MB** 平台期（+0.4 MB），线程 5–8；
  宿主 CPU 3586 s / 3610 s 墙钟（≈ 1 核 99%），见“内存/进程趋势”的观察项。
- 说明：ng 串口**没有周期 heartbeat**（`heartbeat` 程序只存在于 legacy
  `src/kernel/apps/heartbeat.c`，不在 ng 构建里）。因此“还活着”由三路证据
  共同判定：QEMU 进程存活 + 每 5 分钟 QMP `query-status: running` +
  每 15 分钟 shell 命令的真实响应。

## 时间线

| 时间（+08:00） | 事件 |
|---|---|
| 17:56 | 会话开始；`git status` 仅 `?? obj-ng/`；无任何 `qemu-system-x86_64` 进程 |
| 17:58:23–17:58:34 | 隔离目录构建 `obj-ng/smoke/{obj,dist}`，`build-ng: PASS`（61 步） |
| 18:06:10 | 预运行记录：HEAD `f40464c`、0 QEMU（见 `obj-ng/smoke/pre-run.txt`） |
| 18:06:11 | QEMU 启动，PID **5064**，QMP 端口 **11996** |
| 18:06:20 | `ng: shell ready` + `ng>` 提示符（启动耗时 **9,699 ms**） |
| 18:11:21 / 18:16:21 | 采样 t=5/10 min，串口 20,557 B，QMP running |
| 18:21:21 | 采样 t=15 min；注入 `ls /`（213 ms）、`dmesg -n 3`（479 ms） |
| 18:36:21 | 采样 t=30 min；注入 `ls /`（196 ms）、`dmesg -n 3`（486 ms） |
| 18:51:21 | 采样 t=45 min；注入 `ls /`（198 ms）、`dmesg -n 3`（481 ms） |
| 19:06:21 | 采样 t=60 min：存活、QMP running、串口 21,229 B、`ng>` 空闲 |
| 19:06:24 | QMP `quit`；进程退出，`qemu.err` 0 字节；`summary.json` 落盘 |
| 19:10 前后 | 写本报告并提交（`session-commit.ps1 -Name ng`） |

## 环境与镜像

| 项 | 值 |
|---|---|
| 仓库 / 分支 | `D:\OS_Project` / `ng` |
| 构建基线 | **9a6ab48**（tag `ng-e3`），构建时工作区除 `obj-ng/` 外干净 |
| 运行期间 HEAD | 并行会话推进到 `7401160`（WP4-1/4-2/4-3，18:06–18:08）；`src/**`、`tools/build-ng.ps1`、`build.ps1` 无改动，仅新增 `sdk/examples/hello_d0/**`（WP4-2，未参与本构建）与文档/工具 |
| 工具链 | clang / ld.lld（`D:\msys64\mingw64\bin`，build-ng 自选） |
| QEMU | 11.0.0（`v11.0.0-12122-ga4bb4b10c9`） |
| 内核镜像 | `obj-ng/smoke/dist/aster-ng.elf`，2,082,328 B，ELF64/5 PT_LOAD/PVH；SHA-256 见上 |
| 磁盘 | `obj-ng/smoke/dist/disk.img`，8,388,608 B（FAT16）；SHA-256 `750C54A77AECB0F9E44059814FB73384532F605C590583334DB4CF13AF69070F` |
| 内存 / 显卡 | `-m 512`、`-vga std` |
| QEMU 参数 | `-kernel …\aster-ng.elf -serial file:…\1h.log -drive file=…\disk.img,format=raw,if=ide -display none -no-reboot -m 512 -cpu max -vga std -qmp tcp:127.0.0.1:11996,server=on,wait=off` |

构建全程使用 `-ObjDir obj-ng/smoke/obj -DistDir obj-ng/smoke/dist`，
**没有覆盖 `dist-ng/`**（`dist-ng` 在 18:09 的更新来自并行 WP4-3 release 会话，
不是本会话）。`-cpu max`、`-no-reboot` 与本项目既有 run-ng/s1-fault 口径一致
（任务列表未列，属运行稳定性所需）。

## 方法

1. **构建**（隔离产物，只落 `obj-ng/smoke/**`）：
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-ng.ps1 `
       -Repo D:\OS_Project -ObjDir obj-ng/smoke/obj -DistDir obj-ng/smoke/dist
   ```
2. **运行**：`obj-ng/smoke/smoke-1h.ps1` 启动 QEMU 后等到 `ng: shell ready`
   且 `ng>` 出现，开始计时；随后每 `SampleSec=300` 秒采样一次，共 12 次；
   每 `InjectEveryMin=15` 分钟检查 shell 是否空闲（串口最后 4 字节是提示符、
   其后没有输入回显），空闲才注入。
3. **注入方式**：QMP `input-send-event`（QEMU 11 已无 `sendkey`）逐字符发
   qcode（`spc`/`slash`/`minus` + 字母数字），最后 `ret`；从回车起轮询串口，
   直到证据行出现，记录耗时。`ls /` 证据 = `ng: shell ls ok`，
   `dmesg -n 3` 证据 = `ng: shell dmesg ok`。
4. **结束**：t=60 min 采样后发 QMP `quit`（不 kill），等待 15 s，兜底才
   `Stop-Process`；写 `samples.log` + `summary.json`。
5. 正式 1h 前用同一脚本做过 2 轮 2 分钟预跑（校准 QMP JSON 对象与注入路径），
   预跑与正式跑都只写 `obj-ng/smoke/**`；预跑不作为证据。

**PASS 判据**：QEMU 全程存活；12/12 QMP `running`；6/6 注入成功；0 PANIC；
结束仍在 `ng>` 提示符；串口只随命令增长。

## 结果

### 每 5 分钟采样

| t (min) | 时刻 | 存活 | QMP | 串口 B | 提示符数 | WS (MB) | CPU (s) | 线程 |
|---:|---|---|---|---:|---:|---:|---:|---:|
| 5 | 18:11:21 | yes | running | 20,557 | 1 | 73.1 | 309.0 | 6 |
| 10 | 18:16:21 | yes | running | 20,557 | 1 | 73.3 | 607.0 | 6 |
| 15 | 18:21:21 | yes | running | 20,557 | 1 | 73.3 | 904.9 | 6 |
| 20 | 18:26:21 | yes | running | 20,781 | 3 | 73.4 | 1203.1 | 6 |
| 25 | 18:31:21 | yes | running | 20,781 | 3 | 73.4 | 1500.9 | 5 |
| 30 | 18:36:21 | yes | running | 20,781 | 3 | 73.4 | 1798.7 | 5 |
| 35 | 18:41:21 | yes | running | 21,005 | 5 | 73.4 | 2096.5 | 5 |
| 40 | 18:46:21 | yes | running | 21,005 | 5 | 73.5 | 2394.4 | 7 |
| 45 | 18:51:21 | yes | running | 21,005 | 5 | 73.4 | 2692.2 | 5 |
| 50 | 18:56:21 | yes | running | 21,229 | 7 | 73.5 | 2990.2 | 8 |
| 55 | 19:01:21 | yes | running | 21,229 | 7 | 73.5 | 3288.2 | 6 |
| 60 | 19:06:21 | yes | running | 21,229 | 7 | 73.5 | 3586.0 | 8 |

（t=15/30/45 的采样先于注入，故串口增量出现在下一次采样；提示符数
1→3→5→7 = 初始 1 次 + 6 条命令各 1 次。）

### 命令响应（6/6）

| 计划 | 实际 | 命令 | 证据行 | 回车→证据 | 串口增量 |
|---|---|---|---|---:|---|
| 15 min | 18:21:21 | `ls /` | `ng: shell ls ok` | **213 ms** | +224 B（两条合计） |
| 15 min | 18:21:22 | `dmesg -n 3` | `ng: shell dmesg ok` | **479 ms** | 同上 |
| 30 min | 18:36:21 | `ls /` | `ng: shell ls ok` | **196 ms** | +224 B |
| 30 min | 18:36:22 | `dmesg -n 3` | `ng: shell dmesg ok` | **486 ms** | 同上 |
| 45 min | 18:51:21 | `ls /` | `ng: shell ls ok` | **198 ms** | +224 B |
| 45 min | 18:51:22 | `dmesg -n 3` | `ng: shell dmesg ok` | **481 ms** | 同上 |

每条命令后提示符都重新出现（`prompt_delta=+1`），说明 shell 读键、解析、
VFS/log 调用和输出链路完整；`ls /` 的 8 项输出与既有 WP2-6 基线一致。

### 串口与输出

- 启动完成 **20,557 B / 507 行**；结束 **21,229 B**（+672 B = 3×2 条命令，
  每次 `ls /`+`dmesg -n 3` 恰为 224 B）。
- 空闲窗口零增长：5–15 min 保持 20,557；20–30 min 保持 20,781；
  35–45 min 保持 21,005；50–60 min 保持 21,229。没有日志环/定时打印刷屏。
- 末字节为 `ng> `（无换行），即 shell 停在空闲提示符。
- 典型现场：

  ```text
  ng> ls /
  tmp/ mnt/ selftest/ edge/ etc/ System/ parity/ bin/
  ls: (showing the first 8 entries)
  ng: shell ls ok
  ng> dmesg -n 3
  selftest: WP3-1 PASS
  selftest: WP3-2 PASS
  selftest: WP3-3 PASS
  ng: shell dmesg ok
  ng>
  ```

### PANIC / 冻结 / 重启

- `PANIC` **0** 次、`FAIL` **0** 次；采样期间无新增 `#PF`/`#GP`。
  串口里出现的 `ng: #pf` ×2、`ng: #ud` ×1、`ng: #gp` ×1 是 WP1-3 启动自检的
  **受控探测**（`ng: #pf cr2=0x15f000 …` / `trap recovery`），不是异常。
- QEMU 进程 PID **5064** 从启动到结束未变；12/12 次 `query-status` 都是
  `running`；`qemu.err` 0 字节；`-no-reboot` 下没有任何退出/重启。
- 没有冻结证据：t=15/30/45 的命令都在 0.5 s 内得到响应；结束采样仍取到
  空闲提示符。

### 内存 / 进程趋势

- **客户机**：启动日志的 PMM 快照为
  `window=0x0..0x40000000 total=262144 usable=130943 free=130163`
  （512 MB 客户机中可用约 511.5 MiB、启动后空闲 508.4 MiB）。
  ng 串口没有周期性内存/进程计数行，**无法从串口得出客户机内存曲线**；
  能确认的是：shell、displayd/wm/sessiond 链路在 1 小时内没有出现
  失败/重启类输出，`dmesg -n 3` 每次仍返回 WP3 的 PASS 日志。
- **宿主**：QEMU 工作集 73.1 → 73.5 MB（+0.4 MB，+0.5%，已平台期），
  线程 5–8，说明没有明显的宿主侧泄漏或线程增长。
- **观察项（不计 FAIL）**：QEMU CPU 时间从 309 s（t=5）线性增长到
  3586 s（t=60），约 99% 单核。即客户机“空闲”时宿主仍持续占用 1 核；
  可能是空闲循环/定时器或某个 ring3 进程轮询所致，本 WP 未定位
  （不改变 1h 稳定性结论，留给后续 CPU/功耗专项）。

### 异常现场

无。未发生 PANIC、冻结、重启、QMP 失联或命令超时；无需保留异常截图。

## 判定

| 验收项 | 结果 |
|---|---|
| 1 小时连续运行 | **PASS**，60.17 min |
| 进程存活 | **PASS**，12/12 采样存活，PID 不变 |
| 串口记录 | **PASS**，20,557 → 21,229 B，空闲零增长 |
| 每 15 分钟命令仍响应 | **PASS**，6/6（196–486 ms） |
| PANIC / 冻结 / 重启 | **PASS**，0 / 0 / 0 |
| 内存/进程趋势 | **PASS**，宿主 WS +0.4 MB 平台期；CPU 观察项见上 |
| 正常结束 | **PASS**，QMP `quit`，`qemu.err` 0 B |
| 范围约束 | **PASS**，只写 `obj-ng/smoke/**` 与本报告；不改 `src/**`、`tools/**`、`build.ps1`、`sdk/**`；不 push；不删 `dist-ng/`；1 小时到点结束（不延长到 8h） |

## 产物

| 路径 | 内容 |
|---|---|
| `obj-ng/smoke/smoke-1h.ps1` | 1h smoke harness：启动/采样/QMP 注入/判定/退出（15,478 B） |
| `obj-ng/smoke/1h.log` | 串口全文，507 行 / 21,229 B |
| `obj-ng/smoke/samples.log` | 12 条采样 + 6 条注入记录（3,043 B） |
| `obj-ng/smoke/summary.json` | 机读汇总（镜像哈希、参数、12 采样、6 响应）（15,327 B） |
| `obj-ng/smoke/build.log` | 隔离构建日志（9,248 B，`build-ng: PASS`） |
| `obj-ng/smoke/qemu.args.txt` / `qemu.pid` / `qemu.err` | 运行参数 / PID 5064 / 空 stderr |
| `obj-ng/smoke/dist/aster-ng.elf`、`dist/disk.img` | 被测镜像与磁盘（隔离，不覆盖 `dist-ng/`） |
| `obj-ng/smoke/pre-run.txt` | 运行前 HEAD/工作区/QEMU 状态快照 |
| `docs/claims/ng-WP4-4.md` | 本报告 |

## 复跑

```powershell
# 构建（隔离目录，避免覆盖并行会话的 dist-ng/obj-ng）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-ng.ps1 `
    -Repo D:\OS_Project -ObjDir obj-ng/smoke/obj -DistDir obj-ng/smoke/dist

# 1 小时 smoke（退出码 0 = PASS；约 61 分钟）
powershell -NoProfile -ExecutionPolicy Bypass -File obj-ng/smoke/smoke-1h.ps1 `
    -Repo D:\OS_Project -Minutes 60 -SampleSec 300 -InjectEveryMin 15
```

## 并行会话与范围说明

- 会话开始时（17:56）工作区干净（仅未跟踪的 `obj-ng/`），且无其它 QEMU；
  运行期间并行会话 WP4-1/WP4-2/WP4-3 在本工作区提交了 ABI/release/SDK 示例
  文档与工具（HEAD 9a6ab48 → 7401160），**均未改 `src/**` 或构建脚本**，
  不影响被测镜像；`dist-ng/` 的 18:09 更新来自 WP4-3，本会话未写它。
- 本会话新增/修改仅：`obj-ng/smoke/**`（未跟踪目录）与
  `docs/claims/ng-WP4-4.md`；提交用
  `tools/session-commit.ps1 -Name ng -Paths docs/claims/ng-WP4-4.md`，不 push。
