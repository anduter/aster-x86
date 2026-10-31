# D1 工作包（WP5-0 … WP5-6）

> 状态：**规划**，2026-10-31；配套 `d1-plan.md` / `risks.md` / `posix-profile.md`。
> 执行口径：一个 WP 一个会话；每个 WP 有独立出口断言和 `docs/claims/ng-WP5-*.md`。
> 路径所有权：只改本 WP “允许路径”；跨路径先改本文件/`../decisions.md` 并说明。
> 构建：`tools/build-ng.ps1` 是共享单写者文件（见 §0.3），不要多个会话同时改。
> 编号：WP5-0…WP5-6 是**轨道**；`WP5-0a` 等是**可独立开工/验收的子包**（本文件
> 的“WP”默认指子包）。总共 **31 个子包**，约 **50–60 个会话**（含 4 个预检）。

## 0. 通用规则

### 0.1 一个 WP 的完成定义

1. 代码 + host 单测（纯状态机优先，QEMU 只验集成）；
2. 出口断言在 `run-ng` / 专用门禁 / 真机日志中**真实出现**；
3. `docs/claims/ng-WP5-x.md`：期望行、复现命令、证据文件、已知限制；
4. 更新对应设计文档（`docs/design/*-ng.md`）或 domain 页；
5. `tools/ci-ng.ps1`（至少 `-Quick` + 相关 QEMU 门禁）绿；不 push。

### 0.2 路径所有权

- 内核公共文件（`src/ng/main.c`、`syscall.[ch]`、`proc.[ch]`、`sched.c`、
  `tools/build-ng.ps1`）是**冲突热点**：同一时间只允许一个 WP 改；其他 WP 在
  claim 里写“需要谁加什么”，由 owner 合入。
- 新目录按域：L0 `src/ng/`；L1 `src/services/`；L1D `src/drivers/`；
  L2 `src/desktop/`；L3 `src/apps/`；A `libc/`、`sdk/`、`docs/abi/`；
  B `tools/`；第三方 `third_party/`。
- 不允许 `git add -A`；提交信息 `[ng][WP5-x] <动词>: <一句话>`。

### 0.3 构建单写者

`tools/build-ng.ps1` 每加一个源文件/参数都会改同一段。规则：

1. 一个里程碑内只指定一个“构建 owner”会话持有该文件；
2. 其他 WP 在 claim/issue 里列出需要新增的对象/参数，由 owner 合入；
3. 合并窗口在里程碑末，避免每天冲突。

### 0.4 ABI 1.1 号段

- `WP5-0a` 冻结新增号段/错误码/结构体；后续 WP **只使用**，不得重排/复用。
- ABI 1.0 的编号、错误码、结构体布局**不变**；新号从 48 起。
- 具体提案见 `posix-profile.md` §3；最终以 `docs/abi/abi-1.1.h` 为准。

### 0.5 会话估算

“1” = 1 个会话；“2” = 通常 2 个会话（实现 + 集成/证据）。只用于排期，
不作为出口条件。

## 1. 总表

| WP | 名称 | 轨道 | 依赖 | 会话 |
|---|---|---|---|---|
| WP5-0a | ABI 1.1 注册表 + `ENOSYS` | 内核地基 | — | 1 |
| WP5-0b | per-process fd / cwd / uid + fd 对象 | 内核地基 | 0a | 2 |
| WP5-0c | ring3 fault → 进程终止 + 信号骨架 | 内核地基 | 0a/0b | 2 |
| WP5-0d | `SHM_FREE` + peer 身份 + cap 位 | 内核地基 | 0a/0b/0c | 2 |
| WP5-0e | FPU/SIMD 上下文 | 内核地基 | 0b | 1 |
| WP5-0f | tick IRQ / HLT idle / 时间 / 熵 | 内核地基 | 0a | 2 |
| WP5-1a | POSIX 文件/进程 syscall | POSIX | 0a/0b/0c | 2 |
| WP5-1b | newlib 移植 + crt/syscall stubs | POSIX | 1a | 2 |
| WP5-1c | 内核线程（clone/futex/TLS） | POSIX | 0b/0e/0f/1a | 2 |
| WP5-1d | libpthread 子集 | POSIX | 1c | 1 |
| WP5-1e | 信号完成（handler/sigreturn/EINTR） | POSIX | 0c/1c | 2 |
| WP5-1f | zlib/libpng/FreeType | POSIX | 1b | 1 |
| WP5-1g | FFmpeg/SDL2 最小运行 | POSIX | 1b/1c/1d/1f | 2 |
| WP5-2a | 网卡 + ARP/IPv4/ICMP/UDP/DHCP/DNS | 网络 | 0b/0f | 2 |
| WP5-2b | TCP + socket + poll | 网络 | 2a | 2 |
| WP5-2c | 配置/cap/loopback/POSIX shim | 网络 | 1b/2b/0d | 1 |
| WP5-2d | 网络硬化/性能/丢包 | 网络 | 2b/2c | 1 |
| WP5-3a | BearSSL 移植 + TLS 1.2 | TLS/HTTP | 0f/2b | 2 |
| WP5-3b | X.509/信任库/证书验证 | TLS/HTTP | 3a | 1 |
| WP5-3c | HTTP/1.1 客户端 | TLS/HTTP | 2c/3b | 1 |
| WP5-4a | HTML/CSS 解析与 DOM | 浏览器 | 1b | 2 |
| WP5-4b | 布局 + FreeType 文本 | 浏览器 | 4a/1f | 2 |
| WP5-4c | 绘制/滚动/抓取/PNG | 浏览器 | 4b/3c | 2 |
| WP5-4d | 语料/健壮性/像素基线 | 浏览器 | 4c | 1 |
| WP5-5a | UEFI stub + GOP + memmap/RSDP | 真机 | 0f | 2 |
| WP5-5b | ACPI + LAPIC/IOAPIC + timer | 真机 | 5a | 2 |
| WP5-5c | SMP 启动（AP 停泊）+ 锁审计 | 真机 | 5b | 2 |
| WP5-5d | 目标机驱动 + 1h 稳定 | 真机 | 5c/2x/4c | 2 |
| WP5-6a | 浸泡 harness + 计数/泄漏阈值 | 浸泡 | 0f/2b | 1 |
| WP5-6b | idle/心跳/故障注入 | 浸泡 | 6a | 2 |
| WP5-6c | 8h+ 浸泡 + D1 发布 | 浸泡 | all | 2 |

## 2. WP5-0 内核缺口收口

> 目标：把 POSIX/网络/浏览器共用的“看不见的地基”一次收掉。**不含任何 POSIX
> 语义**；内核只增加通用机制。所有新增号在 0a 冻结。

### WP5-0a ABI 1.1 注册表 + `ENOSYS`

| 项 | 内容 |
|---|---|
| **范围** | 新增 ABI 1.1 号段/错误码/结构体提案定稿（从 48 起）；`syscall_dispatch()` 未分配号返回 `-ENOSYS(-38)`；`tools/abi-check.ps1` 同时校验 1.0（不变）与 1.1；SDK 头只追加；不实现任何新调用。 |
| **不做** | 不改 1.0 已有编号/错误码；不实现 POSIX；不动 `abi.h`（旧 32 位）。 |
| **允许路径** | `src/ng/syscall.h`、`src/ng/syscall.c`、`src/ng/main.c`、`tests/unit-ng/test_syscall.c`、`tools/unit-ng.ps1`、`tools/abi-check.ps1`、`docs/abi/abi-1.1.md`、`docs/abi/abi-1.1.h`、`sdk/include/aster/syscall.h`、`docs/claims/ng-WP5-0a.md` |
| **出口** | `ng: syscall ENOSYS ok (-38)`；未分配 2/15/48 号一致；1.0 的 29 个调用/28 错误码逐项不变；`abi-check` PASS 且打印 `1.1: N syscalls`；unit 覆盖 default 分支。 |
| **依赖** | 无（D1 起点） |

**关键设计**：`-ENOSYS` 只对“未分配号”生效（ABI 1.0 §10 第 1 项允许）；已分配
调用的坏参数仍返回原错误码（`EINVAL/EFAULT/...`），不能借机改语义。号段是
“预留提案”，未用的号保持未分配、不复用。

### WP5-0b per-process fd / cwd / uid + fd 对象

| 项 | 内容 |
|---|---|
| **范围** | 把全局 `vfs_ctx` 搬进 `proc`：每进程 fd 表、cwd、uid；进程退出/被 kill 关闭自己的 fd；`spawn` 不继承 fd（execve 才继承，见 1a）；`close(23)` 只认 fd，不再误关同数值的 ticket；fd 上限 16→32；引入 `struct file`（offset/flags/refcount/type）作为 fd 指向的对象，为 `dup/pipe/socket` 留扩展点。 |
| **不做** | 不实现 `dup/pipe/socket` 本身（1a/2b）；不做 fd 传递（SCM_RIGHTS）；不做每进程命名空间。 |
| **允许路径** | `src/services/fs/include/vfs.h`、`src/services/fs/vfs.c`、`src/ng/proc.h`、`src/ng/proc.c`、`src/ng/proc_table.c`、`src/ng/fs_syscall.c`、`src/ng/syscall.c`、`src/ng/syscall.h`、`tests/unit-ng/test_vfs.c`、`tests/unit-ng/test_proc.c`、`tools/unit-ng.ps1`、`docs/design/vfs-ng.md`、`docs/claims/ng-WP5-0b.md` |
| **出口** | `ng: fd per-process ok (a=.. b=..)`：A 打开的 fd 在 B 返回 `EBADF`；A 退出后 fd 计数回到基线；`chdir` 只影响自己；`getuid` 返回 `proc->uid`；`close(ticket)` 不再影响 fd；unit 覆盖 fd 表满/代际/退出回收。 |
| **依赖** | 0a |

**关键设计**：fd 数值仍是 `(gen<<16)|slot`，但**解析域**从“全局 VFS”改为
“当前进程的 fd 表”；`vfs_ctx` 里的 `cwd/uid` 同步搬走。`struct file` 的
refcount 为 `dup/fork/exec` 共享偏移做准备；`pipe` 的读端/写端也指向同一
`struct file` 的两个方向对象。fd 与 ticket 的数值歧义在 syscall 层按“调用号
决定命名空间”解决，不再靠数值区分。

### WP5-0c ring3 fault → 进程终止 + 信号骨架

| 项 | 内容 |
|---|---|
| **范围** | ring3 的 `#PF/#GP/#UD/#DE/#BP` 不再 panic：按向量映射到 `SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP`，终止当前进程（`0x100 OR sig` 状态），回收 AS/线程/句柄；内核自身 fault 仍 panic。增加 `proc->pending_signals`、默认动作表、阻塞 syscall 的 `EINTR` 唤醒；用户 handler 的安装/返回留 1e，但结构先留好。 |
| **不做** | 不做用户态 `sigaction/sigreturn`（1e）；不做 `fork` 的 fault 继承；不改变内核 fault 的 panic 语义。 |
| **允许路径** | `src/ng/trap.c`、`src/ng/isr.S`、`src/ng/syscall.c`、`src/ng/proc.c`、`src/ng/proc.h`、`src/ng/sched.c`、`src/ng/thread.h`、`tests/unit-ng/test_signal.c`、`tests/audit-ng/sys/audit_sys.c`、`tools/audit-ng-sys.ps1`、`docs/design/trap-syscall-ng.md`、`docs/claims/ng-WP5-0c.md` |
| **出口** | `ng: user fault killed pid=.. sig=11`；`audit-ng-sys -Scenario crash` 从“预期 PANIC”改为“预期 kill + 内核存活”；连续 100 次 fault 后进程/线程/AS 计数回基线；shell/WM 不受影响；unit 覆盖向量→信号映射。 |
| **依赖** | 0a/0b |

**关键设计**：fault 发生在 CPL3 时，trap 入口已经在内核栈上；不能“恢复用户
现场”，只能走进程退出路径。退出前必须：摘 IPC 等待者、关端口、释放 shm/fb
映射、关闭 fd（0b）、回收 AS；**不能在持有调度器/堆锁的路径里做完整回收**，
沿用 WP3-0 kill 的“摘除 + 延迟回收”顺序。`EINTR` 只对可中断的阻塞 syscall
（recv/wait/nanosleep）生效，已返回部分数据的读写不重试。

### WP5-0d `SHM_FREE` + peer 身份 + cap 位

| 项 | 内容 |
|---|---|
| **范围** | 新增 `shm_free(handle)`（0a 号段）：最后一个映射解除或显式 free 后回收对象/页；displayd 的 DESTROY 真正释放 surface 像素（修 8 对象耗尽）。IPC `from` 改为**内核填/校验**：发送方不填时用进程默认回复端口，填了必须属于自己；接收方看到可信 peer。增加最小 cap 位（`CAP_FB`、`CAP_NET`、`CAP_SPAWN`）与 `spawn_ex(path,argv,caps,&pid)`；`fb_map` 要求 `CAP_FB`，socket 要求 `CAP_NET`；默认不授予。修 kill 阻塞 IPC 进程时的 kbuf 泄漏（TCB 登记阻塞缓冲）。 |
| **不做** | 不做完整 capability 表/撤销链；不做签名/uid 沙箱；cap 在 D1 只是“最小化门禁”，不是安全边界（所有进程仍 uid=0）。 |
| **允许路径** | `src/ng/shm.h`、`src/ng/shm.c`、`src/ng/shm_core.c`、`src/ng/syscall.h`、`src/ng/syscall.c`、`src/ng/ipc.c`、`src/ng/ipc_core.c`、`src/ng/handle.c`、`src/ng/proc.h`、`src/ng/proc.c`、`src/ng/fbmap.c`、`src/desktop/display/displayd.c`、`src/desktop/session/sessiond.c`、`tests/unit-ng/test_shm.c`、`tests/unit-ng/test_ipc.c`、`tests/unit-ng/test_proc.c`、`docs/design/ipc-ng.md`、`docs/design/display-server-ng.md`、`docs/claims/ng-WP5-0d.md` |
| **出口** | `ng: shm free ok (objects=0)`：1000 次 alloc/free 后对象/页/句柄零增长；`ng: peer from ok`：伪造 `from` 被拒/被覆盖；`ng: cap gate ok (fb=deny net=deny)` + 授权后成功；kill 阻塞 sender 后 `kmalloc` 计数回基线；displayd 连续 CREATE/DESTROY 100 次不 `ENOSPC`；unit 覆盖 free 重入/双 free/映射未解。 |
| **依赖** | 0a/0b/0c |

**关键设计**：`shm_free` 的语义必须是“**最后一个映射解除并显式 free 才回收**”，
否则客户端还在画就被 displayd 释放，会 UAF。`SHM_FREE` 与进程退出回收共用
`shm_core` 的 refcount；拒绝 double free / 非 owner。peer 身份填进 `ipc_msg_hdr.from`
后，displayd 的 WM 令牌校验、sessiond 的端口绑定才有可信基础（但 D1 仍不把
它当完整安全边界）。

### WP5-0e FPU/SIMD 上下文

| 项 | 内容 |
|---|---|
| **范围** | 开 `CR0.MP=1/EM=0/TS=0`、`CR4.OSFXSR/OSXMMEXCPT`；每线程 512 B 对齐 `FXSAVE` 区；`switch_context` 保存/恢复 XMM/MXCSR；用户构建从 `-mno-sse -mgeneral-regs-only` 切到 `-msse2 -mfpmath=sse`（内核仍 no-sse）；新增 SSE 自检/多线程浮点 demo。 |
| **不做** | 不做 AVX/AVX-512/XSAVE 动态区（D2 若需要）；不做 lazy FPU 切换优化；不改内核 ABI 的寄存器承诺。 |
| **允许路径** | `src/ng/paging.c`、`src/ng/thread.h`、`src/ng/sched.c`、`src/ng/sched_arch.c`、`src/ng/switch.S`、`src/ng/proc.c`、`src/apps/fpu/fpu.c`、`tools/build-ng.ps1`、`tools/run-ng.ps1`、`tests/unit-ng/test_sched.c`、`docs/design/sched-ng.md`、`docs/claims/ng-WP5-0e.md` |
| **出口** | `ng: fpu ok (sse=..)`：用户态 `sqrt/float` 计算正确；两个线程各算不同浮点、切换后结果不串；`build-ng` 用户 ELF 含 SSE 指令且无 #UD；host 单测覆盖 FXSAVE 区对齐/大小。 |
| **依赖** | 0b（线程结构） |

**关键设计**：内核仍用 `-mgeneral-regs-only` 编译，所以 syscall/IRQ 不需要保存
XMM；只在**用户线程切换**时 FXSAVE/FXRSTOR。`FXSAVE` 区必须 16 B 对齐（建议
64 B），放在 TCB 尾部静态数组，避免分配失败。SSE 打开后，用户栈对齐必须遵守
SysV（crt0 保证 16 B）；否则 `movaps` 会 #GP。

### WP5-0f tick IRQ / HLT idle / 时间 / 熵

| 项 | 内容 |
|---|---|
| **范围** | PIC 重映射 + PIT 1000 Hz；IRQ0 只累加 tick/唤醒睡眠队列，不在 IRQ 里调度；无 ready 线程时 `sti; hlt`，tick 唤醒后继续；TSC 校准（PIT 基准）；CMOS RTC 读墙钟；`clock_gettime(CLOCK_MONOTONIC/REALTIME)`、`nanosleep`、`getrandom`（RDRAND/RDSEED，缺失时 SHA-256 DRBG + TSC/RTC/中断抖动）；调度器睡眠按 ms。 |
| **不做** | 不做**内核**抢占式时间片（ring3 抢占见 WP5-0g）；不做 APIC timer（5b）；不做 NTP；不做高精度 `<1ms` 睡眠；不做 RTC 闹钟。 |
| **允许路径** | `src/ng/pic.c`、`src/ng/pic.h`、`src/ng/pit.c`、`src/ng/pit.h`、`src/ng/rtc.c`、`src/ng/rtc.h`、`src/ng/time.c`、`src/ng/time.h`、`src/ng/random.c`、`src/ng/random.h`、`src/ng/isr.S`、`src/ng/trap.c`、`src/ng/sched.c`、`src/ng/sched_arch.c`、`src/ng/main.c`、`src/ng/syscall.c`、`src/ng/syscall.h`、`tests/unit-ng/test_time.c`、`tools/build-ng.ps1`、`tools/unit-ng.ps1`、`tools/run-ng.ps1`、`docs/design/sched-ng.md`、`docs/claims/ng-WP5-0f.md` |
| **出口** | `ng: timer irq ok (hz=1000)`；`ng: idle hlt ok (cpu<5%)`（宿主 1 min 采样）；`ng: clock ok (realtime=.. mono=..)`（与宿主差 <5s）；`ng: nanosleep ok (err<=2ms)`；`ng: random ok`（重复种子测试）；1h 空闲串口零增长。 |
| **依赖** | 0a（syscall 号） |

**关键设计**：当前内核“IF=0 + 协作式”假设必须保留：IRQ handler 只做
`tick++`、把到期线程从睡眠链搬到 ready（**不 switch**），调度仍发生在
`thread_sleep/wake`/syscall 返回点。`hlt` 前要确认没有 ready 线程；IRQ 返回后
重新检查。`clock_gettime` 的 REALTIME 在 boot 时读 RTC 一次，之后 = RTC 基准 +
单调 tick；RTC 读失败则用编译日期/`0`，并在证据里标注。

### WP5-0g ring3 抢占（D1 前置）

| 项 | 内容 |
|---|---|
| **范围** | IRQ0 tick 置 need_resched；IRQ 返回 ring3 时若 need_resched/时间片到期，保存用户现场并切到调度器；内核仍 IF=0/协作，不在 IRQ 里 switch；时间片默认 10ms（1000Hz 下 10 tick）。 |
| **不做** | 不做内核抢占；不做 SMP/APIC timer（5b）；不做 RT 优先级/期限调度。 |
| **允许路径** | src/ng/isr.S、src/ng/trap.c、src/ng/sched.c、src/ng/sched_arch.c、src/ng/thread.h、src/ng/main.c、tests/unit-ng/test_sched.c、tools/run-ng.ps1、docs/design/sched-ng.md、docs/claims/ng-WP5-0g.md |
| **出口** | ng: tick preempt ok (hz=1000 slice=10ms)；WM 自旋时 shell/其他应用仍可运行，可 kill WM 并恢复面板；CPU-bound ring3 进程不能饿死 shell；unit 覆盖 need_resched/时间片边界。 |
| **依赖** | 0f |

**关键设计**：IRQ0 只做 tick++、唤醒到期线程、置 need_resched；返回路径在 iretq 前检查该标志，若目标仍是 ring3 且时间片到期，保存现场后走 sched_yield()。这样不需要把内核改成可抢占，也不在 IRQ handler 里切栈。

## 3. WP5-1 POSIX Profile

> 目标：newlib + Aster 包装 + 内核机制；应用静态链接，源码兼容。**不做**
> 动态链接、fork/COW、完整 POSIX。号段见 `posix-profile.md` §3。

### WP5-1a POSIX 文件/进程 syscall

| 项 | 内容 |
|---|---|
| **范围** | 实现 `fstat/lseek/dup/dup2/pipe/chdir/getcwd/getuid/waitpid/execve/getdents/readlink/rename/rmdir/fcntl`；`pipe` 4–64 KiB 环形缓冲 + 阻塞/非阻塞；`dup` 共享 `struct file`（0b）；`execve` 保留 pid/fd（CLOEXEC 关闭）/cwd/信号处置重置，失败保留旧镜像；`waitpid` 支持 `WNOHANG`；fd `read/write` 单次上限 512 B → 64 KiB（kmalloc bounce + uaccess 分页校验）；目录 fd 用 `getdents` 枚举（不再受 `listdir` 8 条限制）。 |
| **不做** | `fork/vfork`（返回 `ENOSYS`）；文件锁/`fcntl` 高级命令（只做 `F_GETFL/F_SETFL`）；`mmap` 文件映射；`ftruncate`/`fsync`（可后续追加）。 |
| **允许路径** | `src/ng/syscall.h`、`src/ng/syscall.c`、`src/ng/fs_syscall.c`、`src/ng/fs_syscall.h`、`src/services/fs/include/vfs.h`、`src/services/fs/vfs.c`、`src/services/fs/ramfs.c`、`src/services/fs/fat16.c`、`src/ng/proc.c`、`src/ng/proc.h`、`src/ng/main.c`、`tests/unit-ng/test_vfs.c`、`tests/unit-ng/test_proc.c`、`tools/unit-ng.ps1`、`tools/abi-check.ps1`、`docs/abi/abi-1.1.h`、`docs/abi/abi-1.1.md`、`docs/design/vfs-ng.md`、`docs/claims/ng-WP5-1a.md` |
| **出口** | `posix: fstat/lseek/dup/dup2/pipe/chdir/getcwd/getuid/waitpid/execve/getdents/fcntl ok`；`posix: pipe 64KiB ok` + `poll` 可读/可写；`posix: execve keeps fd/cwd ok` + 失败保留旧镜像；`posix: read 64KiB ok`；`posix: getdents 128 entries ok`；unit 覆盖坏 fd/跨页/CLOEXEC/僵尸子进程。 |
| **依赖** | 0a/0b/0c |

**关键设计**：`execve` 不是“spawn 后替换”，要在当前进程内完成：新 AS 加载 →
用户栈重建 → 切 CR3 → 释放旧 AS；任何失败必须在切换前回滚。`getdents`
把目录 fd 的 `offset` 当索引，返回 §3.3 的 88 B `ng_dirent` 记录（不是
80 B `vfs_dirent`），libc 再转 `struct dirent`。`pipe` 用 `struct file` 的
`type=PIPE`，读写端各一个对象，引用计数到 0 才释放缓冲。

### WP5-1b newlib 移植 + crt/syscall stubs

| 项 | 内容 |
|---|---|
| **范围** | 固定 newlib 版本（pin 哈希）；构建 `x86_64-elf` 静态 `libc.a/libm.a`；`libc/aster` 提供 crt0（`_start` 取 argc/argv/envp、调 `main`、`exit`）、syscall stubs（`_read/_write/_open/_close/_lseek/_fstat/_sbrk/_gettimeofday/_kill/_getpid/_exit/_isatty`）、`errno`（TLS 指针）、`opendir/readdir` 适配、`system()`（走 `posix_spawn`）、`assert`、`setjmp/longjmp`（若 newlib 缺）；`malloc` 走 `brk`；stdio 缓冲。 |
| **不做** | 不移植 newlib 的完整 locale/宽字符（C locale 即可）；不做动态链接；不实现 `fork`；不替换 newlib 的 libm（用它的）。 |
| **允许路径** | `third_party/newlib/**`（pin 清单/补丁）、`libc/**`、`tools/build-libc.ps1`、`tools/build-ng.ps1`、`tools/posix-check.ps1`、`tests/posix/**`、`docs/design/libc-ng.md`、`docs/claims/ng-WP5-1b.md` |
| **出口** | `posix: newlib hello`（printf/exit）；`posix: stdio ok`（fopen/fread/fwrite/fseek/fflush）；`posix: malloc ok`（brk 扩堆/对齐/`free` 合并）；`posix: time ok`；`posix: zlib build ok`（zlib 源码用该 libc 编过并跑 inflate）。 |
| **依赖** | 1a |

**关键设计**：newlib 的 `_sbrk` 只返回连续 VA；内核 `brk` 负责按页映射/回收。
`errno` 必须是每线程（1c 后），否则 pthread 会串。newlib 的 `signal()` 不走
它的 stub，改由 Aster libc 的 `signal.c` 直接实现（1e），避免和 newlib 的
`__sigtramp` 打架。构建脚本只产出静态库，不把 newlib 源码编进内核镜像。

### WP5-1c 内核线程（clone/futex/TLS）

| 项 | 内容 |
|---|---|
| **范围** | `clone(CLONE_VM + CLONE_FS + CLONE_FILES + CLONE_THREAD + CLONE_SIGHAND + CLONE_SETTLS + ...)` 最小集；每进程多线程共享 AS/fd 表/cwd/信号处置；每线程内核栈 + FPU 区 + TID；`futex`（WAIT/WAKE/REQUEUE 最小）；`arch_prctl(ARCH_SET_FS)` 设 TLS 基；`exit_thread`/`gettid`；最后一个线程退出才回收进程；线程上限（每进程 16、全局 64）与回收。 |
| **不做** | 不做 `CLONE_NEWNS`/容器、不做进程组/会话、不做 CPU 亲和、不做优先级；调度仍协作式（只在阻塞/让出点切换）。 |
| **允许路径** | `src/ng/thread.h`、`src/ng/sched.c`、`src/ng/sched_arch.c`、`src/ng/switch.S`、`src/ng/proc.h`、`src/ng/proc.c`、`src/ng/proc_table.c`、`src/ng/syscall.h`、`src/ng/syscall.c`、`src/ng/isr.S`、`src/ng/trap.c`、`src/ng/useras.c`、`src/ng/paging.c`、`tests/unit-ng/test_sched.c`、`tests/unit-ng/test_proc.c`、`src/apps/pthread/pthread_demo.c`、`tools/build-ng.ps1`、`tools/run-ng.ps1`、`docs/design/sched-ng.md`、`docs/design/proc-ng.md`、`docs/claims/ng-WP5-1c.md` |
| **出口** | `posix: clone 8 threads ok`（共享 fd/cwd、独立栈/TLS）；`posix: futex wait/wake ok`；`posix: join/exit ok`；最后一个线程退出后进程回收、AS 释放；线程上限返回 `EAGAIN`；unit 覆盖 TID/回收/共享表。 |
| **依赖** | 0b/0e/0f/1a |

**关键设计**：`proc` 从“1 主线程”变成“1 AS + N 线程 + 共享表”；`thread_t`
增加 `proc` 反向指针、TLS 基、`clear_child_tid`。进程退出条件从“主线程退出”
改为“最后一个线程退出”。信号是**进程级** pending，线程级 mask 在 1e 加。
协作式下 `futex` 唤醒必须走 `thread_wake`，不能忙等；`CLONE_VM` 的线程共享
页表，但每线程栈由 libc `mmap` 分配（1a 的匿名 `mmap` 可在 1c 补）。

### WP5-1d libpthread 子集

| 项 | 内容 |
|---|---|
| **范围** | `pthread_create/join/detach/self/exit/equal`；`mutex`（normal/recursive/trylock）、`cond`（wait/timedwait/signal/broadcast）、`once`、`rwlock`（最小）、`pthread_key_*`（TLS）、`sched_yield`；`sem_*`（最小，SDL2/FFmpeg 可能用）。 |
| **不做** | 不做 `pthread_cancel`、`pthread_atfork`、优先级/调度策略、`pthread_spinlock` 完整实现；不做 `fork` 后的锁重置。 |
| **允许路径** | `libc/pthread/**`、`libc/include/pthread.h`、`libc/include/semaphore.h`、`tests/posix/pthread_test.c`、`tools/posix-check.ps1`、`docs/design/libc-ng.md`、`docs/claims/ng-WP5-1d.md` |
| **出口** | `posix: pthread stress ok (8x10000)`（mutex 计数无丢失）；`posix: cond ok`；`posix: tls keys ok`；`posix: timedwait ok`（超时/EINTR）；无死锁（10 min 压力）。 |
| **依赖** | 1c |

**关键设计**：mutex 的 fast path 用原子 CAS（单核也需 `lock cmpxchg` 保证
IRQ 语义），慢路径 `futex_wait`；`cond` 用 futex 的 requeue 或简单 broadcast
+ 序列号。`pthread_create` 的栈用 `mmap`（默认 64 KiB，可配），guarded page
可选。`errno`/TLS 必须每线程。

### WP5-1e 信号完成（handler/sigreturn/EINTR）

| 项 | 内容 |
|---|---|
| **范围** | `rt_sigaction/rt_sigprocmask/rt_sigreturn`；用户 handler 调用（构造用户栈帧、`sigreturn` 恢复上下文）；默认动作（term/ignore/stop 的最小集）；`SIGCHLD`（子进程退出）、`SIGPIPE`（socket/pipe 写无读者）、`SIGINT`（键盘 Ctrl-C）、`SIGTERM`；`kill()` POSIX 语义（新号，见 posix-profile）与强制终止分离；阻塞 syscall 的 `EINTR`/`SA_RESTART` 最小语义。 |
| **不做** | `SA_SIGINFO`/`siginfo_t` 完整、altstack、实时信号排队、`SIGSTOP/SIGCONT` 完整作业控制、信号打断内核态长操作。 |
| **允许路径** | `src/ng/signal.c`、`src/ng/signal.h`、`src/ng/trap.c`、`src/ng/isr.S`、`src/ng/syscall.c`、`src/ng/syscall.h`、`src/ng/proc.c`、`src/ng/proc.h`、`src/ng/thread.h`、`src/ng/sched.c`、`libc/signal/**`、`tests/posix/signal_test.c`、`tools/posix-check.ps1`、`docs/design/trap-syscall-ng.md`、`docs/claims/ng-WP5-1e.md` |
| **出口** | `posix: sigusr1 handler ok`；`posix: sigsegv default kill ok`；`posix: sigchld ok`；`posix: sigpipe ok`；`posix: sigint ctrl-c ok`；`posix: EINTR ok`（阻塞 read 被信号打断）；handler 返回后寄存器/栈恢复。 |
| **依赖** | 0c/1c |

**关键设计**：handler 帧放在用户栈（不能放内核栈），包含保存的 GPR/`RIP/RSP/
RFLAGS` + 信号号；`sigreturn` 从用户栈恢复。返回路径用 `iretq`，不碰
SYSRET 的 CPL3 限制。信号投递点选在 syscall/trap 返回用户前；内核态不可
被信号打断，阻塞 syscall 被唤醒后返回 `-EINTR`（或 `SA_RESTART` 重试）。

### WP5-1f zlib / libpng / FreeType

| 项 | 内容 |
|---|---|
| **范围** | pin 并构建 zlib、libpng、FreeType（`--with-zlib`、无 HarfBuzz、无 brotli）；提供最小 `fontdemo`（加载 TTF、渲染一行 ARGB）和 `pngdemo`（解码 PNG、CRC 校验、缩放）；字体/图片资产与许可证；3 个库的 `posix-check` 用例。 |
| **不做** | 不做字体子集化/可变字体/彩色字体；不做 JPEG/GIF/WebP（B0 先 PNG）；不做 HarfBuzz 复杂排版；不做字体缓存服务。 |
| **允许路径** | `third_party/zlib/**`、`third_party/libpng/**`、`third_party/freetype/**`（pin/补丁）、`assets/fonts/**`、`assets/images/**`、`src/apps/posix/fontdemo.c`、`src/apps/posix/pngdemo.c`、`tools/posix-check.ps1`、`tests/posix/**`、`docs/design/posix-libs.md`、`docs/claims/ng-WP5-1f.md` |
| **出口** | `posix: zlib inflate/deflate ok (crc=..)`；`posix: libpng ok (w=.. h=..)`；`posix: freetype ok (glyphs=..)`；渲染出的 ARGB 关键像素/文本位置断言；字体许可证入库。 |
| **依赖** | 1b |

**关键设计**：FreeType 默认可能启用浮动点路径，0e 已开 SSE；构建时关闭
`FT_CONFIG_OPTION_USE_BROTLI`、`FT_CONFIG_OPTION_USE_HARFBUZZ`。字体用
DejaVuSans（Bitstream Vera 许可，可再分发）或 Noto Sans 子集；二进制资产
放 `assets/` 并记录 SHA-256。PNG 只支持 8-bit RGB/RGBA，足够 B0。

### WP5-1g FFmpeg / SDL2 最小运行

| 项 | 内容 |
|---|---|
| **范围** | FFmpeg：`--disable-everything --disable-x86asm` + 启用 PNG/MJPEG/H.264 解码、mov/png demuxer、rawvideo/swscale；`ffmpeg_demo` 解一帧到 RGB 并打哈希。SDL2：`--disable-audio/joystick/haptic/power`，自定义 `aster` video driver（displayd SHM）+ 键盘事件；`sdl2_demo` 开窗口、画渐变/文字、按键退出。 |
| **不做** | 不做完整播放器/音频/字幕/硬件加速/多线程解码（线程支持在 1c/1d，但 demo 用单线程）；不保证所有 codec；不把 FFmpeg CLI 作为 D1 出口。 |
| **允许路径** | `third_party/ffmpeg/**`、`third_party/sdl2/**`（pin/补丁）、`src/apps/posix/ffmpeg_demo.c`、`src/apps/posix/sdl2_demo.c`、`tools/posix-check.ps1`、`tests/posix/**`、`docs/design/posix-libs.md`、`docs/claims/ng-WP5-1g.md` |
| **出口** | `posix: ffmpeg frame ok (codec=.. w=.. h=.. sha=..)`；`posix: sdl2 window ok` + 键盘事件 + 像素断言；目标二进制无 `libssl/libcrypto` 依赖。 |
| **依赖** | 1b/1c/1d/1f |

**关键设计**：FFmpeg 的 configure 假设 POSIX/Linux 环境，交叉编译需要
`--target-os=none --arch=x86_64 --disable-programs --disable-doc` 等补丁；
如果 configure 过不去，降级为“只编译 libavcodec 的 PNG/JPEG 解码 + swscale”，
H.264 留 D2（风险见 `risks.md` R1）。SDL2 的 video driver 是 B0 之外的第二个
display 客户端，可作为 display 协议的回归。

## 4. WP5-2 网络栈

> 目标：L1 网络库（暂留内核）+ L1D 网卡 + native socket syscall；POSIX socket
> 在 2c。**不做** IPv6/分片重组/Wi-Fi/netd 进程。

### WP5-2a 网卡 + ARP/IPv4/ICMP/UDP/DHCP/DNS

| 项 | 内容 |
|---|---|
| **范围** | e1000（QEMU 82540EM）+ virtio-net（modern PCI）驱动：PCI 探测、MMIO/DMA 环、RX/TX、统计、polling + IRQ 可选；ARP 缓存/请求队列；IPv4 收发/校验和；ICMP echo；UDP；DHCP 客户端（DISCOVER/OFFER/REQUEST/ACK）；DNS A 解析；loopback（127.0.0.1）；`net_env` host 抽象；`/run/net.status` 或串口统计。 |
| **不做** | TCP（2b）；IP 分片重组；IPv6；DHCP RENEW/REBIND；DNS CNAME/AAAA/缓存；netd 进程；无线。 |
| **允许路径** | `src/drivers/net/e1000.c`、`src/drivers/net/virtio_net.c`、`src/drivers/include/e1000.h`、`src/drivers/include/virtio_net.h`、`src/services/net/**`、`src/ng/net.c`、`src/ng/net.h`、`src/ng/net_selftest.c`、`src/ng/main.c`、`tools/build-ng.ps1`、`tools/run-ng.ps1`、`tools/net-check.ps1`、`tests/unit-ng/test_net.c`、`docs/design/net-ng.md`、`docs/claims/ng-WP5-2a.md` |
| **出口** | `net: e1000 up (mac=..)` / `net: virtio-net up`；`net: dhcp ok ip=.. gw=.. dns=..`；`net: dns ok example.com=..`；`net: icmp echo ok`；`net: udp echo ok`；host 单测覆盖校验和/ARP/选项解析；1h 启动无 panic。 |
| **依赖** | 0b/0f |

**关键设计**：驱动不解析协议，协议不碰硬件；`net_env`（send_frame/rx_poll/
now/random）让 host 用 fake 帧跑单测。e1000 先照旧树移植（轮询），IRQ
后加；virtio-net 用 modern PCI capabilities（legacy 不做）。DMA 缓冲用
PMM 连续页 + 物理地址；MMIO 用 PCD/PWT 映射。DHCP/DNS 要有总预算（≤3s），
失败保留静态配置，不阻塞启动。

### WP5-2b TCP + socket + poll

| 项 | 内容 |
|---|---|
| **范围** | TCP 状态机（CLOSED/LISTEN/SYN_SENT/SYN_RCVD/ESTABLISHED/FIN_WAIT_1/2/TIME_WAIT/CLOSE_WAIT/LAST_ACK）；seq/ack、MSS 1460、接收窗 8–16 KiB；RTO（1ms tick、RFC6298 简版）、重传/退避、3 dupack 快重传、4 段乱序缓存、FIN/RST；socket 表（每进程、fd 类型 SOCKET）；`socket/bind/connect/listen/accept/sendto/recvfrom/setsockopt/getsockopt/shutdown/poll`；阻塞/非阻塞；`net_syscall.c` 胶水。 |
| **不做** | SACK、窗口缩放（除非 1 MiB 传输证明必须）、拥塞控制完整版、TCP 服务端高并发、`sendfile`、零拷贝。 |
| **允许路径** | `src/services/net/tcp.c`、`src/services/net/socket.c`、`src/services/net/**`、`src/ng/net_syscall.c`、`src/ng/syscall.h`、`src/ng/syscall.c`、`src/ng/proc.h`、`src/ng/proc.c`、`tools/build-ng.ps1`、`tools/net-check.ps1`、`tests/unit-ng/test_tcp.c`、`tests/unit-ng/test_socket.c`、`docs/design/net-ng.md`、`docs/claims/ng-WP5-2b.md` |
| **出口** | `net: tcp handshake ok`；`net: tcp 1MiB ok (sha=..)`（slirp 宿主 echo）；`net: tcp loss 10% ok (retx=..)`；`net: tcp reset ok`；`net: tcp timewait ok`；`sock: poll/nonblock ok`；`sock: listen/accept ok`；unit 覆盖 seq/窗口/超时/坏校验和/乱序。 |
| **依赖** | 2a |

**关键设计**：旧树 `tcp.c` 是“同步单段在途”，D1 需要真正的流式发送：发送
队列 + 窗口 + 重传定时器（由 0f 的 tick 驱动）。不能把 TCP 放在 syscall
上下文里等超时（旧树踩过 NMI watchdog）；阻塞 socket 用 `thread_sleep`，
定时器在 IRQ/tick 或 `net_poll()` 里推进。socket 表与 fd 表（0b）共用
`struct file`，`read/write/close/poll` 按类型分发。

### WP5-2c 配置 / cap / loopback / POSIX shim

| 项 | 内容 |
|---|---|
| **范围** | 静态 IP/掩码/网关/DNS + DHCP lease 应用；`/etc/resolv.conf` 或内存配置；`CAP_NET` 门禁（无 cap 的 socket 返回 `EPERM`）；loopback 接口；POSIX `socket/connect/send/recv/poll` 包装（`libc/net`）；`sockaddr_in`/`in_addr` 转换；`/run/net.status`；宿主测试服务器脚本（TCP echo/HTTP）。 |
| **不做** | 不做 netd 服务/端口；不做 `getaddrinfo` 完整（只 A/IN，同步）；不做 IPv6；不做多网卡路由。 |
| **允许路径** | `libc/net/**`、`src/services/net/config.c`、`src/services/net/**`、`src/ng/net_syscall.c`、`src/ng/proc.h`、`tools/net-check.ps1`、`tests/posix/socket_test.c`、`docs/design/net-ng.md`、`docs/claims/ng-WP5-2c.md` |
| **出口** | `posix: socket/connect/send/recv ok`；`posix: getaddrinfo A ok`；`posix: cap net deny ok`；`net: loopback ok`；`net: dhcp lease apply ok`；host 测试服务器可被 QEMU 访问。 |
| **依赖** | 1b/2b/0d |

**关键设计**：D1 的 cap 是“最小化门禁”，不是安全边界：所有进程 uid=0，
sessiond 可给浏览器授 `CAP_NET`；恶意进程仍可自己 `spawn_ex` 带 cap。文档
必须写清楚，避免误认为沙箱。`getaddrinfo` 只做同步 A 记录，失败返回
`EAI_NONAME`；B0 用非阻塞 connect + poll，不依赖它。

### WP5-2d 网络硬化 / 性能 / 丢包

| 项 | 内容 |
|---|---|
| **范围** | 10 MiB 传输、吞吐统计；10% 丢包/乱序/重复/坏校验和 fuzz；接收窗/批量 ACK 调优；virtio-net 完成路径；net 统计接入 `/run/stats`；1h 网络负载浸泡；修复 2a–2c 的边界 bug。 |
| **不做** | 新协议/新设备；不做完整拥塞控制；不做性能承诺（只记录基线）。 |
| **允许路径** | `src/services/net/**`、`src/drivers/net/**`、`tools/net-check.ps1`、`tools/soak-d1.ps1`、`tests/unit-ng/test_net.c`、`tests/unit-ng/test_tcp.c`、`docs/design/net-ng.md`、`docs/claims/ng-WP5-2d.md` |
| **出口** | `net: 10MiB ok (sha=..)`；`net: fuzz 10k packets no panic`；`net: 1h load ok`；统计字段完整；无 fd/socket/缓冲泄漏（1h 计数回基线）。 |
| **依赖** | 2b/2c |

## 5. WP5-3 TLS / HTTP

> 目标：用户态 TLS 1.2（BearSSL）+ X.509 + HTTP/1.1；**不依赖 OpenSSL**。
> TLS 1.3 明确留 D2（决策 D1-B）。

### WP5-3a BearSSL 移植 + TLS 1.2

| 项 | 内容 |
|---|---|
| **范围** | pin BearSSL 源码/许可；`x86_64-elf` 构建；`br_ssl_client` 状态机封装（非阻塞 handshake/read/write）；熵/时间回调（`getrandom/clock_gettime`）；AES-GCM/ChaCha20-Poly1305/ECDHE-RSA/ECDHE-ECDSA 密码套件；KAT（SHA-256/HMAC/AES-GCM/X25519/P-256）；本地 OpenSSL `s_server` 握手/回显。 |
| **不做** | TLS 1.3、会话恢复/0-RTT、客户端证书、OCSP/CRL、TLS 1.2 CBC 套件（禁用）。 |
| **允许路径** | `third_party/bearssl/**`（pin/补丁）、`libc/tls/**`、`tools/tls-check.ps1`、`tests/tls/**`、`docs/design/tls-http-ng.md`、`docs/claims/ng-WP5-3a.md` |
| **出口** | `tls: kat ok`；`tls: handshake ok (ver=1.2 cipher=..)`；`tls: echo ok`；`tls: bad record rejected`；目标镜像无 OpenSSL 符号。 |
| **依赖** | 0f/2b |

**关键设计**：BearSSL 默认阻塞式 I/O 回调；B0 需要非阻塞，封装成“状态机 +
`poll`”：握手/读写都返回 `WANT_READ/WANT_WRITE`，由事件循环推进。证书
验证回调接 3b。熵必须来自 0f 的 `getrandom`（RDRAND/RDSEED），不能用
时间戳伪随机。

### WP5-3b X.509 / 信任库 / 证书验证

| 项 | 内容 |
|---|---|
| **范围** | PEM→DER、链构建（≤4）、签名验证（RSA PKCS#1 v1.5 / ECDSA P-256）、有效期（`clock_gettime`）、主机名（SAN/CN、通配符只允许最左）、BasicConstraints、KeyUsage；Mozilla CA 子集（`assets/ca/*.pem`）与哈希；错误码映射；过期/自签/错主机/错签名/未知 CA 的反向用例。 |
| **不做** | CRL/OCSP、证书透明、客户端证书、证书缓存/更新通道（D2）。 |
| **允许路径** | `libc/tls/x509.c`、`libc/tls/**`、`assets/ca/**`、`tests/tls/**`、`tools/tls-check.ps1`、`docs/design/tls-http-ng.md`、`docs/claims/ng-WP5-3b.md` |
| **出口** | `tls: cert chain ok`；`tls: reject expired/self/hostname/badsig ok`；`tls: example.com chain ok`；信任库 SHA-256 固定。 |
| **依赖** | 3a |

### WP5-3c HTTP/1.1 客户端

| 项 | 内容 |
|---|---|
| **范围** | URL 解析（http/https、host:port、path/query）；DNS A；TCP connect（非阻塞）；TLS 包装；请求行/头（Host/User-Agent/Accept/Connection）；状态/头解析；Content-Length/chunked；301/302/303/307（≤5）；gzip（zlib）；超时；`file://`（B0 本地语料用）；`httpget` 工具。 |
| **不做** | HTTP/2、WebSocket、代理、认证、Cookie 罐、HSTS、下载管理器、POST 表单。 |
| **允许路径** | `libc/http/**`、`src/apps/httpget/**`、`tests/http/**`、`tools/tls-check.ps1`、`docs/design/tls-http-ng.md`、`docs/claims/ng-WP5-3c.md` |
| **出口** | `http: local 200 ok`、`http: redirect/chunked/gzip ok`；`http: https example.com ok (bytes=..)`；`http: 1MiB sha ok`；`http: timeout/bad cert rejected`；`httpget https://example.com` 打印标题/长度。 |
| **依赖** | 2c/3b |

**关键设计**：HTTP 客户端是 B0 的“取字节”层，不解析 HTML；返回状态码/
头/body 流。`file://` 走 VFS，方便本地语料在无网络时回归。所有 I/O 非阻塞，
B0 的事件循环可以直接驱动。

## 6. WP5-4 浏览器 B0

> 目标：L3 `/bin/browser`，自有引擎、无 JS；B0 子集固定，不追求真实网站
> 像素级复刻。B0 自身**单线程非阻塞**，不依赖 pthread；pthread 是给库用的。

### WP5-4a HTML/CSS 解析与 DOM

| 项 | 内容 |
|---|---|
| **范围** | HTML tokenizer（标签/属性/实体/注释/文本/`script/style` 跳过）+ 树构建（html/head/body、p/div/span/h1–h6/ul/ol/li/a/img/br/hr/pre/code/strong/em）；CSS 解析（规则/声明、tag/class/id/后代/子选择器、`,` 分组）；属性：color/background-color/font-size/font-weight/font-family/text-align/line-height/margin/padding/border-width/style/color/display（block/inline/none）/width/height；内联 `style`；特异性/继承最小集。 |
| **不做** | JS、`<table>`/`<form>`/`<video>`/`<canvas>`、CSS float/position/flex/grid/transform/animation、`@media`（可做 viewport 最基本）、`@font-face`。 |
| **允许路径** | `src/apps/browser/**`（parser/dom/css）、`tests/browser/**`、`tools/browser-check.ps1`、`docs/design/browser-ng.md`、`docs/claims/ng-WP5-4a.md` |
| **出口** | `browser: parse corpus 5/5 ok`；DOM dump 与期望树一致；CSS 计算值表；未知标签/属性忽略不崩；100 个畸形片段不越界。 |
| **依赖** | 1b |

### WP5-4b 布局 + FreeType 文本

| 项 | 内容 |
|---|---|
| **范围** | 块布局（width/height/margin/padding/border、auto 宽度、垂直排列）；行内布局（行盒、换行、空白折叠、`text-align`）；文本测量/绘制（FreeType、字号/粗体合成、行高、基线）；字体缓存；图片 intrinsic size（PNG）；`display:none`。 |
| **不做** | 表格布局、float、绝对定位、flex/grid、BiDi/复杂文字整形、连字、字距优化、字体子集。 |
| **允许路径** | `src/apps/browser/layout/**`、`src/apps/browser/text/**`、`src/apps/browser/font/**`、`assets/fonts/**`、`tests/browser/**`、`tools/browser-check.ps1`、`docs/design/browser-ng.md`、`docs/claims/ng-WP5-4b.md` |
| **出口** | `browser: layout 5/5 ok`（盒子坐标/尺寸基线）；`browser: text ok`（文本位置/像素基线）；长段落换行正确；字体缺失回退。 |
| **依赖** | 4a/1f |

### WP5-4c 绘制 / 滚动 / 抓取 / PNG

| 项 | 内容 |
|---|---|
| **范围** | displayd surface（CREATE/SHM_MAP/COMMIT/DESTROY）；ARGB 绘制（背景/边框/文本/图片）；滚动（方向键/PageUp/PageDown/Home/End，滚动条可选）；事件循环（`poll` socket + `readkey`）；`http(s)://` + `file://` 抓取；PNG 解码/绘制；加载中/错误页；窗口标题。 |
| **不做** | 鼠标点击/链接跳转、表单、下载、多标签、前进/后退历史、缩放。 |
| **允许路径** | `src/apps/browser/render/**`、`src/apps/browser/net/**`、`src/apps/browser/main.c`、`tests/browser/**`、`tools/browser-check.ps1`、`docs/design/browser-ng.md`、`docs/claims/ng-WP5-4c.md` |
| **出口** | `browser: surface ok`；`browser: local file ok`；`browser: https page ok (status=200)`；`browser: png ok`；`browser: scroll ok (y=max)`；键盘 `q` 退出；杀浏览器不影响 displayd/WM。 |
| **依赖** | 4b/3c |

### WP5-4d 语料 / 健壮性 / 像素基线

| 项 | 内容 |
|---|---|
| **范围** | 5 页本地语料（标题/段落/列表/链接/背景色/PNG）固定哈希；3 个真实 HTTPS 页面（example.com、纯文本页、带图片页）；100 个畸形 HTML/CSS 输入；像素基线（QMP screendump）；内存/句柄泄漏检查；加载时间基线。 |
| **不做** | 真实网站兼容性承诺；不修 CSS 子集之外的效果；不做自动化视觉回归服务。 |
| **允许路径** | `tests/browser/**`、`src/apps/browser/**`（只修 bug）、`tools/browser-check.ps1`、`docs/design/browser-ng.md`、`docs/claims/ng-WP5-4d.md` |
| **出口** | `browser: corpus 5/5 pass`；`browser: malformed 100/100 no crash`；`browser: https 3/3 ok`；`browser: leak ok`（10 次 reload 计数回基线）；截图/哈希入库。 |
| **依赖** | 4c |

## 7. WP5-5 真实硬件

> 目标：一款锁定 UEFI 机型；GOP 显示、ACPI/APIC、SMP 启动（AP 停泊）、
> 真网卡/键盘。**不做**安装器/更新/回滚（建议 D1.1）、GPU 3D、USB 存储。

### WP5-5a UEFI stub + GOP + memmap/RSDP

| 项 | 内容 |
|---|---|
| **范围** | PE32+ UEFI 应用；从 ESP 读 kernel ELF（或嵌入）；`GetMemoryMap`、`GOP` 查模式、`RSDP`、`ExitBootServices`；`bootinfo` 结构（memmap/GOP/RSDP/ACPI revision）；串口；OVMF/QEMU 测试；失败回退提示。 |
| **不做** | Secure Boot/shim、签名验证、UEFI 网络/PXE、安装器、多引导菜单。 |
| **允许路径** | `src/boot/uefi/**`、`src/ng/entry.S`、`src/ng/link.ld`、`src/ng/bootinfo.h`、`tools/build-uefi.ps1`、`tools/run-uefi.ps1`、`docs/design/uefi-ng.md`、`docs/claims/ng-WP5-5a.md` |
| **出口** | `hw: uefi boot ok (gop=1280x800 rsdp=..)`（OVMF）；GOP 像素可写；memmap 可用页 >0；ACPI RSDP 校验和正确；同镜像 BIOS/PVH 路径不回归。 |
| **依赖** | 0f |

### WP5-5b ACPI + LAPIC/IOAPIC + timer

| 项 | 内容 |
|---|---|
| **范围** | RSDP→XSDT/RSDT；MADT（CPU/LAPIC/IOAPIC/ISO）、FADT、MCFG、HPET；LAPIC MMIO、IOAPIC 重定向表、LAPIC timer 校准（HPET/PIT）；IRQ 路由（ISA 固定 + PCI MSI 优先）；无 ACPI 时回退 PIC/PIT；`acpi`/`apic` host 单测（表解析纯函数）。 |
| **不做** | AML 解释器/`_PRT` 完整解析（用 MSI/固定 IRQ 绕开）、S3/S4、CPPC/P-state、热插拔。 |
| **允许路径** | `src/ng/acpi.c`、`src/ng/acpi.h`、`src/ng/apic.c`、`src/ng/apic.h`、`src/ng/ioapic.c`、`src/ng/ioapic.h`、`src/ng/hpet.c`、`src/ng/hpet.h`、`src/ng/trap.c`、`src/ng/isr.S`、`src/ng/time.c`、`src/ng/main.c`、`tools/build-ng.ps1`、`tools/run-uefi.ps1`、`tests/unit-ng/test_acpi.c`、`docs/design/uefi-ng.md`、`docs/claims/ng-WP5-5b.md` |
| **出口** | `hw: acpi ok (rev=.. cpus=N)`；`hw: apic ok (lapic=.. ioapic=..)`；`hw: lapic timer ok (hz=..)`；键盘/NIC IRQ 可达；QEMU q35 + OVMF 1h 稳定。 |
| **依赖** | 5a |

### WP5-5c SMP 启动（AP 停泊）+ 锁审计

| 项 | 内容 |
|---|---|
| **范围** | MADT 枚举 AP；INIT/SIPI；AP 16 位→保护模式→长模式 trampoline；per-CPU GDT/TSS/IDT/内核栈/GS；AP 进入 `hlt` 停泊；IOAPIC 目标固定 BSP；启动期 spinlock/原子操作审计（PMM/heap/sched/IPC 的“IF=0 单核”假设逐项标注）；AP 崩溃不影响 BSP。 |
| **不做** | AP 上跑进程/中断负载均衡/多核调度、CPU 热插拔、per-CPU 调度器、RCU。 |
| **允许路径** | `src/ng/smp.c`、`src/ng/smp.h`、`src/ng/gdt.c`、`src/ng/trap.c`、`src/ng/sched.c`、`src/ng/paging.c`、`src/ng/main.c`、`src/ng/smp_trampoline.S`、`tools/build-ng.ps1`、`tools/run-uefi.ps1`、`tests/unit-ng/test_smp.c`、`docs/design/uefi-ng.md`、`docs/claims/ng-WP5-5c.md` |
| **出口** | `hw: smp boot ok (aps=N parked)`；所有 AP 在 `hlt`；BSP 继续跑 1h；无 triple fault；锁审计清单入库（每处“单核假设”有标注/后续项）。 |
| **依赖** | 5b |

### WP5-5d 目标机驱动 + 1h 稳定

| 项 | 内容 |
|---|---|
| **范围** | 锁定机型 bring-up：e1000e/82574 NIC、PS/2 键盘（或 xHCI HID boot keyboard）、GOP 帧缓冲接入 `fb_map`、可选 AHCI 只读；串口日志/截图；1h 负载；硬件清单/已知问题。 |
| **不做** | 第二款机型、Wi-Fi、USB 存储/音频、GPU 驱动、安装器/更新。 |
| **允许路径** | `src/drivers/net/e1000e.c`、`src/drivers/net/e1000e.h`、`src/drivers/input/xhci.c`、`src/drivers/input/xhci.h`、`src/drivers/block/ahci.c`、`src/drivers/include/ahci.h`、`src/ng/fbmap.c`、`src/ng/main.c`、`tools/hw-check.ps1`、`docs/design/hardware-d1.md`、`docs/claims/ng-WP5-5d.md` |
| **出口** | `hw: nic up (mac=.. link=up)`；`hw: keyboard ok`；`hw: browser https ok`；`hw: 1h stable (panic=0 reset=0)`；硬件清单（型号/固件/CPU/网卡/键盘/显示）入库。 |
| **依赖** | 5c/2b/3c/4c |

**目标机选型（M1 预检锁定）**：优先满足 UEFI GOP、PS/2 键盘（或可做
xHCI HID）、Intel 82574L/82579LM 网卡、SATA 可选、串口/调试可达。候选：
ThinkPad X230（PS/2、GOP、82579LM）或带 82574L 的台式机。**没有机器则
D1-E5 降级为 OVMF，必须负责人签字**（`risks.md` R3）。

## 8. WP5-6 长时浸泡与硬化

> 目标：带负载的 8h（真机）/24h（QEMU）长跑、计数/泄漏、故障注入、空闲
> 功耗。**不是**挂机看日志。

### WP5-6a 浸泡 harness + 计数/泄漏阈值

| 项 | 内容 |
|---|---|
| **范围** | `tools/soak-d1.ps1`：启动/采样（60s）/QMP 注入/命令注入/HTTP 抓取/浏览器 reload/结束；宿主 RSS/CPU/线程/QMP 状态；in-guest `/run/stats`（PMM free、heap、fd、handle、port、shm、proc、thread、tick、irq、net）；阈值与报告 JSON。 |
| **不做** | 不做遥测上传；不做 GUI 仪表盘；不做真机自动控制（5d 用手工/串口）。 |
| **允许路径** | `tools/soak-d1.ps1`、`src/ng/stats.c`、`src/ng/stats.h`、`src/ng/main.c`、`src/desktop/display/displayd.c`（状态字段）、`tests/soak/**`、`docs/design/d1/soak.md`、`docs/claims/ng-WP5-6a.md` |
| **出口** | 1h dry-run PASS；`/run/stats` 字段可读；阈值（如 PMM free 漂移 <1%、fd/handle/port/shm 不增长）有断言；报告可机读。 |
| **依赖** | 0f/2b |

### WP5-6b idle / 心跳 / 故障注入

| 项 | 内容 |
|---|---|
| **范围** | `hlt` idle + tick 唤醒的 CPU 占用修复；心跳行（60s：tick/proc/free）；watchdog（NMI/软锁检测只记录/打印，不自动重启）；故障注入：kill displayd/WM/sessiond/browser、NIC down/up、TCP RST、坏 TLS、OOM、坏 ELF；恢复断言；1h 负载 + 故障。 |
| **不做** | panic 自动重启、硬件看门狗、GPU 恢复（S3）、displayd 重连（S2）。 |
| **允许路径** | `src/ng/sched.c`、`src/ng/time.c`、`src/ng/watchdog.c`、`src/ng/watchdog.h`、`src/ng/trap.c`、`src/ng/main.c`、`tools/soak-d1.ps1`、`tools/d1-fault.ps1`、`docs/design/d1/soak.md`、`docs/claims/ng-WP5-6b.md` |
| **出口** | 空闲宿主 CPU <10%；`ng: heartbeat tick=.. procs=.. free=..`；故障矩阵 10/10 按 architecture §3 收敛；1h 无 panic。 |
| **依赖** | 6a |

### WP5-6c 8h+ 浸泡 + D1 发布

| 项 | 内容 |
|---|---|
| **范围** | 真机 8h + QEMU 24h（最低 8h）带负载；故障注入穿插；最终门禁（ci-ng、posix/net/tls/browser/hw/soak）；`docs/release/d1.md`、`docs/design/d1/d1-report.md`、兼容矩阵、已知限制；D1 tag 建议（负责人打）。 |
| **不做** | 签名发布/OTA/安装器；不 push。 |
| **允许路径** | `tools/release-d1.ps1`、`docs/release/d1.md`、`docs/design/d1/d1-report.md`、`docs/design/d1/*.md`、`docs/claims/ng-WP5-6c.md` |
| **出口** | `soak: 8h pass`（真机）+ `soak: 24h pass`（QEMU）；0 panic/0 冻结/0 重启；命令 32/32；HTTP 抓取 100%；浏览器 reload 无崩溃；D1-E1…E8 逐项勾选。 |
| **依赖** | 全部 |

## 9. 关键路径与并行建议

```text
M1: 0a → 0b → 0c/0d；0e/0f 与 0b 并行；S1/S3 并行
M2: 1a→1b→1c→1d（用户态车道）  ||  2a→2b→2c（网络车道）
M3: 1e/1f/1g（用户态）          ||  3a→3b→3c（TLS/HTTP，依赖 2b）
M4: 4a→4b→4c→4d（浏览器）       ||  2d（网络硬化）
M5: 5a→5b→5c→5d（真机车道，可与 M4 并行；5c 的锁审计要等 M2/M3 内核改动落定）
M6: 6a（M2 就可先做计数）→ 6b→6c
```

**不要并行的事**：同一时间两个会话改 `build-ng.ps1`；同一时间改
`syscall.[ch]` 的号段；在真机 SMP 锁审计窗口内合入新的阻塞/调度路径。

## 10. 与 `d1-plan.md` 的对应

| 里程碑 | WP | 出口主线 |
|---|---|---|
| M1 | 0a–0f + S1–S4 | ABI 1.1、fd、fault、FPU、tick |
| M2 | 1a–1d、2a–2c | zlib、TCP/socket、DHCP/DNS |
| M3 | 1e–1g、3a–3c | FreeType/FFmpeg/SDL2、HTTPS |
| M4 | 4a–4d、2d | B0 本地/真实页面、滚动 |
| M5 | 5a–5d | 真机 UEFI/ACPI/SMP/网络/显示 |
| M6 | 6a–6c | 8h 真机 + 24h QEMU、D1 发布 |

细节取舍、风险与回退见 `risks.md`；syscall/errno 映射见 `posix-profile.md`。
