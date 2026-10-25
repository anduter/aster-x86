# 内核模块划分（九模块）

本文是模块边界契约。新代码按「归属决策树」定位；跨模块只能 include `src/kernel/include/<mod>.h`。

## 1. 分层与模块

### L0 底座（机制，不含策略）

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M0 启动与内核骨架** | 启动链、init 注册表、日志/panic/符号、公共 libc | 能进内核并跑完 init 的镜像 | 子系统初始化细节、自检、演示、对象表 |

### L1 资源（按资源所有权）

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M1 进程管理** | CPU 时间、进程/线程、调度、trap 入口、信号 | 可运行/切换/回收的执行体 | 具体设备、文件、网络 |
| **M2 内存管理** | 物理页、地址空间、页表、堆、对象生命周期 | 可分配/映射/回收的地址空间 | 调度策略、文件语义 |
| **M3 设备管理** | PCI/ACPI、块设备、字符设备、中断控制器 | 统一设备模型 + 驱动注册 | 文件系统、协议栈、服务编排 |
| **M4 文件系统** | 命名空间、持久化、权限执行 | 路径 → 文件的稳定视图 | 块设备驱动、用户程序 |
| **M5 网络管理** | ARP/ICMP/UDP/TCP、socket | 可收发数据的 socket | 网卡寄存器细节（归 M3） |

### L2 策略（跨资源协调）

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M6 服务管理** | 服务注册/发现/生命周期、channel/port | 可注册、可重启的服务运行时 | 具体服务业务逻辑 |
| **M7 安全管理** | 身份、capability、验签、沙箱、W^X 策略 | 统一权限判定入口 | 文件系统本身、加载器 |

### L3 用户态

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M8 用户态环境** | ulib、ABI 头、ELF 加载、程序注册、shell | ring3 可运行的程序环境 | GDT/页表实现、服务编排、内核 syscall 实现 |

## 2. 归属决策树

```
1. 是启动/日志/panic/libc/init 注册表？        → M0
2. 管 CPU 时间、进程、陷入、调度？              → M1
3. 管页、地址空间、对象生命周期、堆？            → M2
4. 直接访问硬件寄存器、总线、中断控制器？        → M3
5. 管路径、文件、目录、持久化、权限执行？        → M4
6. 管协议、报文、socket？                        → M5
7. 管服务注册、发现、重启、IPC 端口？            → M6
8. 管身份、权限判定、验签、隔离策略？            → M7
9. 在 ring3 跑，或负责加载 ring3？               → M8
10. 都不是 → 不是新模块，是某模块的子功能；禁止新建 misc/core/common
```

## 3. 目录布局

```
src/
├─ boot/  loader/                  M0 启动链
├─ kernel/
│  ├─ include/                     十个对外头（唯一跨模块入口）
│  ├─ core/                        M0：kernel.c / bootseq / panic / ksym / version / splash / M0 原语 / misc_syscall
│  ├─ lib/                         M0：kprintf、string
│  ├─ proc/                        M1：进程、调度、switch、gdt、smp、trap/
│  ├─ mm/                          M2：pmm、vmm、heap、object、user_layout
│  ├─ dev/                         M3：device/pci/acpi/rtc、block/、char/、intc/、net/
│  ├─ fs/                          M4：vfs、ramfs、fat16、svcfs、blkfs、pipe
│  ├─ net/                         M5：net 协议栈（e1000 在 dev/net）
│  ├─ svc/                         M6：service、supervisor、channel、port
│  ├─ security/                    M7：trust、cap、cpu features
│  ├─ user/                        M8 内核侧：loader、entry、program registry
│  └─ apps/                        内核启动的服务程序
└─ user/                           M8 用户态：ulib、user_shell、链接脚本
```

依赖方向（单向）：

```
M0 → M2 → M1 → M3 → M4
              ↘ M5
M6 →（M1/M2/M4/M5）
M7 →（M1/M2/M4）
M8 → 只依赖用户态 ABI（src/user/ulib.h）
```

## 4. 硬指标

| 指标 | 阈值 | 检查方式 |
|---|---|---|
| `core/kernel.c` | ≤ 120 行，只含注册表 + init 调用 | 人工 + 行数 |
| `core/` 内容 | kernel.c / bootseq.c / panic / ksym / splash / version / M0 原语 / misc_syscall | 目录清单 |
| 跨模块 include | 只能走 `include/<mod>.h`（syscall 子系统 11 条白名单见第 8 节） | `tools/check-deps.ps1` |
| 每模块文档 | `docs/modules/<mod>.md`（职责/接口/依赖/不变量/拒绝） | 目录检查 |
| ring3 程序位置 | `src/user/`（ulib、user_shell）；内核侧加载器在 `kernel/user/` | 目录检查 |
| 自检代码 | 各模块 `*_selftest.c`，不得进 `kernel.c` | 人工 |

## 5. 迁移清单（按风险从低到高）

| 步 | 动作 | 状态 |
|---|---|---|
| 0 | 本文 + `tools/check-deps.ps1` 基线 | ✅ |
| 1 | M2：`heap`、`object`、`user_layout` 迁入 `mm/` | ✅ |
| 2 | M7：`trust`、`cap`、`cpu` 迁入 `security/` | ✅ |
| 3 | M3：`block/`、`device/pci/acpi/rtc`、`fb/console`、`intc/` 迁入 `dev/` | ✅ |
| 4 | M6：`service`、`channel` 迁入 `svc/` | ✅ |
| 5 | M5：`net` 迁入 `net/`，`e1000` 迁入 `dev/net/` | ✅ |
| 6 | M1：`smp/percpu`、`idt/fault/trap`、`gdt` 迁入 `proc/` | ✅ |
| 7 | M8：内核侧加载器归 `kernel/user/`，用户态 ulib/shell 归 `src/user/` | ✅（程序注册表仍需再收口） |
| 8 | 拆 `syscall.c` 到各模块 `*_syscall.c` | ✅（978 → 75 行） |
| 9 | M6 `port` 并入 `svc/`，删除 `ipc/` | ✅（第 8 节） |

## 6. 迁移进展（截至 2026-10-24 21:5x）

| 项 | 迁移前 | 现在 |
|---|---|---|
| 内核目录 | core 45 文件混杂 | core 15 文件（启动骨架 + M0 原语 + misc_syscall 组） |
| 新模块目录 | — | `mm/ proc/ dev/{block,net,intc,char} net/ svc/ security/` |
| 跨模块 include 违规 | 137 | **0**（含 11 条显式白名单，见第 8 节） |
| 回归 | 94/94 | **97/97 PASS** |

已落地：

- **M2 内存**：`heap.c/h`、`object.c/h`、`user_layout.h` → `mm/`；
- **M7 安全**：`trust`、`cpu`、`cap` → `security/`；
- **M3 设备**：`device/pci/acpi/rtc` → `dev/`；`ata/block` → `dev/block/`；`apic/ioapic/madt` → `dev/intc/`；`e1000` → `dev/net/`；
- **M6 服务**：`service`、`service_selftest`、`channel`、`port` → `svc/`；
- **M1 进程**：`smp`、`percpu`、`gdt` → `proc/`；
- **M0**：`demo` → `apps/`，`kernel.c`、`bootseq.c`、`panic`、`ksym`、`version`、`splash` → `core/`；
- **接口层**：`include/` 十个模块头，跨模块 include 全部改走接口头；
- **转发头**：已全部删除（`FORWARD_H` 计数 0）。

## 7. syscall 拆分完成（2026-10-24 21:2x）

`proc/trap/syscall.c`：**978 → 约 100 行**（按换行符计；`Get-Content | Measure-Object -Line`
口径约 85 个非空行，旧记录 75 为不一致口径，不再使用），0 个 `case SYS_`，
只剩入口 + 沙箱检查 + 7 个委派 + `delegate_out`。

| 组 | 文件 | 归属模块 |
|---|---|---|
| net | `net/net_syscall.c` | M5 |
| fs | `fs/fs_syscall.c` | M4 |
| proc | `proc/proc_syscall.c` | M1 |
| mm | `mm/mm_syscall.c` | M2 |
| svc | `svc/svc_syscall.c` | M6 |
| security | `security/security_syscall.c` | M7 |
| misc | `core/misc_syscall.c` | M0（调试/杂项） |

规则：所有组统一 `ret = xxx_syscall(f); goto delegate_out;`，唯一出口执行
`f->eax = ret; proc_check_signals(f);`；沙箱拒绝仍是直接 `return`（与拆分前一致）。
共享辅助在 `proc/trap/syscall_util.c`（`sys_getchar_buffer`/`copy_user_string`/`fd_*` 等已导出，未复制实现）。
1..58 号调用全部有归属（58/58；58 号为 fs 会话的 rename，未含在本次提交）。

## 8. 架构收尾（2026-10-24 21:5x）

### 8.1 check-deps 重写与真实违规量

旧版 `check-deps.ps1` 只匹配 `../<mod>/` 与 `<mod>/` 两种写法，凡是 `../../<mod>/` 全部漏计。
本次改为「以包含文件所在目录解析相对路径，再回退 `-I src/kernel`」，`../`、`../../`、`include/...` 一视同仁。

按新规则复测，收尾前真实跨模块 include 共 **60 处**：

- 35 处为旧版报告（7 个新拆 `*_syscall.c` 直接 include `../proc/...`、`../fs/...` 等）；
- 25 处为旧版漏计：19 处在 `proc/trap/{native,irq,idt,uaccess,syscall_util}.c` 与
  `dev/{intc/apic,intc/ioapic,net/e1000}.c`，6 处是 `proc/trap/syscall.c` 对六个组头的 include。

处理结果：**49 处改为接口头（或删除冗余 include），11 处进入白名单，剩余 0 处**。

### 8.2 白名单（逐条，脚本与本节必须同步）

规则 A：`proc/trap/syscall_internal.h` 是 syscall 层内部共享辅助头（`copy_user_string`、
`make_abs_path`、`fd_alloc` 等），只允许 syscall 组文件 `*_syscall.c`（以及 `syscall.c`、
`syscall_util.c`）包含。它不是模块公开 API，不放进 `include/`。

| # | 包含方 | 被包含头 | 理由 |
|---|---|---|---|
| 1 | `core/misc_syscall.c` | `../proc/trap/syscall_internal.h` | syscall 子系统共享辅助（规则 A） |
| 2 | `fs/fs_syscall.c` | `../proc/trap/syscall_internal.h` | syscall 子系统共享辅助（规则 A） |
| 3 | `mm/mm_syscall.c` | `../proc/trap/syscall_internal.h` | syscall 子系统共享辅助（规则 A） |
| 4 | `net/net_syscall.c` | `../proc/trap/syscall_internal.h` | syscall 子系统共享辅助（规则 A） |
| 5 | `svc/svc_syscall.c` | `../proc/trap/syscall_internal.h` | syscall 子系统共享辅助（规则 A） |

规则 B：七个 `*_syscall.h` 是 syscall 子系统的**组分发接口**（`xxx_syscall_owns` /
`xxx_syscall`），由唯一分发器 `proc/trap/syscall.c` 包含；它们是 syscall 子系统的内部契约，
不是各模块对外的公开头。

| # | 包含方 | 被包含头 | 理由 |
|---|---|---|---|
| 6 | `proc/trap/syscall.c` | `../../net/net_syscall.h` | syscall 组分发（规则 B） |
| 7 | `proc/trap/syscall.c` | `../../fs/fs_syscall.h` | syscall 组分发（规则 B） |
| 8 | `proc/trap/syscall.c` | `../../mm/mm_syscall.h` | syscall 组分发（规则 B） |
| 9 | `proc/trap/syscall.c` | `../../svc/svc_syscall.h` | syscall 组分发（规则 B） |
| 10 | `proc/trap/syscall.c` | `../../security/security_syscall.h` | syscall 组分发（规则 B） |
| 11 | `proc/trap/syscall.c` | `../../core/misc_syscall.h` | syscall 组分发（规则 B） |

（`proc/proc_syscall.h` 属同模块 include，不占白名单。）

白名单之外的放行项只有：同模块 include、`include/<mod>.h`、`lib/`、M0 原语
（`core/error.h`、`core/atomic.h`、`core/spinlock.h`）。其余一律计违规。

### 8.3 ipc → svc 搬迁

- `src/kernel/ipc/port.c`、`port.h` → `src/kernel/svc/port.c`、`port.h`（`git mv`，保留历史）；
- `include/svc.h` 的 `#include "ipc/port.h"` 改为 `#include "svc/port.h"`；
- `proc/trap/native.c` 不再直接 include `../../ipc/port.h` / `../../svc/*.h`，统一走 `include/svc.h`；
- `ipc/` 目录已删除，无兼容空壳；`build.ps1` 按目录递归收集 `.c`，`svc/port.c` 自动进构建；
- 引用点复核：`git grep "ipc/"` 仅剩历史文档（`docs/driver-service.md`、`docs/status-2026-10-24.md`、
  `docs/error-paths.md` 的旧记号），以及 `fs/pipe.c`、`svc/channel.c` 的文件头历史注释。

### 8.4 接口头收口

- `include/*.h` 不再包含任何 `*_internal.h`（checker 专项检查为 0）；
- `include/proc.h` 新增 `proc_root_set()` 公开声明：`fs_syscall.c` 原先直接 include
  `proc/proc_internal.h` 只为这一个函数，现改为 `include/proc.h`，实现仍留在 `proc/process.c`；
- `fs/fs_syscall.c` 去掉重复的 `../dev/block/block.h`（`include/dev.h` 已包含）；
- `dev/intc/apic.c` 去掉重复的 `include/proc.h` 与冗余 `../../proc/smp.h`；
- `proc/trap/idt.c` 去掉冗余 `../../core/ksym.h`（`include/kernel.h` 已包含）。

### 8.5 死文件与遗留 TODO 清单

| 项 | 结论 |
|---|---|
| `TODO` / `FIXME` / `XXX`（`src/kernel`、`tools/`） | 0 处，无需清理 |
| 空的 `apps/*.h` | 不存在；6 个头均有内容，且由 `include/apps.h` 统一导出 |
| 零字节 / 仅 guard 的源文件 | 不存在 |
| 转发头（`FORWARD_H`） | 0 个（此前已全部删除） |
| `ipc/` 空目录 | 已删除 |
| 未被任何代码引用的 `.c` | 未发现；`build.ps1` 递归编译 `src/kernel/**/*.c` |
| 旧迁移脚本 `tools/converge-includes.ps1`、`fix-includes.ps1` | 一次性工具，现无调用点；保留作历史参考，不参与构建 |
| 文档遗留 | `docs/driver-service.md` 仍写 `ipc/channel.c`；`docs/error-paths.md` 仍以 `ipc/port.c` 记录审计（历史快照）。本次不改（非本任务文档范围），留待文档会话统一 |

### 8.6 验证

- `tools/check-deps.ps1`：**module include violations: 0**，whitelisted 11，interface layer 0；
- 连续两次 `tools\session-run.ps1 -Name arch -Test`：**97/97 PASS**（2026-10-24 21:2x，第二次接第一次无改动）；
- 构建：`-Wall -Wextra -Werror`，**0 warning**；
- 代码提交：`c3e6457`（[refactor] 模块边界收口，含 fs_syscall.c/h 的 include 收口）；
  fs 的 rename 功能随后由 `ae20490` 合入主线；
- 约束：未改 syscall 号 / ABI / 测试项 / `build.ps1`；未提交 `.obj-*`、`.dist-*`、
  `serial*.log`、`logs*.log`、`.session/`。

### 8.7 未完成 / 待并行会话

- `net/net.c`、`net/net.h`、`proc/process.c`、`proc/switch.asm`、`mm/vmm.c`、`src/user/*` 的未提交改动
  未触碰（属并行会话）；如它们合并后再出现新的跨模块 include，按同一 checker 规则处理；
- M8 程序注册表（`kernel/user/program.c`）与用户态目录的最终合并仍可继续收口，但不影响模块边界；
- 文档历史记号（`docs/driver-service.md` 的 `ipc/channel.c`、`docs/error-paths.md` 的 `ipc/port.c` 审计快照）
  仍未改；属其它文档范围，已在 8.5 记录。

### arch/x86 platform layer (option B, 2026-10-25)

- head.asm / io.c / io.h / linker.ld moved to src/kernel/arch/x86/
- types.h stays at src/kernel/types.h (option C not done: wide churn, low value)
- all includes repointed to "arch/x86/io.h" (17 files)
- build.ps1 linker path and Makefile asm wildcard updated

### include/types.h split (2026-10-25)

- include/types.h: fixed-width types, size_t/uintptr_t, address types, NULL
- include/macros.h: KiB/MiB/GiB, ARRAY_SIZE, ALIGN_*, MIN/MAX, UNUSED
- src/kernel/types.h is now a compatibility shim including both (66 existing includes unchanged)