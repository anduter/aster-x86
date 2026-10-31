# ng 代码审查 / 缺陷清单（2026-10-31）

> 状态：**审查报告（非实现）**。
> 基线：ng HEAD a48c29b；D0 tag ng-d0-final（a44e896）。
> 工作树：有 D1 WIP（ABI 1.1、syscall、abi 文档），部分结论可能随 WIP 变化。
> 方法：全仓静态扫描 + 文档 claims/ABI §10 + 高风险路径逐段读；**未跑 QEMU/fuzz**（避免与 D1/soak 抢资源）。
> 范围：src/ng、src/desktop、src/services、src/drivers、sdk；不含 legacy 32 位树。

## 0. 结论

- **功能面比隔离面成熟**：D0 的窗口/会话/SDK 能跑，但 L3 应用仍能越权碰 L0/L1/L2。
- **P0 两项**：用户态 fault 直接 panic 内核；8h 浸泡没有“在跑”的证据。
- **P1 八项**：uid=0/root 全开、spawn 无授权、role_token 可预测且被 LIST 泄露、L2 无 supervisor、无 tick/抢占、VFS 全局 ctx、无时间基、无 FPU。
- **P2 四项**：串口日志洪泛、资源配额、轮询/单线程、文档/构建漂移。
- **修复顺序**：0c 用户 fault → 8h 证据 → 0b uid/fd + 0d cap/令牌/spawn → 0f 时间基 + 0g 抢占 → VFS/FPU/P2。
- 一句话：**先让坏应用杀不死系统，再谈 POSIX 和浏览器。**

## 1. 方法与限制

**做了**

- 全仓扫描：panic/PANIC、TODO/FIXME、strcpy/sprintf/gets、kmalloc 失败路径、无界循环、cli/sti。
- 读 claims：ng-audit-syscall、ng-audit-ui、ng-audit-wip、ng-WP4-*；读 ABI 1.0 §10。
- 逐段读高风险路径：trap、sched、proc、ipc、shm、vfs、sessiond、displayd、wm。
- 交叉核对：D1 计划/wps/risks、status、README。

**没做**

- 动态 fuzz/渗透、QEMU 长跑、真实硬件、形式化验证、SMP/RT。
- 逐行穷举 4 万行；结论是“静态审查 + 已知证据”，不是“全部缺陷已找到”。

**等级**

- P0：可导致整机不可用，或让 D0/D1 验收不成立。
- P1：安全/隔离/可用性地基，必须在 D1 前处理。
- P2：健壮性/工程债，可在 M1–M2 逐步处理。

## 2. P0

### P0-1 用户态 #PF/#GP 直接 panic 内核

- 证据：src/ng/trap.c:184-217。trap_dispatch() 只认 probe_active 的受控探测；其它 #PF/#UD/#GP 一律 panic()（trap.c:216）。
- 影响：任何 L3 应用执行 *(volatile int *)0 = 1 就能让整机停住；D1 的 POSIX/浏览器应用会频繁触发。
- 复现：tools/audit-ng-sys.ps1 -Scenario crash 目前**期望** PANIC vec=0xe；这正说明缺陷存在。
- 修复：WP5-0c——按 trap frame 的 cs&3 判断来源；ring3 fault 终止进程（后续接 SIGSEGV/SIGBUS），ring0 才 panic；串口打印限流。
- 出口：user fault 后 kernel 继续跑、进程退出、shell/desktop 不受影响；audit-ng-sys 的 crash 场景改为“no PANIC”。

### P0-2 8h 浸泡没有“在跑”的证据

- 证据：obj-ng/soak-final/ 只有 dry/pre/probe 目录（dry.log 是 shell transcript）；21:0x 无 qemu 进程；README/d1-plan 写“8h 在跑”。
- 影响：D0 的最终长跑门禁无法验收；若在别的机器跑，仓库里没有日志。
- 修复：在 ng-d0-final 上重跑，日志落 obj-ng/soak-final/；完成后在 claims 记录起止时间、panic/OOM/泄漏/空闲 CPU。
- 出口：soak: 8h pass (panic=0 oom=0 leak=.. idle_cpu=..)；或明确记录失败并修复后重跑。

## 3. P1：安全 / 隔离

### P1-1 所有进程 uid=0，root 权限全开

| 子项 | 证据 | 影响 |
|---|---|---|
| kill 任意进程 | src/ng/proc.c:793-810：只在 p->uid != cur->uid 且 cur->uid != 0 时拒绝 | 任何应用可杀 displayd/sessiond/wm |
| 映射 framebuffer | src/ng/fbmap.c:51-54：只允许 uid 0 | 任何应用可读写整屏、伪造 UI |
| 共享内存越权 | src/ng/shm_core.c:121-133：uid == 0 绕过 owner 校验 | 任何应用可 map 别人的 shm |
| 端口 ACL 失效 | src/ng/ipc_core.c:241-245：内核路径一律 root | 端口权限形同虚设 |

- 修复：WP5-0b（per-process uid/fd/cwd）+ WP5-0d（cap 位/peer 身份）；新应用默认非 root，L2 服务持有专用 cap。
- 出口：应用 A kill B / FB_MAP / SHM_MAP 全部 EPERM；L2 服务仍能正常工作。

### P1-2 spawn 没有权限/角色校验

- 证据：src/ng/proc.c:912-945。proc_sys_spawn 只校验指针/长度/argv，不检查调用者身份或目标路径。
- 影响：任意应用可 spawn /bin/displayd、/bin/sessiond、/bin/wm，起“假桌面”；结合 P1-3 可抢会话。
- 修复：加 cap/role 门禁；L2 服务只允许 sessiond 拉起；L3 只能 spawn 允许的应用白名单/经 sessiond LAUNCH。
- 出口：应用直接 spawn /bin/displayd 返回 EPERM；sessiond LAUNCH 正常。

### P1-3 role_token 可预测，而且会被 LIST 泄露

- 证据：
  - src/desktop/src/session_table.c:108-113：next_seq++ / next_nonce++，令牌只由递增计数组成。
  - src/desktop/include/session_protocol.h:58-60：token = (nonce<<16) | (role<<8) | seq，无 MAC/随机。
  - src/desktop/display/displayd.c:737：LIST_REPLY 把每个窗口的 token 回给**任意客户端**（d_handle_list 无角色检查）。
  - src/desktop/session/sessiond.c:656 注释也承认“令牌可能从 displayd 的 LIST 里被读到”。
- 影响：应用可读 token、预测下一个，伪造 FOCUS/DESTROY/KILL/LAUNCH；结合 P1-2 可接管桌面。
- 修复：WP5-0d——CSPRNG/HMAC 令牌，绑定 pid+port，LIST 不返回 token；长期用 cap 表替代裸 token。
- 出口：伪造/重放/预测 token 全部拒绝；LIST 输出脱敏；unit 覆盖。

### P1-4 L2 没有 supervisor

- 证据：src/ng/main.c:398-400 只 spawn sessiond；src/desktop/session/sessiond.c:5-13 只重启 WM，**displayd 死了不重启，sessiond 死了没人接管**。
- 影响：displayd 崩溃后窗口/输入全失；sessiond 崩溃后无法再启动应用；整栈崩只能重启系统。
- 修复：S2/L2 supervisor——displayd → sessiond → WM 逐级监督/重启；应用重连协议。
- 出口：kill displayd/sessiond 后，N 秒内恢复或明确进入 KNOWN/降级；内核不 panic。

### P1-5 没有 tick / 抢占

- 证据：src/ng/sched.c:16 明确“没有 IRQ tick、没有抢占”；sched_tick()（sched.c:416）没有 IRQ 调用者；thread.h:219 标注“给未来的 tick IRQ 用”。
- 影响：WM/应用自旋会饿死全系统；输入/合成延迟无上界。
- 修复：WP5-0f（PIT 1000Hz + TSC/RTC/clock）+ WP5-0g（IRQ0 置 need_resched，返回 ring3 时切时间片；内核仍协作）。
- 出口：WM 自旋时 shell 仍可跑、可 kill WM；1h drift ≤100 ppm；tick 开销 ≤1% CPU。

## 4. P1：功能地基

### P1-6 VFS fd/cwd 是全局一份

- 证据：src/ng/fs_syscall.c:30 static struct vfs_ctx g_ctx；src/services/fs/include/vfs.h:25 注释“per-AS；本 WP 只有单 AS，内核全局一份”。
- 影响：所有进程共享 fd/cwd；A 的 close 会关 B 的 fd，A 的 chdir 影响 B；POSIX 层无法正确实现。
- 修复：WP5-0b——fd/cwd 搬进 proc，文件对象引入 struct file。
- 出口：A 打开的 fd 在 B 返回 EBADF；A 退出 fd 计数回落；chdir 只影响自己。

### P1-7 时间基仍是裸 TSC

- 证据：src/ng/sched_arch.c:17-25 直接 rdtsc，注释“不要求频率换算，只做相对超时”；无 PIT/RTC/clock_gettime。
- 影响：无法给应用提供 ms/ns/墙钟；POSIX clock_gettime/nanosleep、TLS 证书时间、音频/帧节奏都缺。
- 修复：WP5-0f——PIT 1000Hz、TSC 校准、CMOS RTC、clock_gettime/nanosleep、HLT idle。
- 出口：clock/nanosleep/RTC 证据；1h drift ≤100 ppm。

### P1-8 没有 FPU/SIMD 上下文

- 证据：用户库按 -mno-sse 走；内核 -mgeneral-regs-only；没有 FXSAVE/FXRSTOR per-thread。
- 影响：POSIX/FFmpeg/SDL2/图形库一旦用 SSE 就 #UD 或串值；数值精度受限。
- 修复：WP5-0e——用户线程切换时 FXSAVE/FXRSTOR，TCB 尾静态区；用户栈 16B 对齐。
- 出口：多线程浮点回归；#UD 消失；内核不被 SSE 污染。

## 5. P2：健壮性 / 工程

| 项 | 证据 | 建议 |
|---|---|---|
| 用户 #PF 串口洪泛 | src/ng/trap.c:186-193 每次 #PF 打印 cr2/error/rip | fault 隔离后加限流/采样，避免日志 DoS |
| 资源配额缺失 | PROC_MAX=32、PROC_ELF_MAX=256KB、ramfs 池 64/128；无 per-user 配额 | 加 admission/配额/回收；压测 fork/spawn/文件 |
| sessiond 20ms 轮询 / displayd 单线程 | sessiond.c S_LIST_EVERY=10；displayd 单线程事件循环 | 事件驱动、批量、背压；并发客户端压测 |
| 文档/构建漂移 | obj-ng/ 未跟踪；D1 计划仍写 31 个 WP（0g 并入后 32）；WP5-0g 文档未提交 | 补 .gitignore、更新计数、合并 0g |

## 6. 已修复 / 已验证（避免重复）

- audit-sys：O_CREAT 失败回滚、O_TRUNC/fd 表满不截断、ramfs 节点 64/128、spawn 路径/argv ENAMETOOLONG、ELF 畸形输入（554 cases）。
- audit-ui：DESTROY 归属校验、WM 令牌回收、title 净化、状态行 ≤500、REGISTER_HOTKEY ack 去除、只认 v3 LIST_REPLY。
- 静态扫描未发现 strcpy/strcat/sprintf/gets；IPC/spawn 的 kmalloc 失败路径有检查；未发现无界循环（事件循环除外）。
- 这些降低了风险，但**没有解决 uid=0、token 可预测、spawn 无授权**。

## 7. 修复顺序与门禁

| 顺序 | 项 | WP | 出口 |
|---|---|---|---|
| 1 | 用户 fault 隔离 | WP5-0c | fault 杀进程，内核不 panic |
| 2 | 8h 浸泡证据 | WP5-6a（D0 基线） | 8h 日志 + 指标 |
| 3 | per-process uid/fd/cwd | WP5-0b | A 不能杀/读/写 B |
| 4 | cap/token/spawn 授权 | WP5-0d | 伪造/越权全部拒绝 |
| 5 | 时间基 | WP5-0f | clock/nanosleep/RTC/drift |
| 6 | ring3 抢占 | WP5-0g | WM 自旋不卡死 |
| 7 | FPU/SIMD | WP5-0e | 浮点回归 |
| 8 | P2 工程项 | 各 WP | 配额/限流/文档 |

**门禁**：每项必须 ci-ng -Quick + 对应 QEMU harness；claims 写复跑命令；D0 final 的 8h 不能拿中间镜像充数。

## 8. 复现 / 验证命令

    powershell -File tools/unit-ng.ps1
    powershell -File tools/build-ng.ps1
    powershell -File tools/run-ng.ps1
    powershell -File tools/audit-ng-sys.ps1
    powershell -File tools/audit-ng-ui.ps1
    powershell -File tools/s1-fault.ps1
    # 8h 浸泡：按 WP5-6a 的 harness，在最终 HEAD 上跑

## 9. 残余风险

- 本报告是静态审查；动态 fuzz/渗透、真机、SMP、RT 未覆盖。
- 8h/24h 长跑、真实硬件、POSIX/浏览器生态仍是 D1 的未知数。
- RT（高精度硬件开发）是独立产品线，不混进 D1；见 decisions.md 与 D1 risks。
- legacy 32 位树冻结；不在本报告范围。

## 10. 变更记录

- 2026-10-31：初版（P0×2、P1×8、P2×4），基线 a48c29b。
