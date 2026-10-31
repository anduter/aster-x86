# ng 进程与跨进程 IPC 设计（WP3-0，E3 前置）

> 状态：已实现（2026-10-31），出口见 `docs/claims/ng-WP3-0.md`。
> 范围：只做"多进程用户态 + 跨进程 IPC"这一层前置；display server（WP3-1）、
> shell/WM（WP3-2）、session manager（WP3-3）都不在这里。
> 上位约束：`docs/design/architecture.md` 第 5 节不变量（L0/L1 不共享 AS、
> 跨域只经 IPC + cap，cap 本 WP 可后置）。

## 1. 目标与边界

| 项 | 本 WP 做到 | 明确不做 |
|---|---|---|
| 进程 | `proc_t` + 32 槽进程表、pid 单调、父子、退出码/僵尸 | 多线程进程、fork、uid 策略 |
| 隔离 | 每进程独立 PML4（`user_as`）、独立内核线程、独立 64 槽句柄表 | 每进程 fd 表（VFS 仍是全局一份） |
| 生命周期 | spawn / wait / exit / kill / 僵尸回收 / 孤儿重挂 | 作业控制、信号语义（sig 只记状态码） |
| IPC | ticket（端口全局身份）跨进程 send/recv/reply | capability 授权、共享内存数据面 |
| 用户态 | 一个 ELF 多角色（argv 驱动）+ shell 子进程 | libc/POSIX、动态链接 |

## 2. 进程模型

```c
struct proc {                     /* src/ng/proc.h；纯状态机部分在 proc_table.c */
    u32 pid, ppid, state, tid, uid;
    u32 exit_kind;                /* NONE / NORMAL / KILLED */
    int exit_status;              /* 正常 = 0..255；kill = 0x100|sig */
    u32 killed_sig, wait_tid, wait_count, flags;
    u64 entry, user_rsp, argv_va; /* 用户入口与初始栈/argv（WP3-0 ABI） */
    u32 argc;
    u64 stack_lo, stack_hi, load_base, load_end, load_pages;
    u32 load_segs, marker_phys, as_pml4;
    char path[64], cwd[128];      /* cwd 是占位（VFS ctx 仍是全局一份） */
    void *as;                     /* struct user_as *（独立 PML4） */
    struct handle_table handles;  /* 64 槽，per-process */
    /* 退出时保存的 ring3 统计（证据行用） */
    u64 syscalls, writes, fs_calls, exit_code;
    u32 user_rsp_ok, bad_writes, exited;
};
```

规则：

1. 进程表静态 **32 槽**；`pid` 从 1 单调递增，回绕跳过 0；**槽位可以复用，
   pid 不可以**（避免 stale pid 指向新进程）。
2. **pid 1 = 内核 init**：`kmain` 所在的 boot 线程，没有用户 AS；父进程先退出
   的孩子重挂到它。init 的僵尸只能由 init 自己 `wait` 回收——本 WP 没有后台
   收尸线程，所有测试路径都显式 `wait`（长跑场景的后台回收留给 session
   manager / WP3-3）。
3. 每个进程 = **1 个用户地址空间 + 1 个主线程 + 1 张句柄表**。主线程是真正的
   内核线程（TCB 在调度器里），用 `syscall_run_user_args()` 进 ring3。
4. 状态机（`proc_table.c`，host 单测直接驱动）：

```
FREE --proc_alloc--> READY --调度--> RUNNING --block--> BLOCKED
                       ^                  |               |
                       +----- 唤醒 --------+               |
                                          \--exit/kill--> ZOMBIE --proc_reap--> FREE
```

## 3. 地址空间、线程与调度

三件事必须一起改，否则多进程会互相踩：

1. **CR3 跟着线程走**。`thread_t.pml4` 记录该线程运行时的用户 PML4
   （内核线程为 NULL）；`sched_arch_switch()` 在 `switch_context` **之前**
   写 CR3。所有用户 AS 都映射内核 0..1 GiB 恒等区，所以先换 CR3 再换栈是
   安全的，反过来会让新线程在自己的栈上短暂跑在旧 AS 里。
2. **SYSCALL 入口要有独立栈**。`ring3_enter` 把"回到 C 的恢复点"压在当前
   （C）栈上；如果 SYSCALL 入口也从同一个栈顶开始压 15 个寄存器，就会覆盖
   恢复点。因此用户进程线程额外分配 16 KiB 的 `syscall_stack`（`thread_t.
   syscall_top`），内核线程仍用原来的 C 栈/静态栈。
3. **ring3/syscall 状态进 TCB**。WP1-7 把 `user_rsp / want_exit / 统计` 放在
   `syscall.c` 的全局里，单进程时成立；两个进程交替（A 在 syscall 里阻塞、
   B 在 ring3 跑）时会被覆盖 —— 最典型的是 B 的 exit 把 `want_exit` 置 1，
   A 的下一次 syscall 返回就错误地跳回内核。现在这些状态在
   `thread_t.uctx`（`struct thread_uctx`）：

```text
user_rsp / saved_rip / saved_flags / resume_rsp / resume_rip
stack_lo / stack_hi / syscalls / writes / bytes / fs_calls / exit_code
from_ring3 / run_user / user_rsp_ok / bad_writes / return_mode / want_exit
```

汇编仍通过少量全局与 C 交互，但只在两段**没有调度点**的窗口里有效：
`SYSCALL 入口 -> syscall_dispatch 入口`（`syscall_enter_ctx` 收进 TCB）与
`syscall_prepare_return -> SYSRET/IRETQ`（把本线程状态写回全局）。
`ring3_enter` 调 `ring3_save_resume()` 把恢复点存进 TCB，退出时按本线程的
恢复点返回。

## 4. 进程入口 ABI 与 spawn

用户进程的初始寄存器（`ring3_enter_args`）：

```text
rdi = argc        rsi = argv（用户栈上的 char*[]，NULL 结尾）
```

`spawn(path, argv, &pid)` 的流程（`proc_spawn`）：

```text
1. fs_sys_read_file(path)            VFS 读 ELF（ramfs/FAT16 同一条路径；≤256 KiB）
2. elf64_check()                     ELF64/ET_EXEC/EM_X86_64/W^X/窗口校验
3. proc_alloc(&g_procs, parent)      pid 单调分配（表满 ENOSPC）
4. as_create() + elf64_load()        新 PML4；段按 W^X 映射
5. 用户栈 4 页（RW+NX）+ 标记页      栈下方一页不映射（守护页天然 EFAULT）
6. proc_build_argv()                 字符串从栈顶往下放，再放 char*[] + NULL
7. thread_create(proc_thread_main)   新内核线程；t->pml4 = 新 AS
8. 16 KiB SYSCALL 栈                 t->syscall_top
9. handle_alloc(THREAD)              进程自己的句柄表记一条
```

失败路径（任一步失败）按相反顺序回收：杀线程 → 还句柄/端口 → `as_destroy`
→ 标记退出并回收槽位 → 释放 ELF 缓冲。**不会留下野线程或半张 AS**。

用户栈布局（示例 `spawn /bin/proc hello`）：

```text
NG_USER_STACK_TOP 0x40104000
   ...            "hello\0"
   argv[0] -> 0x/...（字符串）
   argv[1] -> ...
   0
   argv_va == 初始 RSP（16 对齐）
```

### 标记页（AS 隔离证据）

每个进程额外映射一页 `PROC_MARKER_VA = 0x40010000`（RW+NX，U=1）。同一个
ELF 的两个进程在这**同一个 VA** 上是**不同的物理页**；用户程序把一个 tag
写进去，内核用 `as_walk(p->as, PROC_MARKER_VA)` 读回物理页内容对比 —— 这是
"同 VA 不同 AS"的直接证据，不依赖用户态打印。

## 5. syscall 40..44

| 号 | 签名 | 语义 |
|---|---|---|
| 40 | `spawn(path, argv, &pid)` | 从 VFS 读 ELF、建进程；用户指针走 uaccess（path ≤64，argv ≤8×128，总 ≤1 KiB） |
| 41 | `wait(pid, &status)` | 只等**直接子进程**；阻塞（`thread_sleep`），子进程退出时 `thread_wake`；成功后回收僵尸并写 status |
| 42 | `getpid()` | 返回本进程 pid |
| 43 | `kill(pid, sig)` | 终止目标（含阻塞在 recv 里的进程）；状态 = `0x100|sig`；自杀走正常退出路径 |
| 44 | `getppid()` | 返回父 pid |

`status` 编码：正常退出 = exit code（0..255）；被 kill = `0x100 | sig`
（例：`kill(pid, 9)` -> `wait` 得到 265）。这样"正常退出码"与"被终止"不会
混淆，又不需要引入 `WIFEXITED` 那套宏。

## 6. 每进程句柄表与跨进程 ticket

```text
进程 A 的句柄表（64 槽）            全局端口池（16 槽）
 slot 0: PORT -> ipc_port #3   <── 同一个对象 ──>  ticket = (gen<<16)|slot
 slot 1: THREAD -> thread_t
```

- **句柄**是进程私有的引用（`(generation<<16)|slot`，类型 PORT/THREAD/FILE）；
  fork/spawn 不继承，子进程从空表开始；
- **ticket** 是端口的**全局身份**（`(port_generation<<16)|port_slot`），是唯一
  可以跨进程传递的名字。`port_create` 返回 ticket，属主进程同时在自己的句柄表
  里记一条 PORT（退出/kill 时按这张表回收，不会误关别人的端口）；
- 权限：`send`/`reply` 凭 ticket（等于"知道就能发"，cap grant 后置）；
  `recv`/`close` 要求**调用进程 == 端口 owner pid**，否则 `EPERM`。这样
  "ticket 泄露"最多让人往你的端口发消息，取不走消息、也关不掉端口；
- 进程退出/被 kill：遍历自己的句柄表 —— PORT 用 `ipc_core_port_ticket()` 反推
  ticket 调 `ipc_core_close()`（generation 递增，旧 ticket 立即失效），THREAD
  直接 close。

### 3 进程链式 ping（selftest 实测）

```text
client(hello)              relay                     server
  port C                     port R                    port S
  spawn relay(argv C)  -->   spawn server(argv R) -->  send(R, RDY, from=S)
  recv(C) = RDY       <--    send(C, RDY, from=R)
  send(R, PING, from=C) -->  recv(R)=PING
                               reply(C, ACK)
                               send(S, PING, from=R) --> recv(S)=PING
                                                         reply(R, PONG, from=S)
  recv(C) = PONG      <--    send(C, PONG, from=R) <---- recv(R)=PONG
  wait(relay) -> 0           wait(server) -> 0         exit(0)
```

ticket 两跳传递：argv 传一跳（父子）、消息头 `from` 传一跳（内核填不了 from，
由发送方填自己的 ticket，接收方据此 `reply`）。整条链 3 个进程、4 条消息、
2 次 reply，客户端退出码 0 = 整条链成功。

## 7. wait / exit / kill 的状态机

```text
wait(pid):  父进程记录 child->wait_tid，cur->state=BLOCKED，
            while (child->state != ZOMBIE) thread_sleep(big);
            醒来后 status = child->exit_status，proc_reap 释放槽位
exit:       proc_exit_self：保存统计 -> 关句柄/端口 -> as_destroy ->
            reparent children to 1 -> proc_mark_exit -> thread_wake(parent) -> thread_exit
kill:       ipc_core_cancel_thread(tid)  先摘 IPC 等待链
            sched_kill_thread(tid)       从 ready/blocked 链摘下、置 DEAD
            关句柄/端口 + as_destroy
            proc_kill_mark              状态 = 0x100|sig，唤醒父进程
```

三个容易踩的坑（都在实现里处理了）：

1. **kill 阻塞在 recv 里的进程**：必须先把它的 waiter 从端口等待链上摘掉，
   否则它留下的"已通知"条目会吞掉后续唤醒（`ipc_core_cancel_thread`）。
2. **kill 一个暂停的线程**：不能让它再被调度。`sched_kill_thread()` 直接把
   它从 ready/blocked 链摘下并挂 zombie，栈由别的线程下一次调度回收
   （`sched_reap_zombies`，不回收"当前线程"自己的栈）。
3. **自杀**：在 syscall 里 `kill(getpid(), sig)` 走 `proc_exit_self`，不能
   `thread_kill` 自己（那是当前线程）；AS 销毁后继续在自己的内核栈上跑到
   `thread_exit()`。

僵尸回收：`wait` 成功后 `proc_reap` 释放槽位（pid 不复用）；父进程先退出的
子进程重挂到 init，避免永久僵尸。

## 8. 与 VFS / shell 的关系

- **VFS ctx 仍是全局一份**（`fs_syscall.c` 的 `g_ctx`）：`cwd`/`uid`/fd 表没有
  按进程拆；`proc->cwd` 只是占位。spawn 读 ELF 用的就是这份 ctx（绝对路径），
  因此单核协作式下没有并发问题。每进程 fd 表属于 fsd / WP3-x 的活。
- **shell 是进程**（`/bin/shell`，由 `wp26_selftest` spawn + wait），新增
  `spawn <path> [args]`、`wait [pid]`、`pid` 三个命令；run-ng 注入
  `spawn /bin/proc hello` + `wait`，验证 shell 作为父进程（子进程 ppid =
  shell pid，wait 得到 status=0）。
- **内嵌用户 ELF 种子到 ramfs**：`proc_init()` 把 `ng_shell_elf_*`、
  `ng_proc_elf_*`、`ng_user_elf_*` 写到 `/bin/shell`、`/bin/proc`、
  `/bin/hello`（先建 `/bin`）。spawn 从 VFS 读回来，走的就是真实文件路径。

## 9. 证据行（run-ng 全要求命中）

```text
ng: proc bin ok (shell=/bin/shell proc=/bin/proc hello=/bin/hello)
ng: proc table ok (max=32 init=pid 1 tid=1)
ng: proc spawn ok (pid=2 path=/bin/proc)
ng: proc wait ok (status=0 pid=2 tid=15)
ng: proc ipc cross ok (pid=3 procs=3 msgs=4 exits=3)
ng: proc as isolated ok (va=0x40010000 p1=0x5c8000 p2=0x5d4000 tags=111/222)
ng: proc kill ok (pids=6,7 status=265 waiters=0)
ng: WP3-0 selftest PASS
```

用户程序的辅证：`ng: proc hello ok (pid=.. ppid=..)`、
`ng: proc ipc hello/relay/server ok`、`ng: proc hold ok (...)`；
shell 侧：`ng: shell spawn ok (pid=9 path=/bin/proc)`、
`ng: shell wait ok (pid=9 status=0)`。

## 10. 限制 / 遗留（按 WP 边界）

- **用户态缺页/异常仍会 panic**：进程隔离的下一步是"用户态 #PF -> 杀进程"，
  本 WP 没有改 trap 的故障策略（E3 故障注入一起做）。
- **cap 未实现**：ticket 就是能力（知道即可 send/reply）；`cap_grant/revoke`
  与 ticket 防伪（`from` 现在由发送方填）留给 ABI 1.0 / E3。
- **无线程/进程组**：一个进程一个主线程；`kill` 的信号只记状态，不做异步投递。
- **每进程 fd 表 / cwd**：VFS ctx 全局一份（见第 8 节）。
- **抢占**：仍是协作式；`kill` 一个在 ring3 死循环的进程不会生效（没有 tick
  IRQ）。E3 的故障注入需要先接 tick。
- **wait 只支持直接子进程**，没有 `wait -1`/`waitpid` 选项；阻塞用
  `thread_sleep(big)` + 显式唤醒，`wait` 无超时。
- **没有后台收尸**：父进程先退出的孩子重挂到 init，init 只在显式 `wait` 时
  回收；长跑会话的孤儿僵尸回收留给 WP3-3 session manager。
- **spawn 的 ELF 上限 256 KiB**、argv ≤ 8 项 / 单项 <128 字节、路径 ≤ 63 字节。
- **数据面**：跨进程大数据仍没有共享内存，消息上限 4 KiB（WP1-6 的约束）。
- **ASID/PCID**：每次切进程全刷 TLB（正确但不快），E4 再谈。
