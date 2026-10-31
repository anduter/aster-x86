# D1 计划

> 当前阶段：见 [CURRENT.md](../../CURRENT.md)（唯一状态源）。
> 目标：可用桌面（POSIX / 网络 / 浏览器 B0 / 真机 / 浸泡）。

> 状态：**规划（PLANNING）**，2026-10-31。D0 已关闭：tag `ng-d0-final`，HEAD `a44e896`。
> 上位：`../architecture.md` §10（D1）、`../evolution.md` E5/D1、`../decisions.md`
> ADR-004/005/007/008/009/011。
> 输入：`../../abi/abi-1.0.md`、`ng-WP4-*`、`ng-audit-*`、`../domains/**`、
> 旧树 `../../network.md` / `../../e1000.md`（行为 oracle）。
> 范围：**本文件只做设计**，不改代码/工具/SDK/构建，不跑 QEMU。
> 8h 浸泡：D0 基线的 8h 长跑由另一会话在最终 HEAD 上执行中（`obj-ng/soak-final/`，
> 不进 git）；本阶段所有工作必须避让它。

## 0. 结论摘要

D1 不是“把 D0 的功能再堆一点”，而是把 Aster 从**能演示的开发者桌面**推到
**能日常使用的单用户桌面**。五条出口线：

1. **POSIX Profile 1.0**：用户态 libc + syscall shim；zlib / FreeType / FFmpeg /
   SDL2 能交叉编译并在 Aster 上运行（最小 demo）。
2. **网络**：e1000 + virtio-net；ARP/IPv4/ICMP/UDP/TCP；socket syscall；
   DHCP/DNS；1 MiB 级传输可用。
3. **TLS/HTTP**：用户态 crypto（不依赖 OpenSSL）；TLS 1.2 客户端 + X.509
   验证 + HTTP/1.1；能抓取真实 HTTPS 页面。
4. **浏览器 B0**：自有引擎、无 JS 的 HTML/CSS 子集；文本/图片/滚动；能渲染
   本地语料与真实 HTTPS 页面。
5. **真实硬件 + 8h+**：一款锁定机型 UEFI/ACPI/APIC/多核启动；8h 浸泡（真机）
   + 24h 浸泡（QEMU）无 panic、无单调泄漏。

**顺序一句话**：先补内核“看不见的地基”（ABI 1.1、per-process fd、fault→信号、
FPU/SIMD、tick/时间/熵），再并行做 **POSIX 底座** 与 **网络核心**；两者汇合后
做 **TLS/HTTP**，再上 **浏览器 B0**；**真机**作为独立车道从预检后并行推进，
最后用 **浸泡** 收口。详细依赖与理由见 §3/§4。

**规模**：32 个 WP（WP5-0…WP5-6 的子包），约 **50–60 个会话**（含 4 个预检）；
6 个里程碑 M1–M6。**不做日历承诺**，里程碑按出口断言判定。

**最大风险**（详见 `risks.md`）：POSIX 生态（libc + FFmpeg/SDL2）能否按预算
跑起来；真机 UEFI/ACPI/APIC/SMP 的不确定性；TLS/X.509 的安全与工期。

**第一个开工**：`WP5-0a`（ABI 1.1 号段/错误码 + `ENOSYS`），同时用独立会话跑
预检 `S1`（newlib 交叉构建）。所有后续 WP 都依赖它冻结的编号与语义。

## 1. D1 的起点（D0 基线）

| 面 | D0 已有（证据） | D1 缺口 |
|---|---|---|
| 内核 | 64 位 long mode、4 级页表、W^X/SMEP/SMAP、PMM/heap、协作式调度、IPC/cap 雏形、ELF64、进程/spawn/wait/kill（`src/ng/**`） | 未分配号返回 `-EPERM` 而非 `ENOSYS`；fd 表全局一份；ring3 #PF/#GP → panic；无 `SHM_FREE`；无 FPU/SIMD 上下文；无 timer IRQ/HLT idle；无 wall clock/熵 |
| ABI | native ABI **1.0 冻结**（29 调用、28 错误码；`docs/abi/abi-1.0.md`） | POSIX/网络/线程/时间所需调用全部未分配；需要 **ABI 1.1 只追加** |
| POSIX | 无 libc、无 CRT、无 `malloc/stdio`；应用自己写 syscall 包装 | newlib（或等价）移植、crt0、syscall stubs、libm、pthread 子集、信号、`execve`、`mmap/brk` |
| 网络 | **无**（旧 32 位树有 ARP/ICMP/UDP/TCP/DHCP/DNS 雏形，可作 oracle） | 64 位化移植 + 真实 TCP 状态机 + socket 层 + 网卡（e1000/virtio-net/e1000e） |
| TLS/HTTP | 无 | 用户态 crypto、X.509、TLS 1.2、HTTP/1.1、信任库/时间/熵 |
| 桌面 | displayd / WM / sessiond / SDK（S1 故障注入已验收） | 浏览器 B0；字体/PNG；终端不是 D1 出口 |
| 真机 | 仅 QEMU（`-kernel` + PVH note；BIOS/VBE/PS2/ATA） | UEFI stub、GOP、ACPI、APIC/IOAPIC、SMP 启动、真网卡/键盘 |
| 长跑 | 1h smoke PASS（60.17min、0 panic）；8h 证据未闭合（21:04 起跑，见 CURRENT） | 带负载的 8h（真机）+ 24h（QEMU）；泄漏/CPU 空闲/故障注入 |
| 门禁 | `ci-ng` 7 步、ABI check、SDK check；host 5315 checks、run-ng 159/159 + 75 patterns | POSIX/net/TLS/browser/hw/soak 六类新门禁；ABI 1.1 check |

已知缺口（`../../abi/abi-1.0.md` §10、`../../claims/ng-audit-syscall.md` §6、
`../../claims/ng-audit-ui.md` 未修项）：ENOSYS、per-process fd、fault→panic、
`SHM_FREE`、cap/`from` 可伪造、FD/ ticket 数值歧义、kill 阻塞 syscall 的
内核缓冲泄漏、8 个 SHM 对象无 free、WM 令牌 nonce 短、状态文件未认证。
**这些不是“顺手修”，是 D1 的 WP5-0 范围**：POSIX/网络/浏览器都会踩到它们。

## 2. D1 出口（Definition of Done）

D1 出口不是“功能列表都在”，而是下面 8 条**可复现断言**全部满足。

### D1-E1 POSIX Profile 1.0

- `libc/` 产出静态 `libc.a` + `libm.a` + `libpthread.a`，头文件可用；
- zlib：inflate/deflate 测试向量 CRC 一致；
- FreeType：加载 TTF，把一行文字渲染成 ARGB 并显示（或存 PNG 校验）；
- FFmpeg：静态 `libavcodec/libavutil/libswscale`，解码一段 H.264/PNG，
  输出一帧 RGB；
- SDL2：自定义 `aster` video driver，创建窗口、收键盘事件、画一帧；
- POSIX 文件/进程/线程/信号/时间 API 的 `posix-check` 全绿。

### D1-E2 网络

- e1000（QEMU 82540EM）与 virtio-net 至少各一条路径可收发；
- DHCP 拿到地址、DNS 解析 A 记录、ICMP echo、UDP echo；
- TCP：connect/accept/send/recv/close，1 MiB 传输 SHA-256 一致；
- 10% 本地丢包注入下仍能完成传输（重传/RTO 生效）；
- socket syscall 有 per-process 表、非阻塞、`poll`；`CAP_NET` 缺省关闭。

### D1-E3 TLS / HTTP

- 用户态 TLS 1.2 客户端，AES-GCM/ChaCha20-Poly1305 + ECDHE；
- X.509 链验证：有效期、主机名（SAN/CN）、BasicConstraints；过期/自签/
  错主机名/错签名必须拒绝；
- HTTP/1.1：GET、301/302、chunked、gzip、Content-Length、超时；
- 本地 HTTPS 服务器 + `https://example.com` 均能 200；1 MiB 下载哈希一致；
- 目标镜像**不链接 OpenSSL**（构建产物/符号检查）。

### D1-E4 浏览器 B0

- 本地 5 页语料（标题/段落/列表/链接/背景色/PNG 图片）逐页渲染，
  关键像素基线与截图一致；
- `https://` 页面：抓取→解析→布局→绘制→可滚动到页尾；
- 100 个畸形 HTML/CSS 输入不崩溃、不 panic、不泄漏；
- 与 displayd/WM 正常协作；浏览器崩溃只死自己（S1 语义）。

### D1-E5 真实硬件

- 锁定机型从 UEFI（ESP 上的 loader）启动，经 GOP 显示、PS/2 或 xHCI HID
  键盘输入；
- ACPI MADT 枚举 CPU/IOAPIC，LAPIC timer 工作；多核 **启动**（AP 停泊）；
- 真网卡 DHCP + DNS + TCP + HTTPS 页面；
- 连续 1 小时无 panic/冻结，串口有完整日志。

### D1-E6 8h+ 浸泡

- 真机 **8h**、QEMU **24h**（最低 8h）带负载长跑：
  shell 命令每 15min、HTTP 抓取每 5min、浏览器每 30min reload、周期故障注入；
- 0 panic / 0 冻结 / 0 重启；心跳与命令响应 <2s；
- 内存/句柄/fd/端口/SHM/进程计数无单调增长（阈值见 `wps.md` WP5-6a）；
- 空闲宿主 CPU <10%（修掉 1h smoke 观察到的 ~99% 单核）。

### D1-E7 门禁与可复现

- `ci-ng` 原有 7 步全绿；新增 `posix-check` / `net-check` / `tls-check` /
  `browser-check` / `hw-check` / `soak-d1` 可按需跑；
- `abi-check` 覆盖 ABI 1.0（不变）+ ABI 1.1（新增）；`sdk-check` 仍绿；
- 每个 WP 有 `docs/claims/ng-WP5-*.md`，证据是串口行/测试输出/截图/哈希。

### D1-E8 文档与发布

- `docs/release/d1.md`、兼容矩阵（QEMU/真机/CPU/设备）、已知限制；
- `docs/design/posix-profile.md` 与实现一致（编号/errno/结构体/上限）；
- D1 tag（建议 `ng-d1`）由负责人打；不 push、不签名（除非负责人要求）。

## 3. 五个方向：范围与前置依赖

### 3.1 POSIX Profile

**是什么**：用户态源码兼容层，不是内核 POSIX。应用用 `libc.a` + POSIX 头
编译成静态 ELF64；libc 把 POSIX 调用翻译成 native ABI 1.1 调用。内核只增加
**通用机制**（fd、pipe、exec、信号、线程、时间、随机数），不出现 POSIX 语义。

**分层**：

| 层 | 内容 | 归属 |
|---|---|---|
| `libc/newlib`（移植） | stdio、malloc、string、stdlib、libm、locale 桩 | A（`libc/`） |
| `libc/aster`（新写） | crt0、syscall stubs、errno/TLS、`opendir/readdir`、`system`、`posix_spawn`、pthread、signal 包装 | A |
| 内核机制 | per-process fd/cwd/uid、pipe、execve、clone/futex、信号投递、brk/mmap、clock/getrandom | L0/L1（`src/ng`、`src/services/fs`） |
| 第三方库 | zlib、libpng、FreeType、FFmpeg、SDL2 | `third_party/`（pin 版本 + 哈希） |

**前置依赖**：

- WP5-0b：per-process fd/cwd/uid——没有它，stdio/多进程一跑就串；
- WP5-0c：fault→进程终止 + 信号骨架——POSIX 进程的“异常可恢复”前提；
- WP5-0e：FPU/SIMD 上下文 + 用户态 `-msse2`——FreeType 部分路径、FFmpeg/SDL2
  的浮点与 SIMD 必须；
- WP5-0f：tick/时间/熵——`clock_gettime/nanosleep/getrandom`、stdio 超时；
- WP5-1a：文件/进程 syscall（dup/pipe/lseek/fstat/chdir/getcwd/waitpid/execve…）；
- WP5-1c/1d：内核线程 + pthread——FFmpeg/SDL2 的编译/运行前提（B0 自身不用
  多线程，见 §3.4）。

**为什么先做这个方向**：zlib/FreeType/libpng 是浏览器 B0 的字体/图片/压缩
底座；线程/信号是浏览器后台抓取与崩溃隔离的前提。**没有 POSIX，浏览器只能
写死图片和字模，B0 会退化成截图播放器。**

### 3.2 网络

**是什么**：L1 网络库（暂留内核，facade）+ L1D 网卡驱动 + native socket
syscall。协议：ARP、IPv4、ICMP、UDP、TCP、DHCP 客户端、DNS A、loopback。
**不做**：IPv6、IP 分片重组、Wi-Fi、NAT/路由、防火墙、netd 进程（D2）。

**前置依赖**：

- WP5-0b/0d：per-process fd 类型扩展（FILE/SOCKET/PIPE）、`CAP_NET` 门禁；
- WP5-0f：1 ms tick + 单调时间（TCP RTO/重传/超时）；HLT idle 防止空转；
- WP5-0e：不需要，但网卡 DMA/中断在真机车道会碰；
- 旧树 `network.md`/`e1000.md` 作为 oracle：ARP/UDP/TCP 的边界用例先搬进
  host 单测，再改 64 位。

**为什么与 POSIX 并行、排在 TLS 之前**：网络本身不依赖 libc（可以先写内核
自检），但**TLS/HTTP/浏览器都依赖 socket**；它是 D1 最长的关键路径之一，
必须在 M2 就开始，不能等 POSIX 全绿。两条车道并行：POSIX 车道做 libc，
网络车道做协议/驱动，汇合点是 WP5-1a 的 fd 类型与 WP5-2b 的 socket。

### 3.3 TLS / HTTP

**是什么**：用户态 `libtls` + `libhttp`（A 域），静态链接进浏览器/工具；
crypto 不依赖 OpenSSL。**推荐**：移植 **BearSSL**（TLS 1.2，MIT，无 malloc，
自带 X.509/AES-GCM/ChaCha20/RSA/ECDSA），自己写 HTTP/1.1；TLS 1.3 留 D2。
备选：mbedTLS（TLS 1.2/1.3，体积更大）。

**前置依赖**：

- WP5-2：TCP socket、非阻塞、`poll`；
- WP5-0f：`getrandom`（RDRAND/RDSEED + 抖动）与真实时间（RTC）；
- WP5-1b：libc（malloc/stdio/string）——BearSSL 可以 no-malloc，但 HTTP/证书
  解析需要；
- 信任库：Mozilla CA bundle 的子集（PEM→DER），随镜像只读提供；
- 测试：宿主 OpenSSL/Python `ssl` 只用于**测试服务器**，不进目标镜像。

**为什么在浏览器之前**：B0 的“真实页面”必须 HTTPS；没有 TLS 只能做本地
file:// 演示，不能算 D1。TLS 也是 HTTP 的依赖，先 TLS 后 HTTP 再浏览器。

### 3.4 浏览器 B0

**是什么**：L3 独立进程 `/bin/browser`，自有引擎，无 JS：
HTML 子集（tokenizer + 树 + 常见标签）→ CSS 子集（tag/class/id/后代选择器，
颜色/字体/盒模型/display）→ 块/行内布局 → FreeType 文本 + PNG 图片 →
ARGB surface → displayd。键盘滚动；无鼠标/表单/JS/音视频/多标签。

**前置依赖**：

- 图形：displayd + SHM（已有，D0）；
- 字体/图片/压缩：WP5-1f（FreeType/libpng/zlib）；
- 网络/TLS/HTTP：WP5-2 + WP5-3；
- 线程/信号：**B0 自身用单线程非阻塞事件循环**（socket + readkey + poll），
  不依赖 pthread 的正确性；pthread 是给 FFmpeg/SDL2 和未来用的。信号用于
  崩溃隔离（SIGSEGV 默认终止）与 `SIGPIPE`，不是 B0 的并发模型。
- 但“线程/信号”仍是 B0 的前置**能力**：它依赖的 FreeType/libpng 需要在有
  pthread 的 POSIX 环境里编译；浏览器进程也必须能被信号/fault 隔离。**B0
  不把“多线程正确性”当作自己的验收条件**——这是刻意的范围控制。
- 这避免了“浏览器卡在 TLS 握手时 UI 冻结”：所有网络都是非阻塞状态机。

**为什么最后**：B0 把前面所有东西串起来，任何一层不稳都会在页面上放大；
先做会把调试成本乘几倍。它的**解析/布局/绘制**可以提前在宿主用测试语料
开发（M1 预检 S4），但集成必须等 TLS/HTTP 稳定。

### 3.5 真实硬件

**是什么**：一款锁定 x86-64 UEFI 机型；UEFI loader → GOP 显示 → ACPI →
LAPIC/IOAPIC → SMP 启动（AP 停泊）→ 真网卡/键盘。**不做**安装器/更新/
回滚（architecture §10 的该项建议拆到 D1.1，需负责人确认）；不做 GPU 驱动
（用 GOP 线性帧缓冲）、不做 USB 存储/音频/Wi-Fi。

**前置依赖**：

- WP5-0f：tick/时间基础（QEMU PIC/PIT 先跑通，APIC 后替换）；
- WP5-5a：UEFI stub + GOP + RSDP + memmap（先在 OVMF/QEMU 验证）；
- WP5-5b：ACPI MADT/FADT/MCFG/HPET + LAPIC/IOAPIC；
- WP5-5c：SMP 启动（INIT/SIPI、per-CPU、AP 停泊）；
- 驱动：e1000e/82574、PS/2（或 xHCI HID）、GOP；AHCI 可选只读。

**为什么可以和浏览器并行**：真机车道主要碰 `src/boot`、`src/ng/{acpi,apic,smp}`、
`src/drivers`，与 `libc/`、`src/apps/browser/` 路径不冲突；越早开始越早暴露
固件/ACPI 的坑。**但 SMP 锁审计会碰内核公共路径**，必须在 M2/M3 的网络/线程
合入之后做一次“锁审计”窗口，不能边写 TCP 边改调度器。

### 3.6 8h+ 浸泡与硬化

**是什么**：带负载的长跑 + 泄漏计数 + 故障注入 + 空闲功耗修复。**不是**
“挂机 8 小时看没崩”，而是有工作负载、有指标、有故障注入的验收。

**前置依赖**：

- 所有功能 WP 的计数点（PMM/fd/handle/port/shm/process/thread）；
- WP5-6a 的 harness 与 in-guest `/run/stats`；
- 真机（M5）才能做“真机 8h”；QEMU 24h 随时可跑。

**为什么最后收口、但要提前埋点**：harness 和计数器在 M2 就落地（否则后期
补计数会改一堆公共结构）；最终 8h 必须用 D1 最终镜像，不能拿中间版本充数。

### 3.7 依赖图

```text
                         ┌────────────────────────────────────────────┐
                         │ WP5-0 内核地基                              │
                         │ 0a ABI1.1/ENOSYS  0b per-proc fd/cwd/uid    │
                         │ 0c fault→信号     0d SHM_FREE/peer/cap      │
                         │ 0e FPU/SIMD       0f tick/时间/熵           │
                         └───────────────┬────────────────────────────┘
             ┌───────────────────────────┼───────────────────────────┐
             ▼                           ▼                           ▼
   ┌──────────────────┐        ┌──────────────────┐        ┌──────────────────┐
   │ WP5-1 POSIX      │        │ WP5-2 网络       │        │ WP5-5 真机       │
   │ 1a 文件/进程     │        │ 2a NIC/ARP/IP/   │        │ 5a UEFI/GOP      │
   │ 1b newlib        │        │    UDP/DHCP/DNS  │        │ 5b ACPI/APIC     │
   │ 1c 内核线程      │        │ 2b TCP/socket    │        │ 5c SMP 启动      │
   │ 1d pthread       │        │ 2c 配置/cap/     │        │ 5d 真机驱动      │
   │ 1e 信号          │        │    loopback      │        │                  │
   │ 1f FreeType/PNG  │        │ 2d 硬化/性能     │        │                  │
   │ 1g FFmpeg/SDL2   │        └────────┬─────────┘        └──────────────────┘
   └────────┬─────────┘                 │
            │                           ▼
            │                  ┌──────────────────┐
            │                  │ WP5-3 TLS/HTTP   │
            │                  │ 3a BearSSL       │
            │                  │ 3b X.509/信任库  │
            │                  │ 3c HTTP/1.1      │
            │                  └────────┬─────────┘
            └───────────────┬───────────┘
                            ▼
                  ┌──────────────────┐
                  │ WP5-4 浏览器 B0  │
                  │ 4a HTML/CSS      │
                  │ 4b 布局/文本     │
                  │ 4c 绘制/滚动/抓取│
                  │ 4d 语料/健壮性   │
                  └────────┬─────────┘
                           ▼
                  ┌──────────────────┐
                  │ WP5-6 浸泡/收口  │
                  │ 6a harness/计数  │
                  │ 6b idle/故障注入 │
                  │ 6c 8h+ 与 D1 发布│
                  └──────────────────┘
```

> 图中 WP5-5 只画到 WP5-0 的启动依赖；实际 `5d` 的验收还依赖 WP5-2（网络）、
> WP5-4（浏览器）与 WP5-2d（稳定性）。WP5-6a 的计数/harness 在 M2 就应落地，
> 不要等到 M6 才补。

## 4. 为什么是这个顺序

1. **地基先行**：per-process fd、fault→信号、FPU、tick、熵/时间这五件事，
   任何一件晚做都会逼着 POSIX/网络/浏览器写临时绕路，最后返工。WP5-0 只有
   6 个 WP，但它是所有后续路径的公共依赖，必须先冻结 ABI 1.1 号段。
2. **两条长车道并行**：POSIX（1a→1g）与网络（2a→2d）互不阻塞；汇合点是
   socket 需要 fd 类型（1a）和线程/超时（0f/1c）。**不要串行做完 POSIX 再
   做网络**，否则关键路径翻倍。
3. **TLS 跟着网络走、HTTP 跟着 TLS 走**：TLS 的输入是 TCP 流，输出是 HTTP
   的字节流；三层分开验收，避免“浏览器跑不起来时不知道是 TCP、TLS 还是
   HTML”的问题。
4. **浏览器最后集成**：B0 的解析/布局可以在宿主先写（S4），但页面加载必须
   等 HTTP/TLS 稳定；B0 的每个 bug 都会被误判成网络 bug，集成要放在后面。
5. **真机车道提前、但不抢跑 SMP**：UEFI/GOP/ACPI 尽早验证（S3），但 SMP
   锁审计要等 M2/M3 的公共内核改动落定；AP 只停泊，不做多核调度，避免在
   D1 引入全局锁重写。
6. **浸泡埋点早、验收晚**：计数器/harness 在 M2 就位，之后每个里程碑跑
   1h；最终 8h 用最终镜像，不能拿开发中间态凑数。
7. **一个 WP 一个会话、路径限定**：D1 的并行度比 D0 高，靠路径所有权 +
   `build-ng.ps1` 单写者规则避免互相踩。

## 5. 里程碑 M1–M6

> 里程碑是**能力门**，不是日历。M5（真机）与 M4（浏览器）可部分并行。
> “预计 WP 数”统计 `wps.md` 里的 WP 子包；“会话”是 1 WP ≈ 1–2 会话的估算。

| 里程碑 | 主题 | 包含 WP | 预计 WP | 预计会话 | 出口主线 |
|---|---|---|---|---|---|
| **M1** | 内核地基 + 预检 | WP5-0a…0g + S1–S4 | 7 | 14 | ABI 1.1 冻结；fd/fault/FPU/tick 可用 |
| **M2** | POSIX 底座 + 网络核心 | WP5-1a…1d；WP5-2a…2c | 7 | 12 | zlib 跑通；TCP/socket/DHCP/DNS 可用 |
| **M3** | POSIX 库 + TLS/HTTP | WP5-1e…1g；WP5-3a…3c | 6 | 9 | FreeType/FFmpeg/SDL2；HTTPS 抓取 |
| **M4** | 浏览器 B0 | WP5-4a…4d；WP5-2d | 5 | 8 | B0 本地语料 + 真实页面 + 滚动 |
| **M5** | 真实硬件 | WP5-5a…5d | 4 | 8 | 目标机 UEFI/ACPI/SMP/网络/显示 |
| **M6** | 浸泡与 D1 收口 | WP5-6a…6c | 3 | 5 | 8h 真机 + 24h QEMU；D1 发布 |

### M1：内核地基（WP5-0a…0g + 4 个预检）

**目标**：把 D1 所有方向共用的内核缺口一次性收掉；用 4 个短预检把最大的
未知数提前打掉。

**预检（每个 ≤1 会话，结果进 `docs/claims/ng-WP5-0a.md` 的“预检”附录）**：

| ID | 预检 | 通过判据 | 失败的回退 |
|---|---|---|---|
| S1 | newlib 4.x 交叉构建（`x86_64-elf`）+ hello | 生成 `libc.a/libm.a`，hello 链接并打印 | 改 picolibc；再不行用 in-tree 最小 libc，缩 POSIX 目标 |
| S2 | BearSSL 宿主构建 + 本地 TLS 握手/证书验证 | `br_ssl_client` 握手成功、坏证书拒绝 | 改 mbedTLS；TLS 1.3 明确留 D2 |
| S3 | OVMF/QEMU 上 UEFI stub → GOP/RSDP/memmap | kernel 在 OVMF 下启动、打印 GOP/ACPI 地址 | 改用 Limine/GRUB 作为过渡 loader（不改变 D1 出口） |
| S4 | 浏览器 HTML/CSS 子集宿主原型 | 5 页语料 DOM/布局/截图 | 缩 B0 标签/属性集；不做表格/浮动 |

**WP5-0 出口断言**（必须全部出现或被单测覆盖）：

- `ng: syscall ENOSYS ok (-38)`；ABI 1.1 号段/错误码表冻结，`abi-check` 覆盖；
- `ng: fd per-process ok (a=.. b=..)`：A 的 fd 在 B 里 `EBADF`；进程退出关闭
  自己的 fd；`dup` 共享偏移（若 WP5-1a 尚未合入，则仅验证 per-proc 隔离）；
- `ng: user fault killed pid=.. sig=11`：ring3 NULL 解引用 → 杀进程，**内核
  继续跑**；`audit-ng-sys -Scenario crash` 从“预期 PANIC”改为“预期 kill + 存活”；
- `ng: shm free ok (objects=0)`：1000 次 alloc/free 对象/页不增长；
- `ng: peer from ok`：伪造 `from` 被拒/被内核覆盖；`fb_map`/`socket` 的 cap
  门禁有正反用例；
- `ng: fpu ok (sse=..)`：用户态 SSE 浮点计算 + 多线程上下文切换值不串；
- `ng: timer irq ok (hz=1000)`、`ng: idle hlt ok (cpu<5%)`、`ng: clock ok
  (realtime=.. mono=..)`、`ng: random ok`；
- `unit-ng` / `build-ng` / `run-ng` / `audit-ng-sys` / `audit-ng-ui` 全绿；
  旧 32 位 305/305 不回归。

### M2：POSIX 底座 + 网络核心（WP5-1a…1d；WP5-2a…2c）

**目标**：第一个真正的 C 程序（zlib）在 Aster 上跑；第一条 TCP 流能传 1 MiB。

**出口断言**：

- `posix: newlib hello`、`posix: zlib ok (crc=..)`；
- `posix: stdio ok`（fopen/fread/printf/fseek）、`posix: malloc ok`（brk 扩堆）；
- `posix: pthread stress ok (threads=8 iters=..)`；mutex/cond/join 无死锁；
- `net: e1000 up`、`net: dhcp ok ip=..`、`net: dns ok ..`、`net: icmp ok`、
  `net: udp echo ok`；
- `net: tcp connect ok`、`net: tcp 1MiB ok (sha=..)`、`net: tcp loss 10% ok`；
- `sock: nonblock/poll ok`；`CAP_NET` 进程有/无的正反用例；
- `unit-ng` 新增 net/posix 用例；`run-ng` 原有 159/159 不回归。

### M3：POSIX 库 + TLS/HTTP（WP5-1e…1g；WP5-3a…3c）

**目标**：四个目标库都能跑最小 demo；命令行工具能通过 HTTPS 抓页面。

**出口断言**：

- `posix: freetype ok (glyphs=..)`、`posix: libpng ok (w=.. h=..)`；
- `posix: ffmpeg frame ok (codec=h264 w=.. h=..)`；
- `posix: sdl2 window ok` + 键盘事件 + 一帧像素；
- `posix: signal ok`（SIGUSR1 handler、SIGSEGV kill、SIGCHLD、SIGPIPE、EINTR）；
- `tls: handshake ok (ver=1.2 cipher=..)`、`tls: cert reject ok (expired/self/
  hostname)`；
- `http: get ok (status=200 bytes=..)`、`http: redirect/chunked/gzip ok`；
- `http: https example.com ok` + 1 MiB 下载哈希；
- 目标二进制 `nm`/构建检查无 OpenSSL 符号。

### M4：浏览器 B0（WP5-4a…4d；WP5-2d）

**目标**：浏览器能打开本地语料和真实 HTTPS 页面，滚动到页尾，畸形输入不崩。

**出口断言**：

- `browser: local corpus 5/5 pass`（每页关键像素/文本位置基线）；
- `browser: png ok`、`browser: scroll ok (y=0..max)`；
- `browser: https page ok (url=.. status=200)`；
- `browser: malformed 100/100 no crash`；
- `browser: survives kill`（杀浏览器不影响 WM/displayd）；
- `net: 10MiB ok`（WP5-2d 性能/丢包硬化）；1h 网络负载无 panic。

### M5：真实硬件（WP5-5a…5d）

**目标**：锁定的目标机从 UEFI 启动，显示/键盘/网络可用，浏览器打开 HTTPS，
连续 1 小时。

**出口断言**：

- `hw: uefi boot ok (gop=..x.. rsdp=..)`；
- `hw: acpi ok (rev=.. cpus=N)`、`hw: apic ok (lapic=.. ioapic=..)`；
- `hw: smp boot ok (aps=N all parked)`；
- `hw: nic up (mac=.. link=up)`、`hw: keyboard ok (ps2|xhci)`；
- `hw: browser https ok`、`hw: 1h stable (panic=0 reset=0)`；
- 真机日志/截图/硬件清单进 `docs/design/d1/hardware.md`（新增）。

### M6：浸泡与 D1 收口（WP5-6a…6c）

**目标**：8h 真机 + 24h QEMU（最低 8h）带负载无 panic/泄漏；D1 发布。

**出口断言**：

- `soak: 8h pass (panic=0 oom=0 leak=.. idle_cpu=..)`（真机）；
- `soak: 24h pass ...`（QEMU；最低 8h）；
- 每 15min 命令 32/32 响应；每 5min HTTP 抓取成功；浏览器 reload 无崩溃；
- 故障注入：kill displayd/WM/sessiond/browser、NIC down/up、TCP RST、
  坏 TLS、OOM、坏 ELF，按 architecture §3 收敛；
- `docs/release/d1.md` + 兼容矩阵 + 已知限制；D1 出口清单逐项勾选。

## 6. D1 明确不做（范围围栏）

> 范围膨胀是 D1 最大的敌人。以下每一项都**不阻塞** D1 出口；要做得先改
> `architecture.md`/`decisions.md` 并得到负责人确认。

### 6.1 进程/ABI

- **`fork()`/COW**：D1 只做 `execve` + `posix_spawn`；`fork` 返回 `ENOSYS`
  （libc 的 `system()` 走 `posix_spawn`）。COW/页引用计数留 D2。
- **动态链接 / `dlopen` / PIE / ASLR**：D1 仍静态 ELF64。
- **Linux/Windows 二进制兼容**、Linux personality、`int 0x80`。
- **完整 POSIX**：作业控制、会话/进程组、`setuid/setgid`、`chroot`、
  文件锁、`inotify`、`epoll`、`aio`、`io_uring`、POSIX 消息队列/信号量文件系统。
- **多用户/沙箱**：D1 所有进程仍 uid=0；cap 位只做“最小化/门禁”，不是安全
  边界。真正的每应用命名空间/picker/broker 留 D2。

### 6.2 调度/内核

- **抢占式时间片**：内核保持 IF=0/协作式；**ring3 抢占由 WP5-0g 提供**（tick 置 need_resched，返回用户态时切时间片）；不做内核抢占。内核态长临界区仍可能推迟调度。
- **多核调度 / 负载均衡 / CPU 热插拔**：AP 启动后停泊，调度仍在 BSP。
- **SMP 安全的全内核锁重写**：只做到“AP 不破坏 BSP”的启动与停泊。
- **watchdog 自动重启**：D1 做到心跳/日志/浸泡检测；panic 后自动重启留 D2。
- **fsd/netd/logd 进程化**：VFS/网络仍以内核库 + facade 存在。

### 6.3 网络/TLS

- IPv6、IP 分片重组、SACK、窗口缩放（除非 1 MiB 传输证明必须）、
  TCP 服务端高并发、拥塞控制完整实现、netd 进程、Wi-Fi、PPP、NAT/路由、
  防火墙、网络命名空间。
- **TLS 1.3**：D1 用 BearSSL 的 TLS 1.2；TLS 1.3 留 D2（若真实站点矩阵
  证明必须，再评估，不临时改）。
- HTTP/2、HTTP/3、WebSocket、代理、认证、Cookie 罐、HSTS、OCSP/CRL。
- OpenSSL（目标镜像禁止链接）。

### 6.4 浏览器

- JavaScript（任何形式）、WebAssembly、JIT。
- CSS：float、position（absolute/fixed）、flex、grid、transform、animation、
  媒体查询（可做最小 viewport）、自定义字体 `@font-face`。
- 表单、Cookie、localStorage、下载、打印、多标签、历史/书签、开发者工具。
- 音视频、Canvas、WebGL、WebRTC。
- 精确复刻真实网站；B0 只保证**子集渲染不崩、可读、可滚动**。

### 6.5 真机/桌面

- **安装器/更新/回滚/签名通道**：architecture §10 的 D1 条目，但本计划建议
  拆到 **D1.1**（需负责人确认）；D1 只做“从 ESP 启动 + 只读镜像”。
- 第二款硬件、GPU 3D、多显示器、HDR、vsync、HiDPI、音频、Wi-Fi、蓝牙、
  USB 存储/打印、电源管理/休眠。
- 鼠标/触摸/输入法（B0 用键盘滚动；PS/2 鼠标可作为可选加分项，不阻塞）。
- 终端/编辑器/文件管理器 GUI：D0 已有串口 shell；D1 的 POSIX demo 输出到
  串口/文件，GUI 终端留 D2（若负责人要求，另立 WP，不塞进 B0）。

## 7. 需要负责人确认的决策（ADR-D1 草案）

> 这些是 D1 的“先定后做”项。实现 WP 启动时逐条落进 `../decisions.md`
> （ADR-021 起），本文件只是提案。

| ID | 决策 | 推荐 | 替代/影响 |
|---|---|---|---|
| D1-A | libc 底座 | **移植 newlib 4.x**（BSD 许可，syscall stubs 清晰）；`libc/` 包装 crt/errno/pthread | 自写 libc 工期不可控；picolibc 为备选；musl 移植面更大 |
| D1-B | TLS 引擎 | **BearSSL（TLS 1.2）** + 自写 HTTP/1.1；不依赖 OpenSSL | mbedTLS 备选（更大、可 TLS 1.3）；自写 TLS 不作为首选 |
| D1-C | 网络位置 | **L1 内核库 + native socket syscall + facade**；netd 进程 D2 | 现在拆 netd 会先造 IPC/驱动 cap，拖长关键路径 |
| D1-D | 调度 | **内核保持协作式；加 ring3 抢占（WP5-0g）**；tick 只做时间/睡眠/idle + need_resched；不做内核抢占/SMP 调度 | 全内核抢占要求锁/可重入审计，D1 风险不可控；ring3 抢占成本低且能解决 WM 自旋 |
| D1-E | 真机范围 | **一款 UEFI 机型 + GOP + PS/2 + 一款 Intel 网卡；AP 停泊；无安装器** | 多机型/USB 存储/GPU 驱动会翻倍；安装器建议 D1.1 |
| D1-F | 浏览器 | **自有引擎、无 JS、单线程非阻塞事件循环**；B0 子集固定 | 移植 WebKit 不在 D1 评估范围；多线程 B0 会引入调度死锁 |
| D1-G | POSIX 进程模型 | **execve + posix_spawn；fork 延后**；线程用 clone + pthread 子集 | fork+COW 是 D2；无 fork 会限制部分 autotools 运行时，但目标库不需要 |
| D1-H | ABI | **ABI 1.1 只追加**（新号从 48 起）；POSIX 仍在用户态 | 改 1.0 已有语义会破坏冻结承诺；POSIX 进内核否决 |
| D1-I | 目标机 | **M1 预检时锁定具体型号**；候选：ThinkPad X230 / 带 82574L 的台式机 | 无机器则 D1-E5 降级为 OVMF，需负责人签字 |

## 8. 执行规则（给实现会话）

### 8.1 WP 协议

1. 一个 WP 一个会话；开工前读 `wps.md` 对应条目 + 依赖 WP 的 claims。
2. 只改该 WP 的“允许路径”；跨路径先改 `wps.md`/`decisions.md` 并说明。
3. 每个 WP 必须有：代码 + host 单测 + QEMU/真机证据 + `docs/claims/ng-WP5-x.md`。
4. 提交信息：`[ng][WP5-x] <动词>: <一句话>`；路径限定提交，不 `git add -A`，不 push。
5. 每个 WP 更新对应 `docs/design/*-ng.md` 或 domain 页；ABI 变更同步 `docs/abi/`。
6. 每完成一个 WP，跑 `tools/ci-ng.ps1`（至少 `-Quick` + 相关 QEMU 门禁）；
   8h 浸泡期间**不跑 QEMU**，改用空闲窗口或等浸泡结束（负责人协调）。

### 8.2 共享文件的单写者规则

D1 有三个高频冲突文件：`src/ng/main.c`（自检调用）、`src/ng/syscall.[ch]`
（编号/分发）、`tools/build-ng.ps1`（对象列表/构建参数）。规则：

- **ABI 号段**只在 WP5-0a 冻结；后续 WP 只“使用”，不重排。
- `build-ng.ps1` 同一时间只有一个 WP 改；其他 WP 把需要的源文件/参数写进
  claim，由构建 owner 合入。
- `main.c` 的自检调用按 WP 追加到文件末尾的独立块，减少 diff 冲突。
- 并行车道：**平台车道**（WP5-0/2/5）与**用户态车道**（WP5-1/3/4）尽量错开
  公共文件；socket/fd 类型变更由 WP5-0b + WP5-1a + WP5-2b 三方对齐后再动。

### 8.3 新门禁（按 WP 逐步接入）

| 门禁 | 覆盖 | 接入 WP |
|---|---|---|
| `tools/abi-check.ps1`（扩展） | ABI 1.0 不变 + 1.1 新增 | WP5-0a |
| `tools/posix-check.ps1` | libc/stdio/malloc/pthread/signal + zlib/FreeType/FFmpeg/SDL2 最小 | WP5-1b…1g |
| `tools/net-check.ps1` | host 单测 + QEMU slirp（DHCP/DNS/TCP/丢包） | WP5-2a…2d |
| `tools/tls-check.ps1` | KAT/证书正反/本地 HTTPS/真实站点 | WP5-3a…3c |
| `tools/browser-check.ps1` | 本地语料像素 + 畸形语料 + HTTPS 页面 | WP5-4a…4d |
| `tools/hw-check.ps1` | OVMF/真机启动、ACPI/APIC/SMP、1h | WP5-5a…5d |
| `tools/soak-d1.ps1` | 8h/24h、负载、故障注入、泄漏阈值 | WP5-6a…6c |

### 8.4 文档与 claim 命名

- 设计：`docs/design/<topic>-ng.md`（如 `libc-ng.md`、`net-ng.md`、
  `tls-http-ng.md`、`browser-ng.md`、`uefi-ng.md`、`hardware.md`）。
- 证据：`docs/claims/ng-WP5-0a.md` … `ng-WP5-6c.md`；每个 claim 写“期望
  串口行/测试输出/截图/哈希 + 复现命令”。
- 阶段汇总：`docs/design/d1/d1-report.md`（M6 时写），`docs/release/d1.md`。

### 8.5 不做的事（对实现会话）

- 不把 QEMU/真机日志、构建产物、字体/第三方大文件提交进 git（用
  `.gitignore` + pin 哈希 + 下载脚本）。
- 不在 8h 浸泡运行时启动第二个 QEMU（端口/CPU/内存互相干扰）。
- 不为了让测试过而放宽断言；已知限制写进 claims，不静默跳过。
- 不在 D1 中途改 ABI 1.0 已有编号/错误码/结构体布局。

## 9. 第一周建议（启动顺序）

1. **WP5-0a**：冻结 ABI 1.1 号段/错误码，`ENOSYS` 落地，`abi-check` 扩展。
2. **S1（newlib 预检）** 并行：验证交叉构建；失败则立刻切 picolibc/最小 libc。
3. **WP5-0b**：per-process fd/cwd/uid；同时把 fd 对象类型（FILE/SOCKET/PIPE）
   的扩展点留好（WP5-1a/2b 用）。
4. **WP5-0e/f**：FPU + tick/时间/熵；这两个是 POSIX/网络/浸泡的公共前提。
5. **S3（OVMF 预检）**：验证 UEFI stub/GOP/ACPI 路线的可行性，决定真机车道
   是否按计划推进。
6. 周末评审：M1 出口断言逐条过；S1/S3 结果决定 M2 是否缩范围。

## 10. 与其他文档的关系

- `wps.md`：32 个 WP 的编号/范围/允许路径/出口/依赖/会话估算。
- `risks.md`：风险、未知数、回退方案与“先验证什么”。
- `posix-profile.md`：POSIX → native 的 syscall/errno/结构体/上限映射，
  是 WP5-1a 的设计输入。
- `../evolution.md`：E5/D1 的阶段口径；本文件是它的执行细化。
- `../architecture.md` §10：D1 的长期目标；本文件对“安装器/更新/回滚”
  给出拆到 D1.1 的建议，需负责人确认。

## 8. 负责人裁决（2026-10-31）

以下为 D1 开工前的正式裁决，覆盖 §7 待确认项；后续 WP 以此为准。

| 决策 | 裁决 |
|---|---|
| D1-A libc 底座 | **newlib**（备选 picolibc，仅在 S1 预检失败时启用） |
| D1-B TLS/HTTP | **BearSSL TLS 1.2** + 自写 HTTP/1.1；不依赖 OpenSSL；TLS 1.3 留 D2 |
| D1-C 网络形态 | L1 内核网络库 + **native socket syscall** + 用户态 facade；netd 进程留 D2 |
| D1-D 调度 | D1 **保持协作式**；tick IRQ 只用于时间/睡眠/HLT，不做抢占、不做 SMP 调度 |
| D1-E 真机 | **一款 UEFI 机型** + GOP + PS/2（或 xHCI）+ Intel 网卡；AP 停泊；**不做安装器** |
| D1-G 进程模型 | **execve + posix_spawn**；`fork()` 返回 `ENOSYS`（D2 再评估） |
| D1-H ABI | **ABI 1.1 只追加**，新号自 48 起；ABI 1.0 语义不变；`abi-check` 同时校验 1.0/1.1 |
| 安装器/更新/回滚 | 拆到 **D1.1**，D1 不做 |
| fork/线程组 | D1 不做 fork；线程用 clone 子集 + futex（WP5-1c/1d） |

**开工顺序**：WP5-0a（ABI 1.1 + ENOSYS + abi-check）与 S1（newlib 预检）可并行；S1 只在 8h 浸泡结束后启动重负载构建。