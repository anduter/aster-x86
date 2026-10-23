# Aster OS 系统结构模型

> 状态：Phase 0 概念冻结（目标架构，不是现有实现说明）
>
> 适用范围：Aster 后续内核、服务、ABI、命名空间、测试与文档的共同基线。
>
> 与现有代码的关系：`docs/architecture.md` 记录当前原型的实现；本文描述**迁移目标**。
> 两者冲突时，以本文为设计目标，以现有代码为迁移输入。

---

## 1. 目标与范围

本文先回答五个结构问题，不展开具体 API 和代码：

1. **边界**：什么必须在内核，什么必须在用户态？
2. **资源**：对象有哪些大类，生命周期如何管理？
3. **权限**：进程凭什么访问资源，如何防止越权？
4. **命名**：没有全局根时，路径和服务发现如何工作？
5. **失败**：用户错误、驱动崩溃、内核异常分别怎么处理？

本文的产出是结构模型，不是实现计划，也不声称现有系统已经达到模型描述的状态。

### 1.1 默认假设

- 目标平台：32 位 x86、BIOS/MBR 启动、QEMU 验证；多核按 2 核 bring-up 推进（BSP + 1 AP）。
- 虚拟地址：3GB 用户空间 / 1GB 内核空间。
- 原生 ABI 优先；POSIX 只作为可选兼容层，不是内核语义。
- 采用“对象化混合内核”：ring0 保留不可替换的机制，文件系统、网络、驱动、加载器、shell 在 ring3 作为服务运行。
- 启动必需的串口、定时器、中断控制器、早期控制台可以先留在内核，但接口按服务契约设计，后续可迁移。
- 暂不做完整对称 SMP（当前只有双核受限并行）、动态链接、GUI、多用户登录、交换分区。

### 1.2 非目标

- 不追求 POSIX 兼容认证。
- 不宣称形式化验证或达到 seL4 级安全保证。
- 不在本阶段设计完整 TCP/IP、USB、GPU 或电源管理。
- 不把现有实现细节当作不可更改的约束。

---

## 2. 一句话模型

> **Aster 是一个基于能力（capability）的、强类型对象的、服务化的混合内核操作系统：
> 内核只提供对象、地址空间、线程、IPC、中断和时间等机制；所有资源都通过带类型和权限的句柄访问；
> 每个进程拥有私有命名空间，系统没有全局可写根、没有全局 `/dev`、没有全局注册表、没有 ambient root。**

---

## 3. 设计公理（不可破坏的不变量）

| 编号 | 不变量 | 含义 |
|---|---|---|
| I1 | 资源即对象 | 每个可管理资源都有类型、状态、生命周期、统计信息 |
| I2 | 权限即句柄 | 进程只能通过句柄访问对象；知道名字不等于有权限 |
| I3 | 无环境权限 | 新进程不自动继承任何权限，启动句柄必须显式授予 |
| I4 | 命名空间私有 | 每个进程有独立命名空间；不存在全局可写根 |
| I5 | 机制在内核，策略在服务 | 内核不解析路径、不实现文件系统语义、不做网络协议 |
| I6 | IPC 有界 | 消息队列有上限，同步调用必须有超时，内核快速路径不做无限分配 |
| I7 | 用户内存不可信 | 所有跨边界指针都做范围校验和可恢复缺页处理 |
| I8 | 故障可隔离 | 用户态异常不导致内核 panic；驱动崩溃可被监督者重启 |
| I9 | 可观测是默认能力 | 对象、句柄、IPC、调度、内存都有计数器与追踪点 |
| I10 | 单核实现，多核接口 | 双核 bring-up 已落地：per-CPU GDT/TSS、共享 IDT、IPI 与 TLB shootdown；对称调度未做 |

---

## 4. 总体结构

```text
┌──────────────────────────────────────────────────────────────┐
│ L5 应用与工具（ring3）                                       │
│   shell · 文件工具 · 测试程序 · 未来的编辑器/网络客户端       │
├──────────────────────────────────────────────────────────────┤
│ L4 服务域（ring3）                                           │
│   init/supervisor · device manager · FS · net · loader       │
│   console/input · log · identity · POSIX personality         │
├──────────────────────────────────────────────────────────────┤
│ L3 原生运行时与 ABI（ring3）                                 │
│   syscall stubs · ulib · ELF 启动 · namespace client         │
├──────────────────────────────────────────────────────────────┤
│ L2 内核机制（ring0）                                         │
│   object/capability · process/thread · scheduler             │
│   VM · IPC · IRQ/exception · time · sync · tracing           │
├──────────────────────────────────────────────────────────────┤
│ L1 HAL 与平台抽象（ring0）                                   │
│   boot · GDT/IDT/TSS · paging · PIC/PIT/APIC · PCI/ACPI      │
│   serial · DMA · power                                       │
├──────────────────────────────────────────────────────────────┤
│ L0 固件与硬件                                                │
└──────────────────────────────────────────────────────────────┘
```

**纵向切面**：安全（能力检查、审计）、可观测性（trace/counter）、工具链（构建/测试/调试）贯穿所有层。

**层间规则**：

- L1 只做架构相关操作，不含策略；不暴露给用户态。
- L2 只实现通用机制，不认识“文件系统”“TCP”“shell”。
- L3 是用户态进入内核的唯一合法入口，负责参数编组和句柄封装。
- L4 通过 IPC 组合，不直接调用内核内部函数。
- L5 只能看到 L3/L4 暴露的句柄和命名空间。

---

## 5. 大类与小类

### A. 引导与平台抽象（HAL）

| 编号 | 小类 | 职责 | 明确边界 |
|---|---|---|---|
| A1 | 引导链 | MBR → stage2 → 内核；校验、分块加载、跳转 | 不做设备策略 |
| A2 | 启动信息契约 | E820、VBE、ACPI、SMBIOS、PCI、启动盘信息 | 只读快照，固定布局 |
| A3 | CPU 架构 | GDT/IDT/TSS、ring 切换、分页、上下文切换、系统调用入口 | 架构相关，不解释语义 |
| A4 | 中断与定时 | PIC/PIT，后续 APIC/IOAPIC；IRQ 路由、异常入口 | 只投递事件，不处理设备业务 |
| A5 | 平台发现 | PCI 枚举、ACPI 表解析、设备资源描述 | 只报告资源，不绑定驱动 |
| A6 | DMA 与 IOMMU | 物理页锁定、DMA 地址映射、IOMMU 预留 | 不绕过能力检查 |
| A7 | 电源与复位 | ACPI S5、重启、关机 | 只提供机制 |
| A8 | HAL 接口 | 稳定的内部 `hal_*` 契约，隔离汇编与 C | 不暴露给 ring3 |

### B. 内核机制

| 编号 | 小类 | 职责 |
|---|---|---|
| B1 | 对象管理器 | 对象头、类型注册、引用计数、状态机、析构 |
| B2 | 能力与句柄 | 每进程句柄表、代际、类型标签、权限、复制/转移/撤销 |
| B3 | 执行体 | 进程、线程、安全域、生命周期、父子关系（无默认权限） |
| B4 | 调度器 | 优先级 + 公平轮转、时间片、等待队列、睡眠超时、抢占 |
| B5 | 虚拟内存 | PMM、AddressSpace、MemoryObject、映射/保护/COW/按需分页、W^X |
| B6 | IPC | Port、Channel、Event、Pipe；同步 call/reply、异步 send/recv、超时、句柄传递 |
| B7 | 异常与中断 | IDT、异常分发、IRQ 顶半部、缺页修复表、用户态故障转消息 |
| B8 | 时间 | 单调时钟、RTC、定时器、睡眠、NMI 看门狗 |
| B9 | 同步 | 内核互斥、信号量、等待队列；用户态 futex 预留 |
| B10 | 内核观测 | 日志环形缓冲、计数器、tracepoint、对象统计 |

### C. 对象目录（类型系统）

C 不是另一套机制，而是 B 之上对外可见的资源分类。

| 编号 | 对象族 | 对象 | 关键权限 | 生命周期 |
|---|---|---|---|---|
| C1 | 执行族 | Process、Thread、SecurityDomain | WAIT、SIGNAL、MANAGE | 进程退出回收线程 |
| C2 | 内存族 | AddressSpace、MemoryObject、Mapping | READ、WRITE、EXEC、MAP | 引用计数；撤销映射 |
| C3 | 通信族 | Port、Channel、Event、Pipe | CONNECT、READ、WRITE、WAIT、SIGNAL | 关闭端点后回收 |
| C4 | 存储族 | Volume、Directory、File、BlockDevice | READ、WRITE、MANAGE | 句柄 + 挂载引用 |
| C5 | 设备族 | Bus、Device、Driver、Interrupt、DmaBuffer | MAP、WAIT、ACK、MANAGE | 驱动卸载时回收 |
| C6 | 服务族 | Service、Component、Namespace、Config | CONNECT、TRAVERSE、MANAGE | 组件生命周期 |
| C7 | 时间族 | Clock、Timer | READ、WAIT、SET | 关闭即取消 |
| C8 | 调试族 | TraceBuffer、LogSink | READ、WRITE、MANAGE | 由日志服务管理 |

**类型系统规则**：

- 每个对象有稳定 `type_id` 和接口版本；类型错误返回 `E_TYPE`，不是“参数错误”。
- 对象头只由内核管理；用户永远拿不到裸指针。
- `File` 不是可执行对象；执行由 loader 服务把文件内容校验后转成 `MemoryObject`/`Process`，避免“路径即代码”。
- 对象族之间不共享可变状态；跨族交互一律走句柄 + 消息。

### D. 系统服务（ring3）

| 编号 | 小类 | 职责 | 不做什么 |
|---|---|---|---|
| D1 | init / supervisor | 启动服务、依赖排序、健康检查、重启策略、关机协调 | 不实现文件系统/网络 |
| D2 | device manager | PCI/ACPI 设备枚举、驱动匹配、资源能力分配 | 不直接操作设备寄存器 |
| D3 | storage / FS | 卷管理、块缓存、XFS1/FAT16 后端、挂载、同步、fsck | 不定义命名空间根 |
| D4 | network | e1000 驱动、ARP/IPv4/ICMP/UDP/TCP、socket 对象 | 不提供全局端口命名空间 |
| D5 | loader | ELF32 校验、段映射、W^X、参数栈、启动进程 | 不解释 shell 命令 |
| D6 | console/input | 帧缓冲控制台、键盘/鼠标事件、输入焦点 | 不实现命令行语法 |
| D7 | log/telemetry | 结构化日志、环形缓冲、sink、崩溃转储收集 | 不阻塞关键路径 |
| D8 | identity | 安全域标签、登录/会话（可选）、审计主体 | 不用 uid=0 代表全权 |
| D9 | POSIX personality | 把 fork/exec/signal/pipe/socket 映射到原生对象 | 不进入内核核心 |
| D10 | time/RTC | RTC 读取、时区换算、定时服务 | 不替代内核单调时钟 |

**服务启动顺序**：内核内建 init 映像 → init 获得 bootstrap 句柄 → device manager → block driver → FS → loader → console/log → shell。
任何服务崩溃由 supervisor 按策略重启，不拖垮内核。

### E. 命名与组合

| 编号 | 小类 | 职责 |
|---|---|---|
| E1 | Namespace 对象 | 每进程一个命名空间根；挂载表是对象状态 |
| E2 | Mount | 把 Volume、Directory、Service Port 挂到命名空间节点 |
| E3 | 路径语法 | `volume:/path`、相对路径、`.`、`..`；无全局 `/` |
| E4 | 解析器 | 从根/当前目录句柄逐段解析，每段检查 TRAVERSE/READ |
| E5 | cwd/root | 每进程当前目录和命名空间根；`..` 不越出挂载根 |
| E6 | 服务发现 | `svc:/name` 解析为 Port 句柄，不是全局注册表 |
| E7 | 配置 | 每个组件一个 Config 对象；无全局 registry |
| E8 | 继承与沙箱 | 子进程只得到显式传递的句柄和命名空间子树 |

**路径规则**：

```text
path    := [volume ":"] ["/"] segment {"/" segment}
volume  := name
segment := name | "." | ".."
```

- 省略卷名：相对当前目录所在卷。
- 以 `/` 开头：相对当前卷根，不是全局根。
- `..` 到卷根即停，不能跨挂载点逃逸。
- Phase 1 不支持符号链接；后续如支持，必须配 `NOFOLLOW` 和解析深度上限。
- 路径只负责“找到对象”，最终权限仍由返回的句柄权限决定。

### F. 原生 ABI 与运行时

| 编号 | 小类 | 职责 |
|---|---|---|
| F1 | 系统调用 ABI | `int 0x80` 起步；eax=nr，ebx/ecx/edx/esi/edi 传参；返回 `int32_t`，负值错误 |
| F2 | ulib | 句柄封装、malloc/printf/string、namespace client、IPC stub |
| F3 | ELF 与启动 | ELF32 静态加载、argc/argv/envp、TLS 预留、退出状态 |
| F4 | shell | 内建命令、外部程序、管道、重定向、历史；运行在 ring3 |
| F5 | SDK | 头文件、链接脚本、示例、ABI 版本文档 |
| F6 | 兼容层 | 可选 POSIX personality；不污染原生对象模型 |
| F7 | 控制台 UI | 文本 framebuffer；输入框/输出框；GUI 后置 |

**系统调用分组**（不是最终编号）：

- 句柄：`close`、`dup`、`info`、`transfer`、`revoke`
- 执行：`process_create`、`thread_create`、`wait`、`exit`、`yield`
- 内存：`mem_create`、`map`、`unmap`、`protect`
- IPC：`port_create`、`connect`、`send`、`recv`、`call`、`reply`、`wait_any`
- 时间：`now`、`sleep`、`timer_create`
- 设备：`irq_wait`、`irq_ack`、`dma_pin`（仅驱动服务）
- 调试：`log`、`system_info`

**关键决定**：原生模型不提供 `fork`/`execve` 作为核心语义。
创建进程 = `process_create(image_handle, argv, handle_set)`；`fork` 只作为 POSIX 兼容服务用 COW 实现。

### G. 工具链、可观测性与验证

| 编号 | 小类 | 职责 |
|---|---|---|
| G1 | 构建 | i686-elf 交叉工具链、NASM、镜像构建、可复现构建 |
| G2 | 测试 | 宿主单元测试、QEMU 集成测试、串口断言、故障注入 |
| G3 | 可观测性 | 日志级别、tracepoint、对象/句柄/IPC 计数器 |
| G4 | 调试 | GDB stub、panic/oops 现场、符号化栈回溯、core 摘要 |
| G5 | 文档 | 架构模型、ADR、ABI 版本表、迁移记录 |
| G6 | CI | 构建矩阵、启动冒烟、回归套件、静态检查 |
| G7 | 安全验证 | 能力泄漏测试、类型混淆测试、uaccess 缺页测试、W^X 检查 |

### H. 安全与可靠性（横切）

| 编号 | 小类 | 职责 |
|---|---|---|
| H1 | 威胁模型 | 恶意用户进程、被篡改的文件系统镜像、缺陷驱动、DMA 设备 |
| H2 | 最小权限 | 启动只给必需句柄；服务按 manifest 申请能力 |
| H3 | 内存安全 | 用户/内核地址隔离、W^X、栈 guard、SMEP/SMAP（硬件支持时） |
| H4 | 故障隔离 | 用户异常杀线程；驱动崩溃重启；内核 oops 杀当前线程；panic 仅最后手段 |
| H5 | 审计 | 记录能力授予/撤销、服务重启、权限拒绝 |
| H6 | 供应链 | 固定工具链版本、镜像签名/校验、依赖清单 |

---

## 6. 对象、句柄与权限细节

### 6.1 对象头

每个内核对象至少有：

```c
type_id, object_id, refcount, state,
owner_domain, ops, lock, stats
```

对象 ID 只用于调试和审计，不能当权限使用。

### 6.2 句柄

- 每进程一张句柄表；句柄是 32 位不透明值，由 `slot + generation + type tag` 组成。
- 句柄表项记录：对象指针、类型、权限位、授权父句柄、关闭状态。
- 关闭后 generation 递增，旧句柄立即失效，防止复用攻击。
- 句柄可以经 IPC 传递；接收方只能得到**权限子集**。
- 撤销一个句柄时，沿授权树撤销其派生句柄；Phase 1 至少支持单句柄撤销，Phase 2 完整递归撤销。

### 6.3 权限位

| 权限 | 适用对象 | 含义 |
|---|---|---|
| `READ` | File、Channel、MemoryObject | 读取内容 |
| `WRITE` | File、Channel、MemoryObject | 写入内容 |
| `EXEC` | MemoryObject | 可作为代码执行 |
| `MAP` | MemoryObject、Device | 映射到地址空间 |
| `WAIT` | Process、Event、Timer、Interrupt | 等待状态变化 |
| `SIGNAL` | Event、Process | 触发通知/信号 |
| `CONNECT` | Port | 建立 Channel |
| `TRANSFER` | 任意句柄 | 允许经 IPC 传递 |
| `MANAGE` | Service、Device、Volume | 管理/配置/销毁 |
| `ACK` | Interrupt | 中断应答 |

**错误语义**：类型不匹配返回 `E_TYPE`；权限不足返回 `E_ACCESS`；对象不存在返回 `E_NOENT`；
资源耗尽返回 `E_NOSPC`；超时返回 `E_TIMEOUT`。

### 6.4 进程模型

- `Process` = 地址空间 + 句柄表 + 命名空间 + 安全域 + 线程集合 + 退出状态。
- `Thread` 是调度单位；进程默认一个主线程，可创建更多。
- 创建进程必须显式给出：镜像句柄、参数、初始句柄集合、命名空间子树。
- 父进程对子进程没有天然控制权；只有拿到 `WAIT`/`SIGNAL`/`MANAGE` 句柄才能操作。
- 退出状态通过 `Event` 或 `wait` 返回；僵尸态只保留退出信息，资源已回收。
- 内核线程只用于内核工作，不承载用户策略。

### 6.5 内存模型

- 32 位地址空间：用户 `0x00000000–0xBFFFFFFF`，内核 `0xC0000000–0xFFFFFFFF`。
- 4KB 页；内核高半区共享但 supervisor-only；用户页按需映射。
- `MemoryObject` 是物理页的抽象，支持匿名、文件后端、共享；`Mapping` 记录地址、权限、COW 状态。
- 用户栈有 guard page，按需增长；堆由 ulib 通过 `mem_create/map` 管理。
- W^X：用户页不能同时可写和可执行；loader 分段映射。
- 所有 `copy_from_user/copy_to_user` 走可恢复缺页路径，失败返回错误而不是 panic。
- DMA 缓冲区必须 pin 住物理页，驱动只能拿到 DMA 地址能力。

### 6.6 IPC 模型

- `Port`：服务端监听对象，客户端用 `CONNECT` 权限建立连接。
- `Channel`：双向有界消息队列，两个端点句柄；支持 `send`、`recv`、`call`、`reply`。
- `Event`：可等待计数器，用于中断通知、异步完成、退出状态。
- `Pipe`：单向字节流，供 shell 管道和兼容层使用。
- 消息 = 头部（类型、长度、flags、句柄数）+ 最多 64 字节内联数据 + 最多 4 个句柄；
  大数据用共享 `MemoryObject`。
- `call` 必须有超时；队列满时 `send` 返回 `E_AGAIN` 或阻塞，不允许无限排队。
- `wait_any` 支持一次等待多个句柄，供事件循环和 shell 使用。
- 不做全局消息总线；所有 IPC 必须先有句柄。

---

## 7. 关键数据流

### 7.1 启动

```text
固件 → MBR → stage2 → 内核
内核：HAL → PMM/VMM → 对象/能力 → 调度/IPC/IRQ
内核创建 init（内建映像），传 console / bootinfo / namespace / service port
init：挂载 system:、data:、svc: → 启动 device manager → block → FS → loader → shell
```

### 7.2 打开并读取文件

```text
shell 持有 namespace 根句柄
→ resolve("system:/docs/a.txt")
→ 每段检查 TRAVERSE
→ 找到 system 卷的 Directory 句柄
→ open 得到 File 句柄（READ）
→ read(File) → FS 服务 → BlockDevice → 驱动
→ 数据返回；错误只返回状态，不 panic
```

### 7.3 启动程序

```text
supervisor → loader Port
→ loader 读取 ELF File 句柄
→ 校验 ELF、映射 RX/RW 段、构造栈
→ process_create(image, argv, handles)
→ 新进程主线程进入 ring3
```

### 7.4 网络

```text
应用持有 socket 句柄
→ net 服务 Channel
→ e1000 驱动服务
→ IRQ Event → RX 环
→ ARP/IPv4/UDP 处理
→ 回复写入 socket 队列
```

### 7.5 故障与重启

```text
用户异常 → 内核异常入口 → 转 fault 消息给 supervisor
→ supervisor 决定杀线程/杀进程/重启服务
驱动服务崩溃 → device manager 撤销资源句柄
→ supervisor 重启驱动 → 重新枚举/恢复
内核 oops → 打印现场 → 杀当前线程
完全无法恢复 → panic
```

---

## 8. 与参考系统的详细差异

### 8.1 总矩阵

| 维度 | Unix/Linux | Windows NT | seL4 | Mach | Plan 9 | Fuchsia | Aster |
|---|---|---|---|---|---|---|---|
| 资源抽象 | 文件为中心 | 对象管理器 + I/O | capability + untyped | port + memory object | 文件/9P | Zircon 对象 | 强类型对象 + capability |
| 命名 | 全局 VFS 根 | 全局对象命名空间 + 盘符 | 无全局名 | 每任务 port 名 | 每进程命名空间 | 组件命名空间 | 每进程命名空间 + 卷根 |
| 设备 | `/dev` + ioctl | 设备对象 + IRP | 用户态驱动 | 用户态驱动 | 文件 | 用户态驱动 | Device/Driver + 资源能力 |
| 进程创建 | fork/exec | CreateProcess | 能力授予 | task/thread | fork/exec | 组件启动 | spawn(image, handles) |
| IPC | pipe/socket/signal | ALPC/IRP | endpoint | port | 9P/pipe | channel | Port/Channel/Event |
| 权限 | uid/gid/DAC | token/ACL | capability | port right | 用户/文件权限 | capability routing | capability rights |
| 内核边界 | 宏内核 | 混合内核 | 微内核 | 微内核 | 混合 | 微内核 | 混合/服务化 |
| 兼容性 | POSIX | Win32 | 无 | 多语言 | 9P/Plan9 | POSIX 部分 | 原生 ABI + 可选 POSIX |

### 8.2 Unix / Linux

**借鉴**：进程/线程分离、虚拟内存、VFS 分层、系统调用纪律、页缓存思想。

**关键差异**：

1. **不是“一切皆文件”**：设备、进程、服务、定时器都是强类型对象，不走 `read/write/ioctl` 通用接口。
2. **没有全局 VFS 根**：每个进程有私有命名空间；`system:`、`data:` 是卷根，不拼成全局 `/`；
   没有 `/dev`、`/proc` 作为核心机制。
3. **权限不是 uid/gid/root**：没有 uid=0；权限完全来自句柄权限位。管理员只是持有更多能力的主体。
4. **不是宏内核**：文件系统、网络、驱动、加载器是 ring3 服务；内核只做机制。
5. **原生创建进程不是 fork/exec**：`spawn(image, handles)` 显式传权；`fork` 仅作为可选 POSIX 兼容层。
6. **进程信息不是 `/proc`**：通过 `sysinfo` 服务或对象查询获取，不伪造文件。

**代价**：POSIX 软件不能直接跑；需要兼容层；服务化 IPC 有性能开销。

### 8.3 Windows NT

**借鉴**：对象管理器、句柄表、异步 I/O、驱动分层、安全主体概念。

**关键差异**：

1. **没有全局对象命名空间**：NT 的 `\` 对象目录是全局的；Aster 的命名空间是每进程的，且没有全局可写根。
2. **没有 IRP 栈**：Aster 用类型化 Channel 请求/回复，设备服务自己解释协议；不强制 IRP 分层。
3. **没有全局注册表**：每个服务持有自己的 Config 对象；配置随组件授予，不靠全局键路径。
4. **权限不是 token/ACL**：Aster 用 capability rights，默认拒绝；NT 的 ACL 仍依赖主体身份。
5. **驱动默认在用户态**：NT 的 WDM/KMDF 主要在内核；Aster 的驱动是服务，崩溃可重启。
6. **句柄不自动继承**：NT 支持继承；Aster 只允许显式 transfer，减少权限泄漏。

**代价**：用户态驱动需要更复杂的 DMA/IRQ 能力模型；设备热插拔和电源管理更难。

### 8.4 seL4

**借鉴**：小而可信的内核、capability、endpoint IPC、用户态服务、能力空间隔离。

**关键差异**：

1. **不做形式化验证承诺**：seL4 有机器辅助证明；Aster 是教学/实验系统，不宣称同等安全保证。
2. **内存模型不同**：seL4 用 untyped memory + retype；Aster 用页级 PMM + MemoryObject，工程上更直接。
3. **内核对象更丰富**：Aster 内建 Process/Thread/Channel/Event 等对象；seL4 核心更极简，很多策略在用户态。
4. **自带服务套件**：Aster 定义 FS、net、loader、shell 等启动服务；seL4 不规定这些。
5. **目标不同**：Aster 优先可教学、可调试、可渐进实现，而不是最小可信基。

**代价**：不能借用 seL4 的安全证明；能力撤销和对象生命周期要自己验证。

### 8.5 Mach

**借鉴**：task/thread 分离、port 作为 IPC 核心、memory object、用户态服务器。

**关键差异**：

1. **句柄覆盖所有对象**：Mach 以 port right 为中心；Aster 的句柄统一覆盖文件、设备、进程、服务、定时器。
2. **没有 port set 复杂性**：Aster 用 `wait_any` 等待多个句柄，不引入 Mach 的 port set/right 组合。
3. **命名空间是独立层**：Mach 的 port 名是每任务局部名；Aster 在此之上加私有命名空间和挂载组合。
4. **服务组合显式化**：Aster 用 manifest/能力路由启动服务，不依赖全局 bootstrap port 约定。
5. **更简单的消息模型**：固定头部 + 内联数据 + 句柄数组，不做复杂 out-of-line 消息语法。

**代价**：IPC 性能可能不如专门优化的 Mach 实现；需要仔细设计等待/超时。

### 8.6 Plan 9

**借鉴**：每进程命名空间、挂载组合、简单协议、用户态服务。

**关键差异**：

1. **不是 9P 万能文件协议**：Plan 9 用文件协议统一资源；Aster 用类型化对象接口，不同对象有不同方法。
2. **设备/进程不是文件**：Aster 不把 `/proc`、`/dev` 作为核心；设备通过 Device 句柄访问。
3. **命名空间是能力作用域**：Plan 9 的命名空间仍以文件树表达；Aster 的命名空间只负责解析到对象，权限由句柄决定。
4. **没有全局网络文件系统**：Aster 的 `net:` 是本地服务命名空间，不默认挂载远程 9P。
5. **shell 管道走 Channel/Pipe**：不是所有命令都通过文件描述符。

**代价**：失去“所有资源统一用文件工具操作”的简洁性；需要更多类型化客户端。

### 8.7 Fuchsia / Zircon

**借鉴**：Zircon 句柄、Channel、组件化、能力路由、用户态驱动。

**关键差异**：

1. **规模与目标不同**：Fuchsia 是完整现代 OS，含 FIDL、async runtime、组件框架、包管理；
   Aster 是 32 位教学系统，先手写最小机制。
2. **没有 IDL 代码生成**：Aster 的 IPC 消息先用手写结构体和版本号，不引入 FIDL 工具链。
3. **对象类型更少**：只保留 Process/Thread/Memory/Channel/Port/Event/File/Device 等核心类型，
   避免 Zircon 式对象爆炸。
4. **组件模型更轻**：manifest 只描述所需/提供的能力和启动顺序，不做完整组件解析器。
5. **先单核同步模型，再多核**：Fuchsia 面向多核/异步；Aster 先在 BSP 把同步模型跑通，
   再把 AP 纳入受限调度（当前已实现）；接口从一开始就预留多核。

**代价**：没有成熟异步框架，服务端事件循环要自己写；扩展性有限。

### 8.8 Minix 3 / QNX（补充参考）

- **Minix 3**：借鉴 multiserver、可重启驱动、用户态服务；差异是 Aster 用原生类型化对象和 capability，
  不把 POSIX 服务器作为核心，supervisor 是显式一等服务。
- **QNX**：借鉴微内核消息传递、资源管理器、驱动重启；差异是 Aster 不把路径式资源管理器作为唯一抽象，
  权限由 capability 而非路径/进程身份决定。

---

## 9. 迁移路线与验收

### Phase 0（本文）

只写模型文档，不改代码。

**验收标准**：

1. 8 个大类、全部小类都有职责和边界。
2. 每个对象族都有对象、权限、生命周期。
3. 启动、文件、进程、网络、故障五条数据流都标出能力检查点。
4. 与 7 个参考系统的差异可逐条审查。
5. `docs/system-model.md` 完成，`README.md` 增加索引。

### 后续实现阶段（路线图）

| 阶段 | 内容 | 验收 |
|---|---|---|
| P1 | 对象/句柄/能力核心 | 创建、复制、传递、撤销、类型错误测试 |
| P2 | VM + IPC + 进程创建 | 用户态进程通过 Channel 调用服务；无句柄无法访问 |
| P3 | init/device manager/FS/loader 服务化 | 驱动崩溃可重启；命名空间挂载正确 |
| P4 | shell + 网络 + 兼容层 | ring3 shell 管道/重定向；ping/UDP 回显 |
| P5 | 硬化与验证 | uaccess 故障注入、能力模糊测试、W^X 检查、CI |

---

## 10. 风险与待确认决策

### 风险

1. **服务化驱动复杂度高**：IRQ/DMA 能力模型如果设计不好，会比宏内核更难调试。
2. **能力撤销成本高**：授权树、并发撤销、句柄复用需要仔细验证。
3. **32 位地址空间限制**：用户/内核 3G/1G 划分会限制大文件缓存和多服务。
4. **POSIX 兼容层可能反客为主**：必须明确它是可选 personality，不污染原生 ABI。
5. **启动依赖链**：FS 需要块驱动，块驱动需要 device manager，init 必须足够小且可靠。
6. **性能**：IPC 和服务组合会增加上下文切换；需要基准和快速路径。

### 默认决策（可在进入实现前修改）

- 目标仍是 32 位 x86、BIOS/MBR（双核 bring-up 已验证）；若改为 x86-64/UEFI，HAL 和 ABI 重做，上层对象模型不变。
- 原生 ABI 优先；POSIX 只做可选兼容层。
- 驱动最终用户态；启动关键平台代码可暂留内核。
- 文件系统先保留卷根模型（`system:`、`data:`），不引入全局 `/`。
- 不支持完整对称 SMP（当前为双核受限并行）、符号链接、动态链接、GUI、多用户登录，直到核心模型稳定。

### 已确认决策

1. **原生对象 ABI 优先**：已确认。系统调用、句柄、IPC、进程创建和启动约定先用
   [Native ABI v0](native-abi.md) 固化；POSIX/Linux 兼容层后置，不进入 v0。
2. **32 位教学路线 vs x86-64/UEFI**：仍待确认；当前文档按 32 位 x86 编写。
3. **驱动一开始用户态 vs 先内核态跑通**：仍待确认；当前按“最终用户态、启动关键代码暂留内核”推进。

Native ABI v0 明确采用 `int 0x80`，旧原型的 POSIX 风格调用表迁移到 `int 0x81` 作为过渡脚手架，
不作为长期兼容承诺。

---

## 11. 术语表

| 术语 | 含义 |
|---|---|
| 对象（Object） | 有类型、状态、生命周期和权限的内核资源抽象 |
| 句柄（Handle） | 进程句柄表中的不透明引用，携带类型和权限 |
| 能力（Capability） | 句柄所代表的、可传递且不可伪造的访问权 |
| 命名空间（Namespace） | 进程私有的名字到对象句柄的映射 |
| Port | 服务端监听端点，客户端连接后得到 Channel |
| Channel | 双向有界消息通道，可传句柄和共享内存 |
| Event | 可等待的通知计数器 |
| MemoryObject | 可被映射的物理页集合，支持匿名/文件/共享 |
| Domain | 安全主体标签，用于审计；本身不授予权限 |
| Personality | 把另一套 ABI（如 POSIX）映射到原生对象的兼容服务 |
