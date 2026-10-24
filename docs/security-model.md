# Aster 安全模型（P3 阶段）

> 状态：P3 安全边界第一版（目标架构的安全基线，不是完整安全认证）
>
> 关联文档：[系统结构模型](system-model.md) · [Native ABI v0](native-abi.md)
>
> 适用范围：32 位 x86、单核、ring3 用户程序、当前原型内核。

---

## 1. 目标与边界

P3 的目标不是“做成一个经过认证的安全内核”，而是先把**可验证的边界**建立起来：

1. ring3 不能直接访问内核地址空间；
2. ring3 不能改写自己的代码页；
3. 加载器拒绝明显恶意的 ELF（RWX 段、越界段、错误入口）；
4. fork 的 COW 只作用于可写页，不能把只读代码页“复制成可写”；
5. 句柄必须带类型和权限，关闭后旧值失效；
6. 构建对编译器告警零容忍，回归必须可重复。

### 非目标

- 不宣称达到 seL4 的形式化安全保证。
- 不做 TPM/安全启动和生产密钥管理。
- 不把 MMIO/设备 DMA 纳入 W^X（这些区域只做数据访问，使用 4KB NX 映射）。
- 不做完整 SMP 调度、侧信道、物理攻击防护；AP 已调度 heartbeat，用户进程仍主要在 BSP。

---

## 2. 威胁模型

### 2.1 受保护资产

| 资产 | 说明 |
|---|---|
| 内核代码与数据 | ring0 地址空间、页表、对象表、调度器 |
| 其他进程 | 用户页、句柄、文件描述符、控制台输入 |
| 持久化文件 | XFS1/FAT16 卷上的文件与元数据 |
| 信任链 | 当前尚未接入签名；未来是 manifest/SealedImage |

### 2.2 攻击者能力

| 攻击者 | 能力 |
|---|---|
| 恶意 ring3 程序 | 任意用户指令、系统调用、错误指针、越界读写、尝试写代码页 |
| 恶意 ELF 文件 | 构造异常 program header、RWX 段、越界 vaddr、错误入口 |
| 缺陷用户程序 | 空指针、栈溢出、use-after-close、错误句柄 |
| 缺陷内核代码 | 未校验的用户指针、对象生命周期错误、竞态 |

### 2.3 不在当前威胁模型内

- 能改内核镜像或启动链的攻击者（没有安全启动/签名）。
- 物理内存探测、冷启动、DMA 攻击（没有 IOMMU）。
- 微架构侧信道。
- 能加载内核模块的攻击者（当前没有模块加载）。

---

## 3. 信任边界

```text
ring3 用户程序
   │  只能通过 int 0x80 / int 0x81
   ▼
系统调用入口（DPL3 门）
   │  参数校验 + copy_from_user / copy_to_user
   ▼
内核对象/页表/调度器（ring0）
   │  U/S 页表位 + 句柄类型/权限
   ▼
硬件
```

关键规则：

- 用户指针必须落在 `0x00000000–0xBFFFFFFF`，且通过可恢复拷贝访问。
- 内核不信任用户提供的任何长度、句柄、ELF 字段。
- 句柄类型错误返回 `E_TYPE`；权限不足返回 `E_ACCESS`；无效句柄返回 `E_BADF`。
- 内核不因 ring3 的普通异常而 panic；用户异常终止当前进程/线程。

---

## 4. 当前已实现的控制

### 4.1 用户/内核隔离

- 用户页映射同时设置 PDE/PTE 的 U/S 位（`vmm_map_user_page`）。
- 内核页不带 U/S，ring3 访问触发 #PF。
- 每进程独立页目录；进程切换装入自己的 CR3。
- 用户代码/数据/栈/堆都在用户区，内核高半区 supervisor-only。

### 4.2 ELF 加载器加固（P3）

`src/kernel/user/user_mode.c` 现在：

- 校验 ELF32、小端、`PT_LOAD`、`p_memsz >= p_filesz`；
- 拒绝 `vaddr + memsz` 越界、`off + filesz` 越界、入口不在用户区；
- 限制 program header 数量（≤16）；
- **拒绝 `PF_W | PF_X` 段**；
- 按 `p_flags` 映射：
  - `PF_X` 段 → `PTE_PRESENT | PTE_USER`（只读、可执行）
  - `PF_W` 段 → `PTE_PRESENT | PTE_WRITE | PTE_USER`（可写）
- 用户链接脚本拆成两个 `PT_LOAD`：`text` = R+X，`data/bss` = R+W；
- 启动日志标记 `user: ELF text RO / data RW (W^X loader)`。

### 4.3 COW 与只读代码页

`src/kernel/mm/vmm.c`：

- 新增软件 PTE 位 `PTE_COW`。
- fork 时只对**原本可写或已经是 COW** 的页建立 COW。
- 代码页保持永久只读；fork 后写入代码页触发 #PF，`vmm_cow_fault` 拒绝复制，进程被终止。
- COW 页写缺页时复制并映射为可写；代码页不会被“复制成可写”。

### 4.4 句柄与权限

`src/kernel/intr/native.c`：

- 每进程 16 槽句柄表，句柄带 generation；
- 关闭后 generation 递增，旧句柄立即 `E_BADF`；
- `dup` 只能缩小权限，权限扩张返回 `E_ACCESS`；
- 槽 0 为进程 self handle；后续由 bootstrap 句柄块交付用户程序；
- 类型/权限检查在系统调用入口完成。

### 4.5 用户指针

- legacy 与 native 系统调用都通过 `copy_from_user`/`copy_to_user` 访问用户内存；
- 缺页由 `uaccess` 修复表接住，返回 `E_FAULT`，不 panic；
- 长度/地址溢出检查（`0xC0000000` 上界）。

### 4.6 工程门禁

- 内核与用户态编译启用 `-Wall -Wextra -Werror`；
- `build.ps1 -Test` 自动启动 QEMU、注入命令、断言串口日志；
- 当前回归 97 项全绿（2026-10-24 15:30 实测 `=== TEST PASS (97/97)`）；
- 新增 W^X 加载断言；
- P3.1 将用户地址布局收敛到 `user_layout.h`，栈、堆、蹦床页互不重叠。

### 4.7 用户地址布局（P3.1）

| 区域 | 地址 | 权限/行为 |
|---|---|---|
| 代码/数据 | `0x40000000..0x40100000` | text 只读，data 可写 |
| 用户栈 | `0x40100000..0x40200000` | 向下增长；底部 4KB guard 永不映射 |
| 用户堆 | `0x40200000..0x40300000` | brk 向上增长 |
| 信号蹦床 | `0x40300000` | 独立只读页，移出栈增长区 |

- 所有地址常量来自 [src/kernel/user/user_layout.h](../src/kernel/user/user_layout.h)。
- fork 的 COW 范围覆盖 `0x40000000..0x40400000`，包含栈、堆、蹦床页。
- 栈增长到 guard 就拒绝，不再覆盖信号蹦床页。

### 4.8 SMEP / SMAP（P3.2）

- `core/cpu.c` 通过 CPUID.07H:EBX 检测 SMEP（bit 7）/ SMAP（bit 20）。
- 支持时置 CR4.SMEP（bit 20）/ CR4.SMAP（bit 21）；不支持则保持关闭并打印状态。
- SMAP 启用后：
  - `uaccess_copy.asm` 在 `rep movsb` 前后执行 STAC/CLAC；
  - 信号帧写入、`sigreturn` 读取用户栈通过 `user_access_begin/end` 开窗口；
  - 所有普通用户指针都走 `copy_from_user/copy_to_user`。
- QEMU 回归使用 `-cpu max`，日志确认两项均为 `supported=1 enabled=1`；
  完整 43 项测试在 SMAP 开启下通过。

### 4.9 用户栈 / 堆 / 蹦床 ASLR

- 每个进程栈顶在 1MB 栈区的顶部 512KB 内随机（128 个 4KB 槽位）。
- 堆基址在 256KB 内随机（64 个槽位）；蹦床页在 256KB 内随机（64 个槽位）。
- 随机状态由 TSC + timer ticks + PID 混合，跨启动具有时序熵。
- 连续/活动进程的栈、堆、蹦床槽位做去重，避免合法碰撞与回归抖动。
- `proc_t` 记录 `user_stack_top` / `user_stack_va` / `user_heap_base` / `user_trampoline`；
  fork 子进程继承这些边界。
- `SYS_BRK`、`user_try_grow_stack`、信号帧写入、`sigreturn`、`exec` 都使用进程级随机地址。
- 栈 guard 固定在 `0x40100000`，不随 ASLR 移动。
- 回归断言要求至少两个进程的栈/堆/蹦床 `ASLR` 地址不同；当前日志示例：
  `stack_init=0x401f4000 heap_base=0x40236000 tramp=0x40333000`（pid 1）
  与对应 pid 3 的三个值均不同。

### 4.10 PAE / NX 与内核/用户页 W^X

- 页表已切换为 **32 位 PAE**：CR3 指向 PDPT，内核恒等映射使用 2MB 页，
  用户区和内核高地址使用 4KB PTE。
- `vmm_init` 检测 CPUID.PAE，`vmm_enable_paging` 启用 CR4.PAE 和 EFER.NXE。
- 用户页权限：
  - `.text`：R + X，NX=0；
  - `.rodata`：R，NX=1；
  - `.data/.bss`、堆、栈：R + W，NX=1；
  - 信号蹦床：R + X，NX=0。
- 可写页和 `PTE_NOEXEC` 页在 `vmm_map_user_page` 中设置 PTE.NX；
  fork COW 时同时继承 `PTE_NOEXEC`，避免 rodata 在子进程里重新变成可执行。
- `user.ld` 现在生成三个 `PT_LOAD`：text R+X、rodata R、data R+W。
- `sectest` 实测：
  - 执行栈地址：`err=15`（取指 NX），status=142；
  - 执行 rodata 地址：`err=15`，status=142。

### 4.11 Syscall 沙箱 v0

- `proc_t` 增加 `sandboxed`、64 位 `syscall_mask`（legacy）和 `native_mask`（native）。
- legacy `int 0x81` 分发入口在进入 switch 前检查 allowlist；未授权调用直接返回 `-1`。
- native `int 0x80` 分发入口做同样检查；未授权 native 调用返回 `NATIVE_E_ACCESS`。
- `userdemo` 被设为沙箱进程：
  - legacy 只允许 `write/exit/getpid/yield/sleep`；
  - native 只允许 `ABI_VERSION/EXIT/YIELD/DEBUG_WRITE`。
- demo 尝试打开 `system:/bin/shell`（legacy）和 `HANDLE_DUP`（native）：
  - `sandbox: open denied`
  - `sandbox: native denied`
- 当前回归 97 项，两项都是硬断言。
- 局限：策略仍按程序名硬编码；尚未升级为基于句柄权限的 capability 沙箱。

### 4.12 RSA 签名包 / 执行时验签

- 构建时由 `sign_image.ps1` 用 OpenSSL 对 `obj/user_shell.elf` 做
  **RSA-2048 + SHA-256 PKCS#1 v1.5** 签名。
- 生成 `obj/shell_signature.c`（签名 + 模数，256 字节大端），编进内核。
- 内核 `core/trust.c` 实现 SHA-256 + 2048 位模幂，验证 PKCS#1 DigestInfo。
- 两处强制执行：
  1. 启动 `trust_selftest()`：验证内嵌 shell，并复制镜像翻转一个字节，
     要求篡改副本被拒绝；失败直接 panic。
  2. `user_load_program()`：从文件系统读出的镜像在映射/执行前再次验签，
     未签名文件返回 `-1`，不会进入 ring3。
- 回归日志：

```text
trust: shell signature OK
trust: tampered image rejected
```

- 当前使用仓库内的**测试私钥** `keys/shell_private.pem`；生产环境必须改为
  离线私钥/HSM，并补撤销、公证和密钥轮换。

#### 4.12.1 loader 验证 kernel（启动链）

- 构建时对 `obj/kernel.bin` 用同一 RSA-2048 私钥签名，生成
  `obj/kernel_signature.c`（`kernel_signature` / `kernel_modulus`）。
- 一个独立的 32 位 freestanding 验证器链接到 **0x9800**，嵌入 loader
  的 0x9800 起始位置；loader 加载完 kernel、切保护模式后先跳验证器。
- 验证器读取 `0x100000` 的 kernel 头和 `0x100004` 的长度，验证通过输出
  `boot: KERNEL SIGNATURE OK` 并跳 `0x100008`；失败输出
  `boot: KERNEL SIGNATURE INVALID` 并 `hlt`，不会执行内核。
- 负向测试：翻转 `dist/floppy.img` 的 kernel 字节后启动，串口只出现
  `KERNEL SIGNATURE INVALID`，没有 `kernel: x86 OS`。

- 信任根说明：boot sector 和 loader 仍然是根（boot sector 只校验
  `LOADER` 后缀）；没有 BIOS/UEFI Secure Boot/TPM 时，替换整个 bootloader
  仍可绕过。kernel 签名解决的是“loader 可信时 kernel 不被替换”，
  不是完整的硬件信任链。

---

## 5. 明确限制（必须诚实记录）

| 限制 | 影响 | 后续 |
|---|---|---|
| 仅栈/堆/蹦床 ASLR | 代码/数据段仍未 PIE 化，整体 ASLR 未完成 | P4 |
| 沙箱策略硬编码 | 按程序名设置 allowlist，尚未由 manifest/句柄权限驱动 | capability 沙箱 |
| 签名测试密钥在仓库 | 仅用于教学/回归，不能用于生产 | 离线私钥/HSM + 撤销/公证 |
| 能力表容量固定 | 每进程 16 项、generation 16 位；安装策略仍按程序名 allowlist | P4 动态容量 + manifest/句柄权限驱动 |
| SMP 共享状态锁不足 | 调度器已支持 per-CPU runqueue、按负载放置与迁移，但 fs、对象表、pipe 等子系统仍无锁，用户进程跨核并发存在竞态 | 给共享子系统加锁或先引入内核大锁（BKL） |
| boot sector/loader 仍是信任根 | 替换整个 bootloader 可绕过 kernel 签名；无硬件 Secure Boot | UEFI Secure Boot/TPM measured boot |
| 没有审计/速率限制 | 拒绝服务与探测较容易 | 日志计数器 + 配额 |
| 没有内核栈保护 | 内核栈溢出可能破坏相邻内存 | guard page + canary |

**内核与用户页 W^X 均已成立**：内核 text R+X、rodata R+NX、data/bss RW+NX，
`__kernel_end` 以上的恒等映射 2MB 页统一 NX；用户 text R+X、rodata R+NX、
data/stack/heap RW+NX。`err=15` 的栈/rodata 取指回归证明 NX 真正生效。

---

## 6. 安全测试矩阵

| 测试 | 期望 |
|---|---|
| 用户访问内核地址 | #PF，终止进程，不 panic |
| 用户写代码页 | #PF，COW 拒绝，终止进程；`sectest` 验证 status=142 |
| fork 后父/子写数据页 | 各得独立副本，互不影响 |
| fork 后子写代码页 | 不复制为可写，终止 |
| 栈访问越过 guard | #PF，终止进程，不覆盖蹦床页；`sectest` 验证 status=142 |
| 信号返回地址 | 蹦床页固定在 `0x40300000`，栈增长不覆盖 |
| ELF 含 RWX 段 | 加载失败，不进入 ring3 |
| ELF vaddr 越界 | 加载失败 |
| ELF 入口越界 | 加载失败 |
| 非法句柄 close/dup/info | `E_BADF` |
| 权限扩张 dup | `E_ACCESS` |
| SMEP/SMAP CPU 支持 | 启用并记录 `supported=1 enabled=1` |
| SMAP 开启后的用户拷贝/信号 | 完整回归通过，无意外 #PF |
| 用户栈/堆/蹦床 ASLR | 至少两个进程的三类随机地址不同 |
| PAE/NX 启用 | `vmm: PAE on, NX enabled` |
| 执行栈地址 | #PF，`err=15`，`sectest` status=142 |
| 执行 rodata 地址 | #PF，`err=15`，`sectest` status=142 |
| 沙箱进程打开文件 | `SYS_OPEN` 返回 -1，打印 `sandbox: open denied` |
| 沙箱进程 native 句柄操作 | `HANDLE_DUP` 返回 `NATIVE_E_ACCESS`，打印 `sandbox: native denied` |
| 有效 shell 签名 | `trust: shell signature OK` |
| 篡改 shell 镜像 | `trust: tampered image rejected` |
| 未签名文件 exec | 验签失败，不进入 ring3 |
| 进程退出/exec 回收 | `vmm: address space teardown ok`，PMM 空闲页数复原 |
| PMM 线性分配 | `pmm: linear first-fit selftest ok`，heap/位图页不可分配 |
| 内核 text/rodata/data 权限 | `vmm: kernel W^X active`；text RX、rodata R+NX、data RW+NX |
| 对象引用生命周期 | `demo: fs refcount ok`；fork/fd 共享句柄不会提前释放对象 |
| fd capability rights | 只读打开 `fd write denied`；drop WRITE `cap drop denied`；跨进程发送 `cap recv hello from ramfs` |
| 通用 capability 表 | `cap: table selftest ok`；install → transfer → close/revoke，generation 防旧句柄 |
| native process_create/wait | `native: process_create/wait ok`；按注册程序名创建子进程，子进程可在 AP 运行 |
| native capability | `native: handle table ok`；restrict 降权、transfer 移动、revoke 失效、generation 防旧句柄 |
| 匿名 mmap | `sectest: mmap ok`；mmap 页写读 + munmap，页表/物理页回收 |
| Channel IPC | `native: channel ok`；成对 endpoint、send/recv、阻塞 recv + wakeup、native handle rights |
| Channel 阻塞唤醒 | `native: channel block ok`；子进程 sleep 后 send，父进程从 CHAN wait key 唤醒 |
| Service registry | `native: service ok`；name → Port 注册/发现，再 connect/accept |
| Supervisor | `native: supervisor ok`；服务循环 accept/查询 registry/回复，`proc_block` 不再假醒 |
| Supervisor 服务 | `echosvc: online`；supervisor + echosvc 由 boot 启动，manifest 检查 echo |
| 周期 watchdog | `svcwatch: restart echo -> pid N`；每 200ms 检查 echo，缺失就 spawn echosvc |
| Restart 后可用 | `native: restart ok`；supervisor 只 stop，客户端重试 service_connect 并完成 serv 往返 |
| VFS namespace | `sectest: chroot ok`；per-process root 侧表，rooted 后拒绝 `..` 越界 |
| VFS 大限制 | `sectest: fd limit ok`；PROC_FD_MAX=16、FS_MAX_OPEN=32，10 个并发文件 |
| ring3 AP | `ring3 AP probe ok`；最小 write/exit 用户程序固定 AP，独立于 fork/IPC |
| ring3 FS 服务 | `sectest: fs service ok`；fsd 在 ring3 持有内存文件，经 Port/Channel 提供 read |
| ring3 FS 写 | `sectest: fs write ok`；'w'+data 写入用户态文件，VFS svc 读回新内容 |
| VFS svc 后端 | `sectest: svc vfs ok`；VFS `svc:` 路由到 ring3 fsd，open/read 走服务 |
| VFS svc 写 | `sectest: svc vfs write ok`；write 缓冲，close 发送 `'w'+data` 给 fsd |
| VFS blk 服务 | `sectest: blk service ok`；ring3 blkd 虚拟块服务，VFS `blk:` 经 Channel 读取 |
| VFS blk 写 | `sectest: blk write ok`；write 缓冲，close 发送 `'w'+data` 给 blkd |
| 用户触发 TCP | `sectest: tcp ok`；SYS_TCP_TEST，在 shell 就绪后运行，避免 boot 探测拖死 |
| Service manifest | `native: manifest ok`；supervisor 检查 supervisor + echo 服务清单 |
| 网络 RX/UDP | `net: ARP reply`、`net: ICMP echo reply`、`net: UDP echo reply`；e1000 RX + ARP/ICMP/UDP |
| TCP | `net: TCP echo reply`；SYN/SYN-ACK/ACK/data、多连接重试 + data 重传，计入硬回归 |
| 用户态 UDP | `sectest: udp ok`；SYS_UDP_SEND/RECV + 宿主回显 |
| socket fd | `sectest: socket udp ok`；socket→sendto→recvfrom→close，fd rights |
| Port IPC | `native: port ok`；create → connect → accept（含阻塞 accept）→ channel 数据 |
| AP ring3 | `user: entering ring3 (builtin demo ... cpu=1)`；跨 CPU console 由 spinlock 原子化 |
| MADT 多核发现 | `madt: 2 cpu(s)`；`smp: 2/2 cpus online, 1 AP GDT/TSS/IDT ready, AP ticks=... ipi=...`；heartbeat 在 AP 执行 |
| 启动链 kernel 签名 | `boot: KERNEL SIGNATURE OK`；篡改 kernel 输出 INVALID 并停机 |
| exec 切换 | `user: exec system:/bin/shell` 后再次打印 `ushell: ready` |
| 关闭后旧句柄 | `E_BADF` |
| 用户指针未映射 | `E_FAULT`，不 panic |
| 编译告警 | 构建失败（`-Werror`） |
| 全量回归 | `build.ps1 -Test` 全绿 |

---

## 7. 工程约定

### 7.1 构建

- 内核和用户态都必须用 `-Werror` 构建。
- 不允许新增 RWX `PT_LOAD`；链接器告警应视为失败信号（当前已消除）。
- 修改 `build.ps1` 后必须保持 UTF-8 BOM，否则 Windows PowerShell 会错误解析中文。
- 构建镜像、启动 QEMU、断言串口日志必须一条命令可重复。

### 7.2 变更纪律

- 新的用户态映射必须显式声明权限；禁止默认 `PTE_WRITE`。
- 新的系统调用必须做：指针校验、长度校验、句柄类型/权限校验、失败路径。
- 新的对象必须定义：类型、权限、生命周期、错误码。
- 新的安全关键路径必须有启动自检或 QEMU 回归断言。
- 内核日志（`kprintf/kprintln`）必须整行原子输出，避免进程切换/中断把安全断言日志撕裂；使用 `console_write_atomic()`。
- 发现“暂时绕过”的安全检查，必须记录到本文限制表。

### 7.3 后续优先级

1. **P3.1（已完成）**：用户地址空间重新布局 + 栈 guard + 蹦床页分离。
2. **P3.2（已完成）**：SMEP/SMAP、用户栈 ASLR、PAE/NX 与内核/用户页 W^X。
3. **P3.3（已完成）**：RSA-2048/SHA-256 签名包 + 启动/执行时验签 + 篡改拒绝。
4. **P3.4（v0 完成）**：legacy syscall allowlist 沙箱已验证；native 能力沙箱列为 P4。
5. **P3.5**：完整代码/数据 ASLR、审计日志、配额与速率限制。

---

## 8. 报告安全问题

当前项目是实验/教学系统，没有生产安全承诺。发现越权、越界、提权或内核崩溃路径时，
应附带：

- 触发条件与最小复现；
- 串口日志/panic 现场；
- 影响范围（ring3→ring0、进程间、持久化文件）；
- 建议修复与回归用例。
