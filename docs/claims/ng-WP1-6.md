# ng-WP1-6：IPC / 句柄 / 端口 / ticket（PASS）

## 结论

**PASS** —— ng 内核在 WP1-5 的线程/调度之上新增控制面 IPC：

- **句柄表**：64 槽、`(generation<<16)|slot` 编码、类型化（PORT/THREAD/FILE
  预留），close 递增 generation 使旧句柄/旧 ticket 失效；表满 `ENOSPC`、
  坏句柄/陈旧 generation/类型不匹配 `EBADF`。
- **端口**：`port_create(depth)`（默认 16，上限 64）、`send/recv/reply/close`；
  消息头固定 16 字节 `{type,len,from,flags}`、payload ≤ 4 KiB；FIFO 环形队列，
  满/空用等待队列 + `thread_sleep` 阻塞，超时 `ETIMEDOUT`、非阻塞 `EAGAIN`，
  close 唤醒全部等待者并返回 `ECANCELED`。
- **ticket**：`ticket = (port_generation<<16)|slot`，与端口句柄同构；`reply`
  校验 ticket 的槽位/generation 与端口对象一致（句柄槽复用后旧 ticket 拒绝）。
- **syscall**：`src/ng/syscall.h` 定义 0/1/10/11/12/13/14，`syscall_dispatch`
  接 `ipc_sys_*`；用户指针做页级 U/S 校验 + `stac/clac` SMAP 窗口 copyin/out，
  内核地址/null 返回 `EFAULT`。
- **自检**：两个内核线程真实 ping/pong 20 次；超时、队列满、坏句柄、close
  唤醒等待者、句柄/端口表满、用户指针全部通过，输出 `ng: WP1-6 selftest PASS`。
- **host 单测**：fake 环境驱动句柄表/端口队列/阻塞状态机，**810 checks,
  0 failures**（WP1-5 为 390，本 WP +420）。
- **回归**：旧 32 位 `build.ps1 -Test` **305/305**；`check-domains` /
  `check-deps` 0 违规；panic 负向路径仍 PASS。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/handle.h` / `handle.c` | 句柄表：编码、generation 递增/回绕、类型校验、表满、统计（**新**） |
| `src/ng/ipc.h` | 消息头/错误码/端口/等待者/`ipc_core`/`ipc_env`/内核与 syscall API（**新**） |
| `src/ng/ipc_core.c` | 纯状态机：端口环形队列、等待队列、阻塞/超时/close 唤醒、延迟回收（**新**） |
| `src/ng/ipc.c` | 内核 env（TSC/sleep/wake/kmalloc）、内核 API、用户指针校验 + SMAP copyin/out、`ipc_sys_*`（**新**） |
| `src/ng/ipc_selftest.h` / `ipc_selftest.c` | 内核自检与 10 条出口行（**新**） |
| `src/ng/syscall.h` | 调用号 0/1/10–14 与参数约定（**改**） |
| `src/ng/syscall.c` | dispatch 接 `ipc_sys_create/send/recv/reply/close`（**改**） |
| `src/ng/main.c` | `ipc_init()` + WP1-6 自检 + PASS 行（**改**） |
| `tests/unit-ng/test_ipc.c` | 句柄表/端口/阻塞状态机 fake 环境单测（**新**） |
| `tests/unit-ng/unit_main.c` | 加 `test_ipc()`（**改**） |
| `tools/unit-ng.ps1` | 加 `handle.c`/`ipc_core.c`/`test_ipc.c`（**改**） |
| `tools/build-ng.ps1` | 加 4 个 C 源，17 → 21 步（**改**） |
| `tools/run-ng.ps1` | 9 条固定行 + ipc 统计正则 + `ng: ipc .* FAIL` / WP1-6 失败模式（**改**） |
| `docs/design/ipc-ng.md` | 句柄/ticket/端口/阻塞状态机/syscall/自检/遗留（**新**） |
| `docs/design/decisions.md` | ADR-018（**追加**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`src/ng` 新增
文件不进旧对象列表。

## 环境

- 分支 `ng`，基线 HEAD `b0104dd`（WP1-5）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`），内核
  `--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector`；host 单测同一 clang 的默认 PE 目标。
- QEMU 11.0.0（`v11.0.0-12122-ga4bb4b10c9`），
  `qemu-system-x86_64 -kernel -m 256 -cpu max`（另跑 `-cpu qemu64` 验证
  SMAP=off 路径）。
- 时间源：`rdtsc`（与 WP1-5 相同，不做频率校准）；超时参数单位是 TSC tick。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP1-6) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe
  [1/21] entry.S
  [2/21] isr.S
  [3/21] switch.S
  [4/21] serial.c
  [5/21] memmap.c
  [6/21] pmm.c
  [7/21] heap.c
  [8/21] paging.c
  [9/21] gdt.c
  [10/21] trap.c
  [11/21] syscall.c
  [12/21] mem_selftest.c
  [13/21] sched_arch.c
  [14/21] sched.c
  [15/21] sched_selftest.c
  [16/21] handle.c
  [17/21] ipc_core.c
  [18/21] ipc.c
  [19/21] ipc_selftest.c
  [20/21] main.c
  [21/21] link -> dist-ng/aster-ng.elf

ELF64: PASS (70392 bytes)
  magic    7f 45 4c 46
  class    2 (ELF64)
  machine  62 (EM_X86_64)
  type     2 (ET_EXEC)
  entry    0x0000000000101000
  phnum    6
segments: PASS
  4 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
  header   file offset 0x1000, length 24, checksum 0, 8-byte aligned
pvh-note: PASS
  entry    0x0000000000101000 (PT_NOTE, Xen type 18; QEMU -kernel)

build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

WP1-2/3/4/5 全部保持 PASS。WP1-6 段（完整串口见运行日志）：

```text
ng: WP1-5 selftest PASS
ng: ipc ping/pong ok (n=20)
ng: ipc ticket ok
ng: ipc timeout ok
ng: ipc queue full ok
ng: ipc close wakes waiter ok
ng: ipc bad handle rejected
ng: ipc handle table ok (slots=64 ports=16)
ng: ipc user ptr ok
ng: ipc stat sends=25 recvs=45 replies=20 timeouts=2 eagains=4 bad=9 live=0
ng: WP1-6 selftest PASS
------------------------------------------------
run-ng: matched 33/33 expected lines + 6 patterns
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

`run-ng.ps1` 期望（全部命中才 PASS）：

```text
9 条固定行（WP1-6）：
  ng: ipc ping/pong ok (n=20)
  ng: ipc ticket ok
  ng: ipc queue full ok
  ng: ipc timeout ok
  ng: ipc bad handle rejected
  ng: ipc close wakes waiter ok
  ng: ipc user ptr ok
  ng: ipc handle table ok (slots=64 ports=16)
  ng: WP1-6 selftest PASS
1 条正则：
  ng: ipc stat sends=[0-9]+ recvs=[0-9]+ replies=20 timeouts=[0-9]+
  eagains=[0-9]+ bad=[0-9]+ live=0
失败早停模式新增：
  ng: ipc .* FAIL / ng: WP1-6 selftest FAIL
```

证据解读：

- **ping/pong**：P/Q 各一个默认深度 16 的端口；P 发 20 次请求（type=PING、
  payload 带自己的 ticket），Q `recv` 后用 `hdr.from` 调 `reply`，P 收到 20
  次 type=PONG 回复并逐条校验 payload；两线程都退出并被回收。
- **ticket**：`port_create` 的返回值解码后 `gen≥1`、`slot<64`，且与端口对象
  记录的 `slot/gen` 一致。
- **timeout**：空端口 `recv(timeout=5M tick)` 与满端口 `send(timeout=5M tick)`
  都返回 `ETIMEDOUT`，且耗时 ≥ timeout；非阻塞空队列返回 `EAGAIN`。
- **queue full**：depth=2 塞满后非阻塞 `send` 返回 `EAGAIN`；另一个线程永久
  阻塞 `send`，主线程 `recv` 腾位后该线程被唤醒并成功入队；最终 FIFO 顺序
  为 0x21/0x22/0x23。
- **close**：`recv` 永久阻塞的线程先被观察到 `THREAD_BLOCKED`，主线程
  `close` 端口后该线程返回 `ECANCELED` 并退出，端口回收。
- **bad handle**：close 后 `send/recv/reply/close` 全部 `EBADF`；句柄 0、
  `0xDEADBEEF`、槽位复用后的旧 ticket、THREAD 类型句柄也全部拒绝。
- **handle table**：端口池 16 个占满后 `port_create` 返回 `ENOSPC`；句柄表
  64 槽占满后 `handle_alloc` 返回 `ENOSPC`；关闭后 `live=0`。
- **user ptr**：用 WP1-3 映射的用户页 `0x40100000`（U|W|NX）做
  `ipc_sys_send`→`ipc_sys_recv` 的 copyin/copyout，内容逐字节一致；内核栈
  地址、null、`max=0` 分别返回 `EFAULT`/`EFAULT`/`EINVAL`。
- **stat**：`replies=20` 与 ping/pong 轮数一致，`live=0` 说明端口/句柄全部
  回收；`sends=25`（20 ping + 队列满 3 条 + 超时填充 1 条 + 用户指针 1 条）、
  `recvs=45`（P 20 + Q 20 + 队列 4 + 用户指针 1）。

### `-cpu qemu64`（SMAP=off）

```text
powershell -File tools/run-ng.ps1 -Cpu qemu64
```

```text
ng: smep=off smap=off
...
ng: ipc ping/pong ok (n=20)
ng: ipc timeout ok
ng: ipc queue full ok
ng: ipc close wakes waiter ok
ng: ipc bad handle rejected
ng: ipc handle table ok (slots=64 ports=16)
ng: ipc user ptr ok
ng: ipc stat sends=25 recvs=45 replies=20 timeouts=2 eagains=4 bad=9 live=0
ng: WP1-6 selftest PASS
run-ng: matched 33/33 expected lines + 6 patterns
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

SMAP=off 时 `user_copy_in/out` 跳过 `stac/clac`，直接访问 U 页（SMAP 关闭时
合法）；数字与 `-cpu max` 完全一致，说明 IPC/调度不依赖 SMAP。

### host 单测

```text
powershell -File tools/unit-ng.ps1
```

```text
=== unit-ng (WP1-6 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
810 checks, 0 failures
unit-ng: PASS (810 checks, 0 failures)
```

新增 `test_ipc.c`（fake `ipc_env`）覆盖：

- 句柄表：编码、槽位复用、gen 递增、16 位回绕跳过 0、类型校验、重复 close、
  表满 64、`bad_lookups/type_errors/full_rejects` 计数；
- 端口：默认/显式 depth、非法 depth、端口池 16、ring 分配失败回滚、owner uid、
  延迟回收；
- 队列：FIFO、depth 满、非阻塞 `EAGAIN`、`recv` 截断返回值/写入边界、
  消息头长度/`flags`/`from` 校验；
- 阻塞状态机：recv 被 send 唤醒、send 被 recv 唤醒、recv/send 超时、
  close 唤醒两侧、deadline 回绕保护、等待者池 20 次复用；
- 权限：同 uid/root/`EPERM`、被拒 close 不失效句柄；
- reply：`from` 往返、reply 独立计数、坏 ticket/已关闭 ticket；
- 上限：句柄满时 `port_create` 回滚端口槽。

### panic 路径回归

```text
tools/build-ng.ps1 -PanicTest  +  tools/run-ng.ps1 -Panic
```

```text
ng: panic selftest: triggering unrecoverable #UD
PANIC: vec=0x6 err=0x0 rip=0x10aebd cr2=0x10b000
run-ng: PASS (panic path observed in 0.2s, cpu max)
```

随后重新执行普通 `build-ng.ps1`，产物回到正常镜像（70392 bytes）。

### 旧树回归与边界检查

```text
powershell -File build.ps1 -Test
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 40.3s ===
=== TEST PASS (305/305) ===
```

```text
powershell -File tools/check-domains.ps1
checked: 205 files, 726 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
checked: 201 files, 650 quoted includes
module include violations: 0
OK: no cross-module include violations.
```

## 设计要点（详见 `docs/design/ipc-ng.md`）

### 句柄与 ticket

```text
handle = ticket = (generation << 16) | slot      slot ∈ [0,64)
```

- 槽位不压缩；close 把 generation +1（跳过 0）并清空槽位，旧句柄失效；
- 类型 PORT/THREAD/FILE，查找时校验类型；
- `reply(ticket)` 除句柄表校验外，还交叉校验 `port->slot/gen` 与 ticket 的
  `slot/gen`，句柄槽复用后旧 ticket 指不到新端口；
- 单进程/单表（内核线程共享），per-process 表留到进程模型 WP。

### 阻塞与唤醒

```text
等待者静态池（12）：tid/kind/notified/next
recv_waiters / send_waiters 两条 FIFO 链
send 成功 -> wake_one(recv_waiters)；recv 成功 -> wake_one(send_waiters)
close    -> wake_all(recv + send) -> 等待者返回 ECANCELED
超时      -> deadline = now + ticks（饱和）-> ETIMEDOUT
```

`notified` 位保证“两条消息叫醒两个不同等待者”；单核协作式下“检查条件 →
入队 → 再检查 → sleep”没有丢唤醒窗口。close 后端口不立即释放：等待者
只看 `closed`，最后一个等待者退出后的下一次 IPC 操作才 `free(ring)` 并回收
槽位。

### 用户指针

```text
va != 0 && 无溢出 && 末地址 <= 0x7FFF_FFFF_FFFF
每一页 pg_walk：P=1 && U/S=1（recv 还要 W=1）
SMAP on 时 stac/clac 包住逐字节 copyin/copyout
```

`msg` 先拷贝到内核缓冲再进状态机；`recv` 用内核缓冲收、copyout 后返回
真实长度。ring3 的 `syscall` 指令往返留给 WP1-7（本轮在 handler 边界用真
用户 VA 验证 copyin/out）。

## 自检项清单

1. 句柄表 64 槽：编码/gen/类型/重复 close/表满/陈旧 generation 全部符合预期。
2. 端口创建：默认深度 16、显式深度、上限 64、池 16、非法参数、分配失败回滚。
3. 两个内核线程 ping/pong 20 次：请求/回复 20/20，payload/type/from 全对。
4. 队列满：非阻塞 `EAGAIN`；阻塞 send 被 recv 唤醒并成功；FIFO 顺序正确。
5. 超时：空端口 recv、满端口 send 均 `ETIMEDOUT` 且耗时 ≥ timeout。
6. close：唤醒 recv 等待者（`ECANCELED`）、清空队列、句柄/旧 ticket 失效。
7. 用户指针：U 页 copyin/copyout 正确；内核地址/null `EFAULT`；`max=0`
   `EINVAL`。
8. host 单测 810 checks / 0 failures；旧 32 位 305/305；check-domains 0。

## 未做 / 遗留（按 WP 边界）

- **无进程/uid**：权限逻辑按“同 uid 或 root”写好并单测，内核路径一律 root；
  真实 uid 在进程模型 WP 接。
- **无 capability/跨进程 ticket**：ticket 只在单张句柄表里有效；`cap_grant`
  与跨进程端口传递是后续 WP。`reply` 的 slot/gen 交叉校验是接 cap 表的挂点。
- **`from` 防伪**：发送方可填表内任意存活 PORT；单进程可接受，跨进程必须由
  内核填或 cap 校验。
- **用户态多线程**：syscall 仍用 WP1-3 的单个静态内核栈；阻塞式 syscall 在
  当前单用户线程下可用，per-thread 内核栈留给 WP1-7+。
- **ring3 IPC 程序**：ELF64/ring3 应用属于 WP1-7；本轮只到 handler 边界。
- **数据面/共享内存**：消息 ≤4 KiB 控制面；大块传输后续 WP。
- **超时单位**：TSC tick，未校准；ABI 冻结时改毫秒。
- **抢占**：仍协作式；tick IRQ 未接（与 WP1-5 遗留相同）。
- **端口 flags/private**：`port.flags`、`IPC_FLAG_*` 预留未实现。
- **`wait_any`/批量 recv**：只做单端口阻塞 recv。

## 出口

```text
powershell -File tools/build-ng.ps1    # PASS，dist-ng/aster-ng.elf（70392 bytes，21 步）
powershell -File tools/run-ng.ps1      # PASS，33 条期望行 + 6 条正则
powershell -File tools/unit-ng.ps1     # PASS，810 checks, 0 failures
powershell -File build.ps1 -Test       # PASS，305/305（旧 32 位）
powershell -File tools/check-domains.ps1   # 0 违规
powershell -File tools/check-deps.ps1      # 0 违规
```
