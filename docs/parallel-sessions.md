# 多会话并行开发约定

本仓库可能同时被多个 DSH 会话/子代理修改。工作区只有一份（`D:\OS_Project`），
`obj/`、`dist/`、`serial.log`、QEMU 端口都是共享资源。以下是约定，避免互相覆盖。

## 1. 当前状态（写本文时）

- HEAD：`8e72a57 fs: 断开 fs->proc 依赖…`
- 工作区有多个来源的未提交改动（`git status` 可查）。
- **进行中**：
  - UDP 用户态修复（子代理）：`core/net.c`、`core/net.h`、`e1000.*`、
    `intr/syscall.c` 的 UDP/socket 分支、`user/user_shell.c` 的 sectest UDP 段；
  - SMP/IOAPIC（本会话）：`core/ioapic.*`（新增）、`core/madt.c`、
    `proc/sched.c`、`core/smp.*`、`core/percpu.*`、`intr/irq.c`。

## 2. 文件所有权（建议）

| 区域 | 归属 | 文件 |
|---|---|---|
| SMP / 中断路由 | 本会话 | `core/smp.*`、`core/percpu.*`、`core/madt.c`、`core/ioapic.*`、`proc/sched.c`、`intr/irq.c` |
| 网络 / socket | UDP 子代理（完成后释放） | `core/net.*`、`core/e1000.*`、`intr/syscall.c` 的 UDP/socket 分支 |
| 文件系统 / VFS | fs 会话 | `fs/*`（`fs.h`、`vfs.c`、`ramfs.c`、`fat16.c`）、`block/*` 的只读使用 |
| 其它区域 | 新会话（请在下方登记） | 待定 |

**共享文件**（改前先声明，避免同时改）：
`kernel.c`、`build.ps1`、`Makefile`、`user/user_shell.c`、`intr/syscall.c`（整文件）、
`proc/process.c`、`proc/proc.h`。

## 3. 构建与测试必须串行

`build.ps1` 已参数化，**编译产物和 QEMU 不再共享**（2026-10-24 起）：

| 参数 | 默认 | 作用 |
|---|---|---|
| `-ObjDir` | `obj` | 目标文件目录（含 `nasm_paths.inc`、签名 C 文件） |
| `-DistDir` | `dist` | `floppy.img`、`disk.img`、`fat.img`、`tcp_host.log` |
| `-QmpPort` | `4444` | QEMU QMP（按键注入、截屏、正常退出） |
| `-SerialLog` | `serial.log` | 串口日志路径 |

各会话固定一套（建议写进自己的常用命令）：

```powershell
.\build.ps1 -Test -ObjDir .obj-a -DistDir .dist-a -QmpPort 4481 -SerialLog serial-a.log
.\build.ps1 -Test -ObjDir .obj-b -DistDir .dist-b -QmpPort 4491 -SerialLog serial-b.log
```

- **仍必须串行的只有 `-Test` 的网络回显**：客户机把回显端口 12345 编进了内核
  （`NET_UDP_LOCAL_PORT` / `NET_TCP_PORT` / `user_shell.c`），宿主监听也固定 12345，
  两个 `-Test` 同时跑会互相吃包。
  脚本现在会在启动 QEMU 前探测 12345 与 QMP 端口，被占用就**立刻报错退出**，
  而不是跑出一堆假失败。看到“宿主 12345 端口已被占用”就等对方跑完。
- **纯构建（不带 `-Test`/`-Shot`）可以完全并行**，已验证两路同时构建互不影响。
- 手工 QEMU 验证仍用各自端口段：本会话 monitor `4480+`、serial `5580+`；
  新会话 monitor `4490+`、serial `5590+`。

## 4. Git 约定

- **只提交自己的文件**：`git add <明确路径>`，禁止 `git add -A` / `git commit -a`。
- 提交信息加前缀：`[smp]`、`[net]`、`[fs]`、`[docs]`，方便区分来源。
- 禁止 `git reset --hard`、`git checkout -- .`、`git rebase` 等会丢弃他人改动的操作。
- 提交前跑 `git diff --check`；有编译能力时至少跑 `-fsyntax-only`。

## 5. 交接方式

- 完成一块后：提交 + 在会话里说明改动文件与验证结果；
- 需要动共享文件时：先在会话中说明“我要改 X”，等对方确认或错开时间；
- 发现他人文件有 bug：不要直接改，记录到本文或会话消息里，由归属方修。

## 6. 登记表

| 会话 | 负责区域 | 开始时间 | 状态 |
|---|---|---|---|
| 本会话 | SMP / IOAPIC / IRQ 路由 | 2026-10-24 | 进行中 |
| UDP 子代理 | 用户态 UDP 接收 | 2026-10-24 | 已完成（net.c/net.h/syscall.c） |
| fs 会话 | 文件系统 / VFS 后端接口（`fs/*`） | 2026-10-24 | 进行中 |
| 工程监理会话 | 进度汇报、完整度核查、文件整理（不改业务代码） | 2026-10-24 | 进行中 |
| 构建会话 | `build.ps1` 产物 / QMP / 串口参数化（见第 3 节） | 2026-10-24 | 已完成（并行构建 + 97/97） |
| 新会话 | （待登记） | | |

## 7. 已完成里程碑（本会话）

- `[smp] IOAPIC driver`（`aeec408`）
- `[smp] IRQ12 → IOAPIC`（鼠标，`cb378f5`，monitor `mouse_move` 验证）
- `[smp] sched_wake + AP tick 校准`（`e712581`，2s 时 bsp=200 ap=195）
- `[smp] IRQ1 → AP`（键盘亲和性，`54ac8fa`，注入按键测试 86/86）

## 8. 待提交（工作区）

- `intr/syscall.c`：UDP 收包改为“短轮询 + sleep 让出”，避免 IF=0 长自旋触发 NMI 看门狗（与另一会话的 VFS chroot 改动混在同一文件，提交时注意拆分）；
- `build.ps1`：harness 用 QMP `quit` 正常退出 QEMU 后再读串口（修 `-serial file:` 缓冲导致的假失败）。

## 9. 监理会话（2026-10-24）

- 报告：`docs/status-2026-10-24.md`（HEAD、版本、测试证据、未提交清单、文档漂移、整理动作）；
- 已整理：根目录 50 个历史日志 → `logs/archive/`；`.gitignore` 增加 `/logs/`、`/.soak-smp/`；
- 已记录风险：工作区 763 行未提交，`syscall.c`/`build.ps1`/`process.c`/`user_shell.c` 有跨会话混改；
- 未能整理：`gdb.txt`、`windd.exe`（根目录 ACL 拒绝移动）。

## 10. 回归效率与协作工具（2026-10-24 18:4x）

| 优化 | 效果 |
|---|---|
| uild.ps1 增量编译（源文件/头文件未变则跳过；参数变化自动全量） | 全量构建 10.8s → **2.3s** |
| 宿主回显改成可中断轮询（ReceiveTimeout + Pending()），收尾 Stop-Job 不再卡阻塞调用 | -Test **253s → 13.5s** |
| 去掉固定 2s 等待，改为等待旧 QEMU 退出 | 少 2s |
| 阶段计时 [t  N.Ns] + 末尾 compile/elapsed | 可直接定位慢点 |

### 多会话工具

- 	ools/session-status.ps1：一屏看板（git、运行中的 build/QEMU、端口占用、各 .obj-* 的 kernel.bin、最近 serial 日志与结论、测试锁）。
- 	ools/session-run.ps1 -Name a -Test：自动用隔离目录（.obj-a/.dist-a/serial-a.log + QMP 4481）运行；-Test 会自动抢 .session/test.lock，别的会话在跑就等待，不再直接失败。

### 已知偶发问题（未修）

约每几次 -Test 会出现一次用户态 #PF err=15 (reserved bit) at eip=0x4000415c，shell 被杀，导致后续检查失败（eexec 哨兵等不到）。这是独立的用户态/页表问题，与构建/测试优化无关，需要单独定位。