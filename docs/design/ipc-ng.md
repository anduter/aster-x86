# ng IPC / 句柄 / 端口（WP1-6）

> 状态：已实现（2026-10-31，ng `b0104dd` 之后）。
> 代码：`src/ng/handle.[ch]`、`ipc.[ch]`、`ipc_core.c`、`ipc_selftest.[ch]`；
> host 单测：`tests/unit-ng/test_ipc.c`；出口：`ng: WP1-6 selftest PASS`。
> 相关：architecture.md §IPC、`docs/design/three-layer.md` §3、旧树
> `docs/claims/ipc.md`（本 WP 的语义 oracle）。

## 1. 范围与边界

WP1-6 只做**控制面**（消息 ≤ 4 KiB）：句柄表、端口消息队列、阻塞/超时/
close 唤醒、ticket/reply、5 个 IPC syscall 与用户指针校验。不做数据面、
不做共享内存、不做跨进程 capability、不做用户态多线程（见 §10）。

分层与 WP1-5 的调度器一致：

```text
        syscall_dispatch（isr.S -> syscall.c）
                    |  ipc_sys_*（ipc.c） 用户指针校验 + copyin/copyout
                    v
  ipc_port_*（ipc.c, uid=root） ---> ipc_core_*（ipc_core.c，纯状态机）
                                          |
                                          v
                                   struct ipc_env（6 个钩子）
                           内核：rdtsc/thread_sleep/thread_wake/kmalloc
                           host：fake 时钟/fake sleep/记录 wake/malloc
        handle_*（handle.c，纯数据结构）<-- 被 ipc_core 与 host 单测直接使用
```

`ipc_core.c` 不 include 串口/heap/线程/页表，因此 host 单测可以在没有内核、
没有 QEMU 的情况下确定性地驱动整个阻塞状态机。

## 2. 句柄表（`handle.h` / `handle.c`）

### 2.1 编码

```text
handle = (generation << 16) | slot          slot ∈ [0, HANDLE_MAX=64)
```

- `slot` 是表内下标；`generation` 每次 close 递增，16 位回绕时跳过 0
  （0 永远表示“无效句柄”）。
- 句柄 0 永远非法：free 槽的 generation 初始为 0，首次分配才置 1。
- 槽位不压缩、不搬移：close 只把槽位标空并递增 generation，后续 alloc
  复用最小空槽。旧句柄因为 generation 不匹配被拒（stale handle）。

### 2.2 类型

| 类型 | 值 | 本 WP 语义 |
|---|---|---|
| `HANDLE_TYPE_PORT` | 1 | 端口；有完整对象（队列/等待者/uid） |
| `HANDLE_TYPE_THREAD` | 2 | 预留；只占槽位，无对象语义 |
| `HANDLE_TYPE_FILE` | 3 | 预留；同上 |

`handle_lookup(t, h, type, &obj)` 同时校验槽位、generation、类型；类型不匹配
与坏句柄一样返回 `HANDLE_EBADF`（避免把 THREAD 句柄当 PORT 用）。
`handle_lookup_any()` 供 close/诊断使用。

### 2.3 失败路径

| 情况 | 返回 |
|---|---|
| 表满（64 槽） | `HANDLE_ENOSPC`（并计 `full_rejects`） |
| 非法类型 / NULL 对象 | `HANDLE_EINVAL` |
| 槽位越界 / gen=0 / 空槽 / gen 不匹配 | `HANDLE_EBADF` |
| 类型不匹配 | `HANDLE_EBADF`（计 `type_errors`） |
| 重复 close | `HANDLE_EBADF` |

### 2.4 作用域

ng 现在只有“一个内核地址空间 + 内核线程”，`ipc.c` 持有一张全局表，所有
内核线程共享（自检里 P/Q/主线程用同一张表传递 ticket）。等 WP1-7 之后有
进程，改成每进程一张表；`handle_table` 的 API 不需要变。

## 3. 端口与消息（`ipc.h` / `ipc_core.c`）

### 3.1 消息格式

```text
struct ipc_msg_hdr {        // 固定 16 字节
    u32 type;               // 业务类型（调用方定义）
    u32 len;                // payload 长度，0..4096
    u32 from;               // 发送方端口 ticket（0 = 单向消息）
    u32 flags;              // 保留，必须 0
};
payload[len]                // ≤ 4 KiB
```

`send/recv` 的 `len` 参数都是**含头部的总长度**，`len == 16 + hdr.len`。
校验（`ipc_msg_check`）：

- `len < 16` / `len > 16+4096` → `IPC_EINVAL` / `IPC_EMSGSIZE`；
- `hdr.len > 4096` → `IPC_EMSGSIZE`；
- `16 + hdr.len != len` → `IPC_EINVAL`（长度不自洽）；
- `hdr.flags != 0` → `IPC_EINVAL`；
- `hdr.from != 0` 时必须是本表里存活的 PORT 句柄，否则 `IPC_EBADF`。

### 3.2 端口

```text
struct ipc_port {
    u32 used, closed, depth, head, tail, count;
    u32 slot, gen;             // 创建它的句柄槽/代（ticket 交叉校验）
    u32 owner_uid;
    struct ipc_msg **ring;     // depth 个指针，port_create 时分配
    struct ipc_waiter *recv_waiters, *send_waiters;
    u64 sent, recvd, sent_blocked, recv_blocked, closure_wakes;
};
```

- `port_create(depth)`：`depth=0` → 默认 16；上限 64；端口池 16 个。
- 队列环形，FIFO；`send` 满则阻塞，`recv` 空则阻塞。
- `recv` 的 `max` 小于真实长度时**只拷 max 字节**，返回值仍是真实总长度，
  调用方据此判断截断。
- `close`：先让句柄失效（generation+1），再清空队列（逐条 `free`）、
  唤醒 recv/send 两侧所有等待者（返回 `IPC_ECANCELED`）。有等待者时端口
  对象延迟到“等待者全部退出后的下一次 IPC 操作”回收；没有等待者时立即回收。

### 3.3 ticket 与 reply

**ticket 与端口句柄同构**：`ticket = (port_generation << 16) | slot`，就是
`port_create` 返回的值。理由：ng 目前单进程、只有一张句柄表，客户端不需要
额外系统调用就能把自己的端口 ticket 交给服务端；旧 32 位树同样是
`ticket=(pid<<16)|port_handle`（见 `src/kernel/svc/ipc.h`）。

`reply(ticket, msg, len)` 的流程与 `send` 共用核心，但额外要求：

1. ticket 必须解析为存活 PORT 句柄；
2. **交叉校验** `p->slot == handle_slot_of(ticket) &&
   p->gen == handle_gen_of(ticket)`——即使句柄表槽位复用，也不能把旧 ticket
   指到新端口；
3. 权限检查同 send。

`from` 字段由发送方填写（本进程内的端口 ticket），内核只校验“存活”。这
意味着当前任何能填 from 的线程都能把回复投到表内任意端口；单进程/自检下
可接受，跨进程必须换成 capability（§10）。

### 3.4 权限

端口记录 `owner_uid`；`send/recv/reply/close` 都要求：

```text
caller_uid == IPC_UID_ROOT(0) || caller_uid == port->owner_uid
```

内核路径一律传 root（没有进程/uid）。host 单测用不同 uid 覆盖了
“同 uid 通过 / 不同 uid `IPC_EPERM` / root 绕过 / 被拒绝的 close 不失效
句柄”。

## 4. 阻塞状态机

### 4.1 等待者

```text
struct ipc_waiter { u32 tid, kind, notified, in_use; struct ipc_waiter *next; };
```

静态池 12 条（阻塞路径不分配内存；池满时阻塞调用返回 `IPC_EAGAIN`）。
每个端口两条链：`recv_waiters` / `send_waiters`，FIFO 追加。

### 4.2 等待循环

```text
ipc_wait(c, port, kind, deadline, forever):
    w = waiter_alloc(self_tid, kind);  add_tail(port, kind, w)
    for (;;):
        if port->closed:              leave(w); return ECANCELED
        if 条件满足(recv: count>0 / send: count<depth):
                                      leave(w); return OK
        if !forever and now >= deadline:
                                      leave(w); return ETIMEDOUT
        w.notified = 0
        sleep(forever ? IPC_FOREVER_SLEEP : deadline - now)
```

- 单核协作式：检查条件 → 入队 → 再检查 → sleep 之间不会被其它线程插入，
  所以没有丢唤醒窗口。
- 发送/接收成功后调用 `wake_one(port, 对侧)`：从链头找第一个
  `notified == 0` 的等待者，置位并 `env->wake(tid)`；两条消息会叫醒两个
  不同等待者，避免“多个接收者只醒一个”的饥饿。
- `close` 调 `wake_all`（recv + send 两侧），把所有等待者置 notified 并唤醒。
- 到期判断放在条件判断之后：`send` 恰好在 deadline 时腾出空间仍然成功。

### 4.3 超时语义

`timeout` 单位是 **TSC tick**（与 WP1-5 的 `thread_sleep` 一致，没有频率
校准）：

| 值 | 语义 |
|---|---|
| `IPC_TIMEOUT_NOW`（0） | 非阻塞；条件不满足返回 `IPC_EAGAIN` |
| `IPC_TIMEOUT_FOREVER`（~0） | 永久等待，只能被 send/recv/close 唤醒 |
| 其它正整数 | 阻塞至多 N tick，超时返回 `IPC_ETIMEDOUT` |

`deadline = now + timeout` 做饱和处理；不会因为回绕立刻超时。

### 4.4 端口回收顺序

`close` 后端口 `closed=1` 但 `used=1`，等待者只读 `closed` 标志；等最后
一条等待者从 `ipc_wait` 返回、把自己从链上摘掉后，下一次任意 IPC 操作
（`ipc_reap_ports`）才 `free(ring)` 并清空槽位。这样避免“唤醒别人之后立刻
释放别人还在看的端口对象”。

## 5. syscall 接口

调用号定义在 `src/ng/syscall.h`（ABI 1.0 草案，WP4-1 冻结）：

| rax | 调用 | 参数（rdi/rsi/rdx/r10） | 返回 |
|---|---|---|---|
| 0 | write | — | 0 |
| 1 | exit test | — | 0 |
| 10 | port_create | depth | handle / -err |
| 11 | port_send | handle, msg, len, timeout | 0 / -err |
| 12 | port_recv | handle, buf, max, timeout | 消息总长 / -err |
| 13 | port_reply | ticket, msg, len, timeout | 0 / -err |
| 14 | port_close | handle | 0 / -err |

错误码（负值，两补码）：`-1 EPERM`、`-9 EBADF`、`-11 EAGAIN`、`-12 ENOMEM`、
`-14 EFAULT`、`-22 EINVAL`、`-28 ENOSPC`、`-90 EMSGSIZE`、`-110 ETIMEDOUT`、
`-125 ECANCELED`。

### 5.1 用户指针校验（`ipc.c`）

```text
user_range_ok(va, len, write):
    va != 0, len != 0, 无 64 位溢出, 末地址 <= 0x00007FFF_FFFFFFFF
    对该范围每一页：pg_walk(page) 必须 P=1 且 U/S=1（write 时还要 W=1）
```

再在 `pg_smap_on()` 时用 `stac/clac` 包住逐字节拷贝（SMAP=off 的 CPU 上
跳过，与 isr.S 的 `ng_clac_enabled` 判定一致）。`msg` 先 `kmalloc` 到内核
缓冲，copyin 后调状态机；`recv` 先校验用户可写范围，再用内核缓冲收，
copyout 后返回真实长度。

**测试方式**：自检直接调用 `ipc_sys_*`，用 WP1-3 映射的用户页
（`0x40100000`，U|W|NX）做真实 copyin/copyout；用内核栈地址 / null 验证
`IPC_EFAULT`。真正的 ring3 `syscall` 指令往返留给 WP1-7（ring3 程序目前
只发 0/1）。

### 5.2 阻塞在 syscall 里的限制

syscall 入口仍然用 WP1-3 的**单个静态内核栈**。阻塞式 `recv/send` 会在
syscall 内 `thread_sleep` 并切走；当前没有用户态并发线程，唤醒后仍回到
同一个栈，所以可用。一旦有多个用户线程同时 syscall，必须改成 per-thread
内核栈（WP1-7/进程模型）。

## 6. 内核自检（`ipc_selftest.c`）

真实线程 + 真实 `thread_sleep/thread_wake`，出口行：

```text
ng: ipc ping/pong ok (n=20)
ng: ipc ticket ok
ng: ipc timeout ok
ng: ipc queue full ok
ng: ipc close wakes waiter ok
ng: ipc bad handle rejected
ng: ipc handle table ok (slots=64 ports=16)
ng: ipc user ptr ok
ng: ipc stat sends=... recvs=... replies=20 timeouts=... eagains=... bad=... live=0
ng: WP1-6 selftest PASS
```

| 子项 | 做法 | 断言 |
|---|---|---|
| ping/pong | P/Q 各一个端口；P 发 20 次请求（`from=P`），Q recv 后 `reply(hdr.from)`；P 收 20 次回复 | 20/20、type/len/from/payload 全对、两线程回收 |
| ticket | `port_create` 返回值解码 | gen≥1、slot<64、端口 slot/gen 与句柄一致 |
| 超时 | 空端口 recv 5M tick；满端口 send 5M tick；非阻塞空队列 | `ETIMEDOUT`、`EAGAIN`、耗时 ≥ timeout |
| 队列满 | depth=2 塞满后非阻塞 send；再让一个线程永久阻塞 send | 第三次 `EAGAIN`；recv 腾位后 sender 被唤醒并成功；FIFO 0x22/0x23 |
| close 唤醒 | recv 永久阻塞的线程，主线程 close 端口 | 等待者返回 `ECANCELED`，端口回收 |
| 坏句柄 | close 后 send/recv/reply/close；0/0xDEADBEEF；槽位复用旧 ticket；THREAD 类型 | 全部 `EBADF` |
| 表/池上限 | 端口池 16、句柄表 64 | 再多 `ENOSPC`，关闭后 live=0 |
| 用户指针 | 用户页 0x40100000 走 `ipc_sys_send/recv`；内核地址/null/0 长度 | copyin/out 正确；`EFAULT`/`EINVAL` |

## 7. host 单测（`tests/unit-ng/test_ipc.c`）

fake 环境：`f_now` 假时钟、`f_self` 假 tid、可注册的 `on_sleep` 回调
（模拟“另一个线程”在睡眠期间 send/recv/close）、记录 `wake(tid)`、
malloc/free 计数、可注入 ring 分配失败。覆盖：

- 句柄表：编码/gen 递增/槽位复用/类型校验/重复 close/表满/generation
  16 位回绕跳过 0；
- 端口：默认与显式 depth、非法 depth、池满、ring 分配失败回滚、owner uid；
- 消息：长度/头部自洽/flags/from（坏句柄、THREAD 类型、合法 ticket）、
  FIFO、截断（返回值与写入边界）、NULL buffer；
- 阻塞状态机：recv 被 send 唤醒（wake tid=1）、send 被 recv 唤醒、
  recv/send 超时、close 唤醒 recv/send、deadline 回绕保护、等待者池复用；
- close：队列清空与内存释放、句柄失效、端口延迟回收；
- 权限：同 uid/root/EPERM、被拒 close 不失效句柄；
- reply/ticket：from 往返、reply 计数、坏 ticket、已关闭 ticket；
- 上限：64 句柄、16 端口、句柄满时 port_create 回滚。

从 WP1-5 的 390 checks 增至 **810 checks, 0 failures**。

## 8. 为什么这样设计

1. **ticket = 句柄**：单进程下这是最少系统调用、最少状态的做法；generation
   已经把“close 后旧 ticket”挡死。跨进程时 ticket 需要独立命名空间或 cap
   grant 表，届时 `reply` 的交叉校验已经是现成的挂点。
2. **`from` 由发送方填**：消息头只有 16 字节，ABI 没有独立 ticket 字段；
   让内核自动填需要“每线程默认回复端口”的新概念，超出 WP1-6。先按旧树
   的做法（请求方把自己的 ticket 放进消息），内核校验存活，host 单测覆盖
   伪造路径。
3. **静态等待者池**：阻塞路径不能分配内存（失败要能回退），12 条对当前
   端口数/自检足够；池满返回 `EAGAIN` 而不是死等。
4. **延迟回收**：等待者醒来时还要读 `port->closed`，所以 close 不能立即
   free 端口对象；用 `closed + 下一次操作 reap` 是最小且可证明的顺序。
5. **TSC tick 超时**：与 WP1-5 的 `thread_sleep` 同一时间源，不引入频率
   校准（校准属于时钟 WP）。
6. **无锁**：单核、IF=0、只有显式调度点，和调度器/堆的现状一致；SMP 锁
   留给多核 WP。

## 9. 文件与工具链变更

| 路径 | 内容 |
|---|---|
| `src/ng/handle.h` / `.c` | 句柄表（编码/generation/类型/统计）**新** |
| `src/ng/ipc.h` | 消息头、错误码、port/waiter/core、env、syscall 声明 **新** |
| `src/ng/ipc_core.c` | 端口队列 + 等待队列 + 阻塞/超时/close/reap **新** |
| `src/ng/ipc.c` | 内核 env、内核 API、用户指针 + SMAP copyin/out、syscall 入口 **新** |
| `src/ng/ipc_selftest.h` / `.c` | 内核自检与出口行 **新** |
| `src/ng/syscall.h` / `.c` | 调用号 10–14 + dispatch **改** |
| `src/ng/main.c` | `ipc_init()` + WP1-6 自检/PASS 行 **改** |
| `tests/unit-ng/test_ipc.c` | 句柄/端口/阻塞状态机 host 单测 **新** |
| `tests/unit-ng/unit_main.c` | 加 `test_ipc()` **改** |
| `tools/unit-ng.ps1` | 加 `handle.c`/`ipc_core.c`/`test_ipc.c` **改** |
| `tools/build-ng.ps1` | 加 4 个 C 源（17 → 21 步）**改** |
| `tools/run-ng.ps1` | 9 条固定行 + ipc 统计正则 + 失败模式 **改** |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`。

## 10. 未做 / 遗留（按 WP 边界）

- **uid/权限**：没有进程，内核路径一律 root；`owner_uid` 字段与判断逻辑已
  就位并由 host 单测覆盖，真实 uid 在进程模型 WP 接。
- **capability / 跨进程 ticket**：ticket 只在单张句柄表里有效；没有
  `cap_grant`，服务端拿不到其它进程的端口。`reply` 的 slot/gen 交叉校验
  是未来接 cap 表的挂点。
- **`from` 防伪**：发送方可以填表内任意存活 PORT 作为 from；跨进程必须
  由内核填或由 cap 校验，属安全遗留。
- **用户态多线程/阻塞 syscall**：syscall 仍用单个静态内核栈；per-thread
  内核栈与用户线程是 WP1-7 之后。
- **ring3 IPC 程序**：ELF64/ring3 应用是 WP1-7；本 WP 只在 handler 边界
  用真实用户 VA 验证 copyin/copyout。
- **数据面/共享内存**：消息 ≤4 KiB 控制面；大块传输（共享内存、零拷贝）
  后续 WP。
- **超时单位**：TSC tick，未校准；将来 ABI 冻结时改毫秒（需要时钟 WP）。
- **抢占**：仍是协作式调度；`sched_tick()`/timed wake 已就绪，tick IRQ
  未接（与 WP1-5 遗留相同）。
- **端口 flags/私有端口**：`port.flags` 预留，`IPC_FLAG_*`、private 语义
  未实现。
- **`recv` 批量/`wait_any`**：只做单端口阻塞 recv；`wait_any`/select 后续。
