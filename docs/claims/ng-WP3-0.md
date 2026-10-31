# ng-WP3-0：多进程用户态 + 跨进程 IPC（PASS）

## 结论

**PASS** —— E3 的前置工作包落地：ng 从"单 AS/单进程 ring3"变成**真进程模型**。

- **进程抽象**：`struct proc`（pid/ppid/AS/tid/入口/栈/退出码/状态/句柄表/
  cwd+uid 占位）；进程表 32 槽、**pid 从 1 单调递增**（槽位可复用、pid 不复用）；
  pid 1 = 内核 init，孤儿重挂到它。
- **syscall（追加 40..44）**：`spawn(path, argv, &pid)`、`wait(pid, &status)`、
  `getpid()`、`kill(pid, sig)`、`getppid()`。`spawn` 从 **VFS**（ramfs `/bin/*`）
  读 ELF64 → `elf64_check/elf64_load` 进**新 AS** → 新内核线程 → ring3；
  失败按相反顺序回收 AS/句柄/栈/槽位。
- **per-process 句柄表**：WP1-6 的全局 64 槽改成 `proc->handles`（每进程一张，
  PORT/THREAD/FILE 命名空间）；spawn 不继承；进程退出/被 kill 只按**自己的表**
  回收自己的端口。
- **跨进程 IPC**：端口有全局身份 **ticket** = `(port_gen<<16)|port_slot`，是唯一
  能跨进程传递的名字；`send/reply` 凭 ticket，`recv/close` 要求 owner pid。
  实测 **3 进程链式 ping**：hello → relay → server（ticket 经 argv 与消息 `from`
  两跳传递、两次 `reply`、4 条消息）。
- **调度/退出**：`wait` 用 `thread_sleep` 阻塞、子进程退出 `thread_wake` 唤醒；
  `kill` 摘 IPC 等待者 + 摘线程 + 关端口 + 销毁 AS；僵尸由 `wait` 回收；
  `exit` 状态 = 正常退出码，被 kill = `0x100|sig`。
- **自检**（run-ng 全命中）：`ng: proc spawn ok (pid=..)`、
  `ng: proc wait ok (status=0 ...)`、`ng: proc ipc cross ok (...)`、
  `ng: proc as isolated ok (...)`、`ng: proc kill ok (...)`、
  `ng: WP3-0 selftest PASS`。
- **host 单测**：**4043 checks, 0 failures**（3203 → +840：进程表/句柄命名空间/
  wait-kill 状态机 + shell spawn/wait + syscall 40..44 分发）。
- **无回归**：parity **100/100**（host 95/95）、`ci-ng` **7/7 PASS**、
  旧 32 位 **305/305**；run-ng 默认 **118/118 行 + 42 正则**
  （Smoke 103+38、NoQmp 102+38、Fallback 117+36、qemu64 118+42）。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/proc.h` | 进程模型/上限/状态/状态码 + 纯状态机 API + 内核 API + syscall 入口（**新**） |
| `src/ng/proc_table.c` | 纯进程表：pid 单调、父子、退出/僵尸/回收、kill 迁移、孤儿重挂（**新**） |
| `src/ng/proc.c` | 内核胶水：spawn（VFS 读 ELF/AS/栈/标记页/argv/线程）、wait、kill、exit、40..44（**新**） |
| `src/ng/proc_selftest.h` / `.c` | WP3-0 内核自检与 8 条证据行（**新**） |
| `src/apps/proc/proc.c` | 用户态多角色程序 `/bin/proc`：hello / hello chain / relay / server / hold（**新**） |
| `src/ng/thread.h` | `struct thread_uctx`（per-thread ring3 状态）、`pml4`/`proc`/syscall 栈、`sched_kill_thread()`（**改**） |
| `src/ng/sched.c` | `sched_core_remove` + `sched_kill_thread`；回收 zombie 时一并归还 syscall 栈（**改**） |
| `src/ng/sched_arch.c` | 切换线程前按 `thread->pml4` 写 CR3（**改**） |
| `src/ng/syscall.c` / `.h` | per-thread uctx、`syscall_enter_ctx`/`prepare_return`、`ring3_enter_args`、40..44 分发（**改**） |
| `src/ng/isr.S` | SYSCALL 入口 CS、恢复点进 TCB（`ring3_save_resume`）、`ring3_enter_args`（**改**） |
| `src/ng/handle.c` / `.h` | `handle_find_obj()`（按对象找本进程句柄）（**改**） |
| `src/ng/ipc.h` / `ipc_core.c` / `ipc.c` | 端口 `owner_pid`、ticket 解析/回收、`ipc_core_cancel_thread`、waiter 统计、per-proc 端口安装/释放（**改**） |
| `src/ng/fs_syscall.c` / `.h` | 内核侧文件接口 `fs_sys_seed_file` / `fs_sys_read_file`（**改**） |
| `src/ng/wp26_selftest.c` | shell 改为**进程**：spawn `/bin/shell` + wait，证据行不变（**改**） |
| `src/ng/main.c` | `proc_init()` + WP3-0 自检块 + PASS/log（**改**） |
| `src/apps/shell/shell.c` | 新命令 `spawn <path> [args]` / `wait [pid]` / `pid`；help 更新（**改**） |
| `tools/build-ng.ps1` | 编/校验/嵌入 `proc.elf`（`ng_proc_elf_start..end`）；新增 3 个内核源；55 步（**改**） |
| `tools/run-ng.ps1` | WP3-0 8 条期望行 + 失败模式；shell 注入 `spawn /bin/proc hello` / `wait`（**改**） |
| `tools/unit-ng.ps1` | 加 `proc_table.c` + `test_proc.c`（**改**） |
| `tests/unit-ng/test_proc.c` | 进程表/句柄命名空间/wait-kill 状态机 host 单测（**新**） |
| `tests/unit-ng/fake_ng_env.c` / `.h` | fake `proc_sys_*`（40..44 分发）+ fake `ring3_enter_args`（**改**） |
| `tests/unit-ng/test_syscall.c` | 40..44 参数搬运用例（**改**） |
| `tests/unit-ng/test_shell.c` | fake spawn/wait + `spawn`/`wait`/`pid` 命令用例（**改**） |
| `docs/design/proc-ng.md` | 设计文档：进程/AS/线程、spawn、syscall、句柄与 ticket、wait/kill、遗留（**新**） |
| `docs/design/evolution.md`、`docs/design/README.md` | E3 进度与索引（**改**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`src/ng` 新增文件
不进旧对象列表。

## 环境

- 分支 `ng`，基线 HEAD `d3a5671`（tag `ng-e2`）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）。
  内核：`clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
  -mno-red-zone -fno-stack-protector -mgeneral-regs-only`；
  用户程序（shell / proc）：同一 target + `-mno-sse -fno-builtin -fno-pic
  -std=c11 -O2`，`src/ng/user/user.ld` 链到 0x40000000。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`，QMP TCP
  `input-send-event` 注入按键。

## 关键参数

| 项 | 值 |
|---|---|
| 进程表 | 32 槽；pid 1 = init；槽位复用、pid 单调（回绕跳过 0） |
| 每进程句柄表 | 64 槽（PORT/THREAD/FILE），spawn 不继承 |
| 用户 AS | 独立 PML4；窗口 `[0x40000000, 0x7FFFFFFFFF]`；每进程栈 16 KiB + 标记页 |
| 标记页 | `0x40010000`（RW+NX，U=1）：同 VA、不同 AS -> 不同物理页 |
| 用户栈 | 16 KiB，`NG_USER_STACK_BASE=0x40100000`，下方守护页不映射 |
| 进程入口 ABI | `rdi=argc`、`rsi=argv`（argv 在用户栈上，NULL 结尾） |
| SYSCALL 栈 | 每进程线程 16 KiB 专用（`thread->syscall_top`） |
| spawn 限制 | ELF ≤ 256 KiB；argv ≤ 8 项、单项 < 128 字节；path ≤ 63 字节 |
| 退出状态 | 正常 0..255；kill = `0x100|sig`（`kill(pid,9)` -> 265） |
| 端口 | 全局池 16；ticket = `(gen<<16)|slot`；depth 默认 16、上限 64 |
| 证据行 | 见第 4 节 |

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
=== build-ng (WP3-0) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe

=== user-elf (WP1-7) ===
user-elf: PASS (15872 bytes, 3 PT_LOAD, entry 0x40000000, end 0x400023b0)

=== user-shell (WP2-6) ===
user-shell: PASS (27000 bytes, 3 PT_LOAD, entry 0x400007c0, end 0x40005dd4)

=== user-proc (WP3-0) ===
  proc.c  -> obj-ng/user_proc.o
  link    -> obj-ng/proc.elf (base 0x40000000)
user-proc: PASS (13288 bytes, 2 PT_LOAD, entry 0x40000000, end 0x4000216c)
...
  [1/55] entry.S
  ...
  [55/55] link -> dist-ng/aster-ng.elf

ELF64: PASS (1977448 bytes)
  entry    0x0000000000101000
  phnum    7
segments: PASS
  5 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
pvh-note: PASS
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

`-Smoke`：`build-ng: PASS`（`-DNG_SMOKE_MODE=1` 跳过 shell，WP3-0 照跑）。

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

```text
=== run-ng (WP3-0) ===
...
ng: parity 100/100 PASS
ng: WP2-7 selftest PASS
ng: proc bin ok (shell=/bin/shell proc=/bin/proc hello=/bin/hello)
ng: proc table ok (max=32 init=pid 1 tid=1)
ng: proc selftest: spawn/wait/ipc/kill/as
ng: proc spawn ok (pid=2 path=/bin/proc)
ng: proc hello ok (pid=2 ppid=1)
ng: proc wait ok (status=0 pid=2 tid=15)
ng: proc ipc server ok (pid=5)
ng: proc ipc relay ok (pid=4 hops=2)
ng: proc ipc hello ok (pid=3 ppid=1)
ng: proc ipc cross ok (pid=3 procs=3 msgs=4 exits=3)
ng: proc hold ok (pid=6 ppid=1 tag=111)
ng: proc hold ok (pid=7 ppid=1 tag=222)
ng: proc as isolated ok (va=0x40010000 p1=0x5c8000 p2=0x5d4000 tags=111/222)
ng: proc kill ok (pids=6,7 status=265 waiters=0)
ng: proc stat spawned=7 live=1 switches=33401
ng: WP3-0 selftest PASS
...
ng: shell ready
ng> spawn /bin/proc hello
ng: shell spawn ok (pid=9 path=/bin/proc)
ng: proc hello ok (pid=9 ppid=8)
ng> wait
ng: shell wait ok (pid=9 status=0)
ng> exit
ng: shell exit ok
ng: syscall exit test
ng: shell exit (code=0 syscalls=231773 writes=200 fs=12)
ng: shell session ok (exit=0 rsp=ok)
ng: shell session file ok (s.txt=3)
ng: shell session pkg ok (lists=+1)
ng: WP2-6 selftest PASS
------------------------------------------------
run-ng: matched 118/118 expected lines + 42 patterns
run-ng: PASS (all expected lines in 4.9s, cpu max)
```

`run-ng.ps1` 新增期望（8 条固定行 + 1 条失败模式）：

```text
ng: proc bin ok (shell=/bin/shell proc=/bin/proc hello=/bin/hello)
ng: proc table ok (max=32 init=pid 1
ng: proc spawn ok (pid=
ng: proc wait ok (status=0 pid=
ng: proc ipc cross ok (pid=
ng: proc as isolated ok (va=0x40010000
ng: proc kill ok (pids=
ng: WP3-0 selftest PASS
（QMP 分支另加）
ng: shell spawn ok / ng: shell wait ok (pid=
失败早停模式新增：
ng: proc .* FAIL / ng: WP3-0 selftest FAIL
```

证据解读：

- **spawn/wait**：`/bin/proc hello` 从 ramfs 读 ELF 建进程（pid 2、新 AS、
  tid 15），打印自己的 `pid=2 ppid=1` 后 exit(0)；内核 `wait` 得到 status=0，
  并核对该子进程 ppid=1、`as_pml4 != 0`、tid != 0。
- **3 进程链式 ping**：客户端 pid 3 spawn relay(pid 4)（argv 传自己的 ticket），
  relay spawn server(pid 5)（argv 传 relay 的 ticket）；server 用 `from=S` 发
  RDY、relay 用 `from=R` 把 RDY 转给客户端；客户端发 PING 到 R，relay
  `reply(C, ACK)` 后转发 PING 到 S；server `reply(R, PONG)`，relay 把 PONG
  转回 C；客户端 `wait(relay)`=0、relay `wait(server)`=0。内核只在客户端退出时
  看到 status=0，即整条链成功。
- **AS 隔离**：两个 hold 进程写同一个 VA `0x40010000`，PTE 物理页不同
  （0x5c8000 / 0x5d4000），读回内容 111/222；ELF base 的 PTE 也不同。
- **kill**：两个 hold 都**阻塞在 recv** 里；`kill(pid,9)` 之后 `wait` 得到
  `0x109`=265，`ipc_core_waiter_live()==0`（等待链没有泄漏），进程表回到
  live=1（只剩 init）。
- **shell 作为父进程**：shell 自己 pid 8，spawn 出 pid 9（ppid=8），wait 得到
  status=0 —— 证明 shell 不是"内核里的 ring3 程序"，而是一个真进程。
- **非子进程**：`wait(9999)` -> ECHILD、`kill(9999)` -> ESRCH、
  重复 kill 已回收的 pid -> ESRCH。

### 负向 / 降级 / 其他 CPU

```text
run-ng -NoQmp        -> 102/102 lines + 38 patterns PASS
run-ng -Fallback     -> 117/117 lines + 36 patterns PASS（-vga none）
run-ng -Cpu qemu64   -> 118/118 lines + 42 patterns PASS（SMEP/SMAP off）
build-ng -Smoke + run-ng -Smoke -> 103/103 lines + 38 patterns PASS
```

`-Smoke` 只跳过 shell；WP3-0（spawn/wait/ipc/kill/as）照常跑并 PASS。

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
...
parity(host): 95/95 PASS (5 display/input cases are kernel-only)
4043 checks, 0 failures
unit-ng: PASS (4043 checks, 0 failures)
```

WP2-7 为 3203，本 WP **+840**：

- `tests/unit-ng/test_proc.c`（新）：进程表 32 槽/pid 单调与回绕/满拒绝、
  find/child_count/child_at/is_child、退出→僵尸→回收、重复退出 EBUSY、
  回收后 ESRCH、wait 的唤醒目标与幂等、kill 状态编码与 EBUSY/ESRCH、
  孤儿重挂、每进程句柄表互不影响（同句柄值不同对象、generation 独立、
  一张表满另一张可用）、`handle_find_obj` 类型/通配/找不到、统计计数；
- `tests/unit-ng/test_shell.c`（+）：fake spawn/wait，`spawn` 路径解析与
  argv[0]=路径、`wait` 默认子进程/指定 pid/非零状态置 `$?`、用法错误分支、
  `pid` 输出；
- `tests/unit-ng/test_syscall.c`（+）：40..44 的寄存器→参数搬运与返回值。

### 门禁

```text
tools/check-domains.ps1
checked: 236 files, 763 quoted includes
domain include violations: 0

tools/check-deps.ps1
... module include violations: 0
OK: no cross-module include violations.

tools/parity-ng.ps1 -LogFile obj-ng/wp3-run.log
parity-ng: PASS (100/100 items, 0 FAIL, 10 domains)

tools/ci-ng.ps1
  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          4043 checks; exit=0, 7.4s
  PASS build-ng         1977448 bytes, user-elf 15872 bytes; exit=0, 8.0s
  PASS run-ng           118/118 lines + 42 patterns; exit=0, 5.5s
  PASS parity-ng        100/100 items, 10 domains; exit=0, 0.2s
  PASS legacy-32bit     305/305 parity checks; exit=0, 39.5s
  7 passed, 0 failed, 0 skipped, 61.7s
=== NG CI PASS ===
```

旧 32 位独立/门禁内均 **305/305**。

## 设计要点（详见 `docs/design/proc-ng.md`）

### 进程 = 1 AS + 1 主线程 + 1 张句柄表

```text
proc_t: pid/ppid/state/tid/uid/exit_status/entry/user_rsp/argc/argv_va
        as(PML4)/load_base/end/pages/segs/marker_phys
        handles[64]/cwd[128]/ring3 统计
进程表: 32 槽，pid 单调；pid 1 = init（boot 线程，无用户 AS）
```

多进程要同时改三处，否则会互相踩：

```text
1) thread_t.pml4      + sched_arch_switch() 先写 CR3 再切栈
2) thread_t.syscall_top  每进程线程 16 KiB SYSCALL 栈
3) thread_t.uctx     ring3/syscall 状态从全局搬进 TCB（from_ring3/want_exit/…）
```

### 句柄 vs ticket

```text
handle = (generation<<16)|slot   进程私有：PORT/THREAD/FILE
ticket = (port_gen<<16)|port_slot 端口全局身份，唯一可跨进程传递
send/reply: 凭 ticket（知道即可发，cap 后置）
recv/close: 要求 caller pid == port owner pid
退出/kill:  按自己的句柄表回收（PORT 反推 ticket 调 ipc_core_close）
```

### spawn / wait / kill / exit

```text
spawn: fs_sys_read_file -> elf64_check/load -> as_create -> 栈 4 页 + 标记页
       -> argv 块 -> thread_create -> syscall 栈 -> THREAD 句柄
fail:  杀线程 -> 还句柄 -> as_destroy -> 标记退出 + 回收槽位 -> 释放 ELF 缓冲
wait:  child->wait_tid = 本线程；BLOCKED 到 child 变 ZOMBIE；再 proc_reap
exit:  统计 -> 关端口/句柄 -> as_destroy -> 孩子重挂 init -> 僵尸 -> wake(parent)
kill:  ipc_core_cancel_thread -> sched_kill_thread -> 关端口/句柄 -> as_destroy
       -> 状态 0x100|sig -> wake(parent)
```

## 自检项清单

1. spawn：VFS 读 ELF、新 AS、16 KiB 用户栈、标记页、argv 块、新线程；
   `ng: proc spawn ok (pid=2 path=/bin/proc)`。
2. wait：阻塞到子进程 ZOMBIE、status=0、ppid/as_pml4/tid 校验；
   `ng: proc wait ok (status=0 pid=2 tid=15)`。
3. 3 进程链式 ping（hello→relay→server、4 条消息、两次 reply）；
   `ng: proc ipc cross ok (pid=3 procs=3 msgs=4 exits=3)`。
4. AS 隔离：同一 VA、不同物理页、内容独立；
   `ng: proc as isolated ok (va=0x40010000 p1=.. p2=.. tags=111/222)`。
5. kill 阻塞在 recv 的进程：status=265、等待者 0、可回收；
   `ng: proc kill ok (pids=6,7 status=265 waiters=0)`。
6. 每进程句柄表：spawn 不继承、只回收自己的端口（host 单测 + 自检 live=1）。
7. host 单测 4043 checks / 0 failures（+840）。
8. 门禁：run-ng 118+42、Smoke 103+38、NoQmp 102+38、Fallback 117+36、
   qemu64 118+42；parity 100/100（host 95/95）；ci-ng 7/7；旧 32 位 305/305。

## 未做 / 遗留（按 WP 边界）

- **用户态异常仍 panic**：trap 的故障策略未改（用户 #PF -> 杀进程属于 E3
  故障注入）；`kill` 杀不掉在 ring3 死循环的进程（仍无 tick IRQ/抢占）。
- **capability 未实现**：ticket 即能力（知道就能 send/reply，`from` 由发送方
  填）；`cap_grant/revoke` 与 ticket 防伪留给 ABI 1.0 / E3。
- **每进程 fd 表 / cwd 未拆**：VFS ctx 仍是全局一份，`proc->cwd` 只是占位；
  每进程命名空间属于 fsd / WP3-x。
- **单线程进程**：无 fork/线程组/进程组；`kill` 的信号只记状态码，不做异步投递。
- **wait 只等直接子进程**（无 `wait -1`/选项/超时）；阻塞用大 deadline + 显式唤醒。
- **没有后台收尸**：孤儿重挂到 init，但 init 只在显式 `wait` 时回收；长跑会话的
  孤儿僵尸回收留给 WP3-3 session manager（本 WP 所有路径都显式 wait）。
- **无共享内存数据面**：跨进程消息仍 ≤ 4 KiB。
- **无 ASID/PCID**：每次切进程全刷 TLB（正确，E4 再优化）。
- **spawn 上限**：ELF 256 KiB、argv 8 项、path 63 字节。

## 出口

```text
powershell -File tools/build-ng.ps1              # PASS，1977448 bytes，55 步
powershell -File tools/build-ng.ps1 -Smoke       # PASS（跳过 shell）
powershell -File tools/run-ng.ps1                # PASS，118 行 + 42 正则
powershell -File tools/run-ng.ps1 -Smoke         # PASS，103 行 + 38 正则
powershell -File tools/run-ng.ps1 -NoQmp         # PASS，102 行 + 38 正则
powershell -File tools/run-ng.ps1 -Fallback      # PASS，117 行 + 36 正则
powershell -File tools/run-ng.ps1 -Cpu qemu64    # PASS，118 行 + 42 正则
powershell -File tools/unit-ng.ps1               # PASS，4043 checks, 0 failures
powershell -File tools/parity-ng.ps1             # PASS，100/100（host 95/95）
powershell -File tools/ci-ng.ps1                 # 7/7 PASS
powershell -File build.ps1 -Test                 # PASS，305/305（旧 32 位）
```
