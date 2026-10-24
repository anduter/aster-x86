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

- `build.ps1` 固定使用 `obj/`、`dist/`、QEMU QMP 端口 **4444** 和 `serial.log`；
  两个会话同时跑会互相覆盖产物、抢端口。
- 约定：**同一时刻只允许一个会话运行 `build.ps1`**。
  运行前先确认：无 `qemu-system-i386` 进程、`serial.log` 不被占用。
- 手工 QEMU 验证请用各自端口段：
  - 本会话：monitor `4480+`、serial `5580+`
  - 新会话：monitor `4490+`、serial `5590+`

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
| 另一会话 | fs/VFS、TCP echo、kernel.c | 2026-10-24 | 进行中 |

## 7. 已完成里程碑（本会话）

- `[smp] IOAPIC driver`（`aeec408`）
- `[smp] IRQ12 → IOAPIC`（鼠标，`cb378f5`，monitor `mouse_move` 验证）
- `[smp] sched_wake + AP tick 校准`（`e712581`，2s 时 bsp=200 ap=195）
- `[smp] IRQ1 → AP`（键盘亲和性，`54ac8fa`，注入按键测试 86/86）

## 8. 待提交（工作区）

- `intr/syscall.c`：UDP 收包改为“短轮询 + sleep 让出”，避免 IF=0 长自旋触发 NMI 看门狗（与另一会话的 VFS chroot 改动混在同一文件，提交时注意拆分）；
- `build.ps1`：harness 用 QMP `quit` 正常退出 QEMU 后再读串口（修 `-serial file:` 缓冲导致的假失败）。

| fs 会话 | 文件系统 / VFS 后端接口（`fs/*`） | 2026-10-24 | 进行中 |
| 新会话 | （待登记） | | |
