# ipc 会话 claim（P1：IPC + capability）

- 批次：2026-10-30 三层架构 P1（`docs/design/three-layer.md` §3/§4）
- 基线 HEAD：`443dba1`（v0.74.2 / 254 项检查 / `ci -Full` 绿）
- 允许改动范围：`src/kernel/svc/**`、`src/kernel/include/svc.h`、
  `src/user/ulib.*`、`src/user/user_shell.c`（`ipc` 演示命令）、
  `docs/claims/ipc.md`
- 未改：`include/abi.h`（80–84 编号已定）、`build.ps1`、`core/**`、
  `net/**`、`fs/**`、`proc/**`

## 1. 改动文件清单

| 文件 | 改动 |
|---|---|
| `src/kernel/svc/ipc.h` | 新增：消息头/端口/cap 契约常量与内核内部 API |
| `src/kernel/svc/ipc.c` | 新增：端口表、16 深队列、消息校验、有界阻塞、ticket、cap 授予、内核自检 |
| `src/kernel/svc/svc_syscall.c` | 新增 80–84 的 `owns()` 与 handler；用户指针拷贝/缓冲校验 |
| `src/kernel/svc/supervisor.c` | `supervisor_entry()` 在 `-Test` 构建里跑一次 `ipc_selftest()`（调度器已启动的进程上下文） |
| `src/kernel/include/svc.h` | 导出 `svc/ipc.h` |
| `src/user/ulib.h` `src/user/ulib.c` | `port_create/send/send_raw/recv/reply/reply_raw/cap_grant/port_ticket` + 错误码；补 `memcmp` |
| `src/user/user_shell.c` | 新增 `ipc` 演示命令；`-Test` 的 `sectest` 自动跑 ring3 ping/reply/EINTR/cap 回归 |

## 2. 实现要点

### 2.1 消息头（设计 §3.1，固定 16B 小端）

| 偏移 | 字段 | 校验 |
|---|---|---|
| 0 | `u32 magic` | 必须 `0x49504331`（"IPC1"） |
| 4 | `u32 type` | 服务自定义 |
| 8 | `u16 flags` | 仅 bit0 NEED_REPLY / bit1 NO_REPLY；互斥；其它位 → `-1` |
| 10 | `u16 ncaps` | ≤ 4，且 `16 + len + ncaps*8 == 整帧长度` |
| 12 | `u32 len` | payload ≤ 4096 |

cap 数组每项 8B（`u32 type; u32 id;`）：额外校验 `type ∈ 1..6`、
`MMIO/DMA` 的 `id != 0`、`IRQ` 的 `id < 256`。P1 只校验，不把 cap
安装给接收方（见 §7 设计问题 #4）。

### 2.2 端口语义

- `SYS_PORT_CREATE(flags)` → 16 位不透明句柄 `(generation<<4)|slot`；
  `flags` 仅 bit0（私有），其它位 `-1`；表满时先回收 owner 已消失的端口。
- `SYS_PORT_SEND(port, msg, len)`：`len` 是**整帧**长度；公共端口任何
  uid 可发，私有端口仅同 uid/root；队列 16 条，满则每 10ms 轮询等待，
  200ms（`IPC_SEND_TIMEOUT_TICKS=20`）后 `-EAGAIN`，未屏蔽信号打断
  `-EINTR`（-4）。
- `SYS_PORT_RECV(port, buf, maxlen)`：仅 owner uid/root；空队列有界阻塞
  300ms（`IPC_RECV_TIMEOUT_TICKS=30`）→ `-EAGAIN`；返回消息**真实长度**，
  `maxlen` 不足时只回填 `maxlen` 字节（截断，见设计问题 #8）。
- `SYS_PORT_REPLY(ticket, msg, len)`：ticket = `(pid<<16)|port_handle`；
  校验句柄 generation、端口 `owner_pid == pid`、发送权限，然后按 SEND
  的队列/超时规则入队。
- 句柄复用：端口释放/回收后 generation+1，旧句柄立即失效。

### 2.3 capability（设计 §3.3）

`SYS_CAP_GRANT(port, type, id, rights)`：把“自己持有”的 cap 记到目标
端口的 grant 表（每端口 8 条，重复 `(type,id)` 合并）。目标端口必须
可被调用方管理（同 uid / root）。类型：

| type | 值 | “持有”的判定 | 额外特权 |
|---|---|---|---|
| `CAP_PORT` | 1 | `id` 是调用方自己创建的端口句柄 | — |
| `CAP_MMIO` | 2 | root 视为持有全部硬件 cap，`id != 0`（基址） | root |
| `CAP_IRQ` | 3 | 同上，`id < 256` | root |
| `CAP_DMA` | 4 | 同上，`id != 0`（长度） | root |
| `CAP_FILE` | 5 | `id` 是调用方 fd 表中的普通文件 fd | 授出 rights ⊆ fd rights |
| `CAP_SOCKET` | 6 | `id` 是调用方 fd 表中的 UDP fd | — |

未实现类型（0 或 >6）→ `-1`。`rights` 位 `READ/WRITE/GRANT/ADMIN`；
0 = 默认 RW；FILE 与源 fd 求交集后为 0 → `-1`。

### 2.4 syscall 分发

`svc_syscall_owns()` 认领 80–84；`syscall_dispatch` 未认领的编号统一
`-1`（不 panic/越界）。SEND/REPLY 先把整帧 `copy_from_user` 到 4KB
栈缓冲再进核心；RECV 睡前用 `user_range_ok` 校验用户缓冲，核心返回后
`copy_to_user`。坏指针/坏长度返回 `-1`。

### 2.5 ulib

`port_create / port_send / port_send_raw / port_recv / port_reply /
port_reply_raw / cap_grant / port_ticket`；`IPC_MAGIC/HDR_SIZE/
PAYLOAD_MAX/NCAPS_MAX/...`、`IPC_EINTR(4)`、`IPC_EAGAIN(11)`。
`port_send/port_reply` 自动组 16B 头（`ncaps=0`）。
`cap_grant` 的 rights 走 `esi`（内核中断现场保存了 esi）。

## 3. 期望自检行（供后续集成会话加进 `build.ps1` 的 checks）

内核侧（`-Test` 构建、supervisor 线程启动后跑一次）：

```text
ipc: port send/recv ok (port=<id> len=<n>)
ipc: reply ok (ticket=0x<hex>)
ipc: queue full ok (depth=16)
ipc: timeout ok (send 20 ticks, recv 30 ticks)
ipc: bad msg rejected
ipc: recv EINTR ok
ipc: send EINTR ok
ipc: cap grant ok (PORT/MMIO/IRQ/DMA)
ipc: cap grant denied uid=1000 (MMIO/IRQ/DMA)
ipc: cap type unsupported rejected
ipc: handle generation ok
ipc: selftest PASS
```

ring3 侧（`sectest` 在 `-Test` 构建自动调用）：

```text
ipc: ring3 server reply ok
ipc: ring3 port ping ok
ipc: ring3 reply ok
ipc: ring3 recv EINTR ok
ipc: ring3 cap grant root ok
ipc: ring3 cap grant user denied ok
ipc: ring3 selftest PASS
```

建议的 `build.ps1` 断言（前缀匹配即可，不依赖 id/长度）：

```powershell
@{ Name = 'IPC 端口收发';       Ok = ($out -match 'ipc: port send/recv ok') },
@{ Name = 'IPC 回复';           Ok = ($out -match 'ipc: reply ok') },
@{ Name = 'IPC 队列满/超时';    Ok = (($out -match 'ipc: queue full ok \(depth=16\)') -and ($out -match 'ipc: timeout ok')) },
@{ Name = 'IPC 坏消息拒绝';     Ok = ($out -match 'ipc: bad msg rejected') },
@{ Name = 'IPC EINTR';          Ok = (($out -match 'ipc: recv EINTR ok') -and ($out -match 'ipc: send EINTR ok')) },
@{ Name = 'IPC cap 授予';       Ok = (($out -match 'ipc: cap grant ok') -and ($out -match 'ipc: cap grant denied uid=1000')) },
@{ Name = 'IPC 内核自检';       Ok = ($out -match 'ipc: selftest PASS') },
@{ Name = 'IPC ring3 ping/reply'; Ok = (($out -match 'ipc: ring3 port ping ok') -and ($out -match 'ipc: ring3 reply ok')) },
@{ Name = 'IPC ring3 EINTR';    Ok = ($out -match 'ipc: ring3 recv EINTR ok') },
@{ Name = 'IPC ring3 自检';     Ok = ($out -match 'ipc: ring3 selftest PASS') }
```

## 4. 自检覆盖（内核侧）

在 `supervisor_entry()` 里（调度器已启动、可阻塞/超时/收信号）：

1. 建 `srv`/`cli` 两个端口 → 发（payload 带 ticket）→ 收 → 校验
   magic/ticket/length → `ipc: port send/recv ok`。
2. 用收到的 ticket `REPLY` 回 `cli` → 收到 "pong" → `ipc: reply ok`。
3. 灌满 16 条 → 第 17 条 200ms 后 `-EAGAIN`（并测实际等待 tick 数）→
   `ipc: queue full ok` / `ipc: timeout ok`；再把 16 条全部收回，验证不丢。
4. 空端口 `RECV` 300ms 后 `-EAGAIN`（同上 timeout 行）。
5. 坏帧：短帧、坏 magic、payload 4097、ncaps=5、长度不自洽、保留
   flags=0x80 → 全部 `-1` → `ipc: bad msg rejected`。
6. `pending_sig=SIGINT` 模拟未屏蔽信号：阻塞中的 RECV、满队列的 SEND
   都返回 `-4` → `ipc: (recv|send) EINTR ok`（真实信号路径由 ring3 覆盖）。
7. cap：root 授 `PORT/MMIO/IRQ/DMA` 成功并回读；`FILE(0)`（控制台）/
   `SOCKET(0)` 因“不持有”被拒；euid=1000 时对自己拥有的临时端口授
   `MMIO/IRQ/DMA` 全部 `-1`；type 0/7 被拒 → 三行 cap 结论。
8. 释放端口后旧句柄失效、重建得到不同 generation → `ipc: handle generation ok`。
9. 汇总 `ipc: selftest PASS`。

## 5. ring3 样例（`ipc` 命令 / `sectest` 自动跑）

```c
/* driver（fork 出来的进程） */
int srv = port_create(0);                  /* 公共端口 */
int cli = port_create(0);
int ticket = port_ticket(cli);             /* (pid<<16)|cli */
port_send(srv, 1, IPC_F_NEED_REPLY, &ticket, 4);

if (fork() == 0) {                         /* server */
    unsigned char buf[128];
    int n = port_recv(srv, buf, sizeof(buf));
    int t; memcpy(&t, buf + IPC_HDR_SIZE, 4);
    port_reply(t, 2, "pong", 4);
    exit(0);
}
n = port_recv(cli, buf, sizeof(buf));
/* buf+16 == "pong" -> "ipc: ring3 port ping ok" / "ipc: ring3 reply ok" */
```

另测：阻塞 `port_recv` 被父进程 `SIGINT` 打断（handler 已装）→ `-4`；
root `cap_grant(srv, CAP_MMIO, ...)` 成功，子进程 `setuid(1000)` 后对
自己新建的端口授 MMIO 被拒。driver 退出后端口由内核在后续 create 时
按 owner 消失回收，反复跑不会耗表。

## 6. 回归结果

- `tools\session-run.ps1 -Name ipc -Test`：**254/254 PASS**
  （`=== TEST PASS (254/254) ===`，2026-10-31 00:14，`.obj-ipc`/`.dist-ipc`，
  `serial-ipc.log`）。基线 254 项全绿；新增自检行随 `-Test` 串口输出，
  未改 `build.ps1` 检查项。
- 第 1 次跑（00:09）出现 **18 项失败**，串口显示 shell 在
  `sectest_sigchld_cow()` 处 `sigreturn rejected (flags=10246)` 后跳到蹦床
  `eip=0x40307007` 被 #PF 杀掉，导致后续 `sectest`/`reexec` 检查全灭。
  同一晚基线文档（`docs/parallel-sessions.md`“已知偶发问题”）记录的正是
  用户态偶发 #PF（不同地址、同属信号帧/COW 竞态），与本会话改动无直接
  关联；第 2 次跑 254/254 全绿。该 flake 需 proc/信号会话单独定位，见 §8。
- `ci.ps1 -Full -StrictBom` 由父会话在 P2（dmesg）与本会话都提交后统一
  执行（共享树里 P2 未提交文件会让 ci 的 clean 检查必红；本会话按父会话
  指示只跑 `session-run -Test`，不单独跑 `ci -Full`）。

## 7. 设计问题（未改契约，记录待评审）

1. **端口命名空间两套**：`svc/port.c`（native ABI `PORT_CONNECT/ACCEPT`，
   accept 队列）与本模块（SYS_PORT_* 消息队列）当前是两套 id 表。
   `service_register/lookup` 登记的是 port.c 的 id，P3 需要统一端口对象/
   注册表，否则“按名查服务再 SEND”接不上。
2. **ticket 没有独立字段**：ABI 1.5 的 SEND/RECV/REPLY 参数位不够，
   设计 §3.2 说“ticket 来自请求消息”但消息头也没有 ticket。P1 约定
   `ticket=(pid<<16)|port_handle`，由请求方放进业务 payload；内核校验
   句柄 generation + `owner_pid`。限制：服务可伪造任意 `(pid,handle)`
   向该端口 reply；ticket 无一次性/过期语义，可重复 reply。建议 ABI 1.6
   给消息头加 ticket 字段或让 RECV 输出 ticket。
3. **CAP_GRANT 的 rights 无参数位**：设计 §3.3 有 `rights`，`abi.h` 注释
   只有 3 参。P1 用 `esi` 传（0=默认 RW，掩到 0xF）；纯 3 参调用方的
   `esi` 是任意值时 rights 可能非预期。建议 ABI 1.6 明确第 4 参。
4. **SEND 的 caps / RECV 的 caps_out 无参数位**：cap 数组内嵌消息并校验，
   但没有安装到接收方（没有接收方 cap 表可挂）；`SYS_CAP_GRANT` 只把
   grant 记在端口上，P2 之前没有消费者。
5. **CAP_SYS_ADMIN 未落地**：进程凭据里没有 admin cap（`PKG_CAP_SYS_ADMIN`
   是包 manifest 权限位），P1 的 MMIO/DMA/IRQ 只能“root 或拒绝”。
6. **阻塞是 1 tick 轮询**：`proc_block()` 没有超时参数、`proc/**` 不在
   可改范围，因此 SEND/RECV 用 `proc_sleep_ticks(1)` 轮询；唤醒延迟
   ≤10ms。建议后续加 `proc_block_timeout(chan, ticks)`（proc 会话）。
7. **没有 SYS_PORT_CLOSE**：端口只能等 owner 退出后由 create 的 reaper
   回收；长驻服务无法主动关闭/降引用。建议 ABI 1.6 增加 close，或把
   端口挂到 fd/handle 表。
8. **RECV 截断**：`maxlen` 小于消息长度时返回真实长度但只回填 `maxlen`，
   没有 `MSG_TRUNC` 之类的显式标记；调用方需自己比较返回值与缓冲大小。
9. **上限固定**：16 端口 × 16 条 × 4144B ≈ 1.0MB 静态 BSS；每端口
   grant 表 8 条。端口表满依赖 reaper，不保证 O(1) create 成功。
10. **SEND 不强制 CAP_PORT**：公共端口任何 uid 可发（按设计 §3.2 的
    “私有端口才限 uid”）；cap 获取路径尚不存在，P1 不引入隐式强制。
11. **沙箱进程用不了 80–84**：`proc_t.syscall_mask` 只有 64 位，
    `syscall_dispatch` 对 sandboxed 进程 `>=64` 一律拒绝。P1 的 shell /
    服务都未沙箱，所以不影响；P3 把服务放进沙箱前，需要把 80–84 纳入
    allowlist（位图加宽，属 proc 模块）。
12. **RECV 回填失败会丢消息**：`user_range_ok` 只查地址窗口，不查映射；
    `ipc_port_recv` 出队后 `copy_to_user` 仍可能因坏页失败并返回 `-1`，
    该条消息已经从队列移除。调用方传坏缓冲会丢一条消息（P1 接受，
    后续可先 lock 用户页或加“未消费”回滚）。

## 8. 遗留

1. 端口/消息的 8h 浸泡未做（P1 只要求回归；P5 起统一浸泡）。
2. 零拷贝共享环、cap 撤销/传递、IRQ EOI 代理不在本阶段（设计 §10 开放问题）。
3. `-smp 4` 手工验证未单独做：内核自检在 supervisor 线程单点执行，
   端口表有自旋锁；并发 SEND/RECV 的多核压力留给 P2 服务化后补。
4. `docs/api.md` 的 ABI 1.5 说明未更新（不在本会话可改范围）；等父会话
   集成时统一补 80–84 的“已实现/参数约定”。
5. `build.ps1` 检查项未加（按规矩不改进度脚本）；§3 已给可直接粘贴的断言。

6. `sectest_sigchld_cow` 偶发：信号帧写到 fork 后的 COW 只读栈页时，
   内核可能以带 RF 的 #PF 现场组帧，`proc_sigframe_valid` 拒绝（`flags`
   含 bit16），用户态落在蹦床上被 #PF 杀掉（本次 run1 复现：
   `pid 7 sigreturn rejected (flags=10246)` → `eip=0x40307007` #PF）。
   属 proc/信号模块（不在本会话可改范围），需要单独修：组帧前清 RF 或
   用被中断的系统调用现场而不是 #PF 现场。与 `docs/parallel-sessions.md`
   的“已知偶发用户态 #PF”同族。

## 9. 串口摘录（`serial-ipc.log`，254/254 那次）

内核侧（supervisor 线程，调度器启动后）：

```text
ipc: port send/recv ok (port=16 len=20)
ipc: reply ok (ticket=0x10011)
ipc: queue full ok (depth=16)
ipc: timeout ok (send 20 ticks, recv 30 ticks)
ipc: bad msg rejected
ipc: recv EINTR ok
ipc: send EINTR ok
ipc: cap grant ok (PORT/MMIO/IRQ/DMA)
ipc: cap grant denied uid=1000 (MMIO/IRQ/DMA)
ipc: cap type unsupported rejected
ipc: handle generation ok
ipc: selftest PASS
```

ring3（`sectest` 内，driver/server 两个用户进程）：

```text
ipc: ring3 server reply ok
ipc: ring3 port ping ok
ipc: ring3 reply ok
ipc: ring3 recv EINTR ok
ipc: ring3 cap grant root ok
ipc: ring3 cap grant user denied ok
ipc: ring3 selftest PASS
```

